// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_query_cache.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

#include "common/logging.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/query_cache/query_cache.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_query_helpers.h"
#include "video_core/renderer_d3d12/d3d12_transform_feedback.h"

namespace D3D12 {
using VideoCommon::QueryType;
namespace {
constexpr u32 BankSize = 256;
// Full banks rotate instead of waiting: a bank is consumed only when it comes around again,
// several submits later, or when the guest reads a counter.
constexpr u32 BankCount = 8;
constexpr u32 SlotCount = BankSize * BankCount;
constexpr u64 PipelineOffset = sizeof(u64);
constexpr u64 SOOffset = PipelineOffset + sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
constexpr u64 ResultStride = SOOffset + 4 * sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
constexpr size_t TypeCount = static_cast<size_t>(QueryType::MaxQueryTypes);
ComPtr<ID3D12QueryHeap> CreateHeap(Context& context, D3D12_QUERY_HEAP_TYPE type, u32 count) {
    if (BisectEnabled("NXBOX_D3D12_NO_QUERIES"))
        return {};
    D3D12_QUERY_HEAP_DESC desc{};
    desc.Type = type;
    desc.Count = count;
    ComPtr<ID3D12QueryHeap> heap;
    Check(context.Device()->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap)), "CreateQueryHeap",
          context.Device());
    return heap;
}
D3D12_QUERY_TYPE SOType(u32 stream) {
    return static_cast<D3D12_QUERY_TYPE>(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + stream);
}
class CounterStreamer final : public VideoCommon::SimpleStreamer<VideoCommon::GuestQuery> {
public:
    CounterStreamer(QueryType type, QueryCacheRuntime& runtime)
        : SimpleStreamer(static_cast<size_t>(type)), runtime{runtime}, type{type} {}
    void StartCounter() override {
        runtime.EnableCounter(type, true);
    }
    void PauseCounter() override {
        runtime.EnableCounter(type, false);
    }
    void CloseCounter() override {
        PauseCounter();
    }
    void ResetCounter() override {
        runtime.ResetCounter(type);
    }
    size_t WriteCounter(VAddr address, bool timestamp, u32,
                        std::optional<u32> stream = std::nullopt) override {
        const auto query =
            BuildQuery(timestamp, address, runtime.ReadCounter(type, stream.value_or(0)));
        {
            std::scoped_lock lock{pending_mutex};
            pending.push_back(query);
        }
        return query;
    }
    bool HasPendingSync() const override {
        std::scoped_lock lock{pending_mutex};
        return !pending.empty();
    }
    void SyncWrites() override {
        std::vector<VideoCommon::SyncValuesStruct> values;
        {
            std::scoped_lock lock{pending_mutex};
            for (const auto index : pending) {
                auto& query = slot_queries[index];
                using VideoCommon::QueryFlagBits;
                if (True(query.flags &
                         (QueryFlagBits::IsInvalidated | QueryFlagBits::IsRewritten))) {
                    continue;
                }
                values.push_back({query.guest_address, query.value,
                                  True(query.flags & QueryFlagBits::HasTimestamp) ? 8ULL : 4ULL});
                query.flags |= QueryFlagBits::IsHostSynced;
            }
            pending.clear();
        }
        runtime.SyncValues<VideoCommon::SyncValuesStruct>(values);
    }
    void Free(size_t index) override {
        std::scoped_lock lock{pending_mutex};
        std::erase(pending, index);
        SimpleStreamer::Free(index);
    }

private:
    QueryCacheRuntime& runtime;
    QueryType type;
    mutable std::mutex pending_mutex;
    std::vector<size_t> pending;
};
} // namespace

struct QueryCacheRuntimeImpl {
    QueryCacheRuntimeImpl(QueryCacheRuntime& runtime, Context& context,
                          Tegra::MaxwellDeviceMemoryManager& memory, BufferCache& cache,
                          TransformFeedback& feedback)
        : context{context}, memory{memory}, cache{cache}, feedback{feedback},
          upload{context, D3D12_HEAP_TYPE_UPLOAD}, readback{context, D3D12_HEAP_TYPE_READBACK},
          guest{0, runtime},
          occlusion{CreateHeap(context, D3D12_QUERY_HEAP_TYPE_OCCLUSION, SlotCount)},
          pipeline{CreateHeap(context, D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, SlotCount)},
          results{context.CreateBuffer(ResultStride * SlotCount, D3D12_HEAP_TYPE_READBACK)} {
        for (auto& heap : so) {
            heap = CreateHeap(context, D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, SlotCount);
        }
        for (const auto type :
             {QueryType::ZPassPixelCount, QueryType::ZPassPixelCount64,
              QueryType::VerticesGenerated, QueryType::PrimitivesGenerated,
              QueryType::VertexShaderInvocations, QueryType::GeometryShaderInvocations,
              QueryType::GeometryShaderPrimitivesGenerated, QueryType::ClipperInvocations,
              QueryType::ClipperPrimitivesGenerated, QueryType::PixelShaderInvocations,
              QueryType::TessellationInitInvocations, QueryType::TessellationShaderInvocations,
              QueryType::StreamingByteCount, QueryType::StreamingPrimitivesSucceeded,
              QueryType::StreamingPrimitivesNeeded, QueryType::VtgPrimitivesOut,
              QueryType::StreamingPrimitivesNeededMinusSucceeded,
              QueryType::TotalStreamingPrimitivesNeededMinusSucceeded}) {
            streamers[static_cast<size_t>(type)] = std::make_unique<CounterStreamer>(type, runtime);
        }
    }
    void RequireClosed() const {
        if (drawing) {
            throw std::logic_error("D3D12 query operation inside draw");
        }
    }
    void Collect() {
        RequireClosed();
        bool any = count != 0;
        for (const u32 used : bank_count)
            any |= used != 0;
        if (!any) {
            return;
        }
        context.Finish(FrameCount::ForcedSubmitQueryBank);
        for (u32 index = 0; index < BankCount; ++index) {
            if (index != bank && bank_count[index]) {
                Consume(index, bank_count[index]);
                bank_count[index] = 0;
            }
        }
        Consume(bank, count);
        count = 0; // Recycle only after completion and CPU consumption of every slot.
    }
    // A full bank is left for the GPU; the next one is reused once its own work has completed.
    void Rotate() {
        bank_count[bank] = count;
        bank_tick[bank] = context.CurrentTick();
        bank = (bank + 1) % BankCount;
        count = 0;
        if (bank_count[bank]) {
            context.Wait(bank_tick[bank], "query_bank");
            Consume(bank, bank_count[bank]);
            bank_count[bank] = 0;
        }
    }
    u32 Slot() const {
        return bank * BankSize + count;
    }
    void Consume(u32 index, u32 used) {
        for (u32 slot = index * BankSize; slot < index * BankSize + used; ++slot) {
            const auto* base = results->mapped.data() + slot * ResultStride;
            const auto add = [&](QueryType type, u64 value) {
                totals[static_cast<size_t>(type)][0] += value;
            };
            if (draws[slot].occlusion) {
                u64 samples{};
                std::memcpy(&samples, base, sizeof(samples));
                add(QueryType::ZPassPixelCount, samples);
                add(QueryType::ZPassPixelCount64, samples);
            }
            D3D12_QUERY_DATA_PIPELINE_STATISTICS p{};
            if (draws[slot].pipeline)
                std::memcpy(&p, base + PipelineOffset, sizeof(p));
            add(QueryType::VerticesGenerated, p.IAVertices);
            if (!(draws[slot].streams & 1U)) {
                add(QueryType::PrimitivesGenerated,
                    draws[slot].geometry ? p.GSPrimitives : p.IAPrimitives);
                add(QueryType::VtgPrimitivesOut,
                    draws[slot].geometry ? p.GSPrimitives : p.IAPrimitives);
            }
            add(QueryType::VertexShaderInvocations, p.VSInvocations);
            add(QueryType::GeometryShaderInvocations, p.GSInvocations);
            add(QueryType::GeometryShaderPrimitivesGenerated, p.GSPrimitives);
            add(QueryType::ClipperInvocations, p.CInvocations);
            add(QueryType::ClipperPrimitivesGenerated, p.CPrimitives);
            add(QueryType::PixelShaderInvocations, p.PSInvocations);
            add(QueryType::TessellationInitInvocations, p.HSInvocations);
            add(QueryType::TessellationShaderInvocations, p.DSInvocations);
            for (u32 stream = 0; stream < 4; ++stream) {
                if (!(draws[slot].streams & (1U << stream))) {
                    continue;
                }
                D3D12_QUERY_DATA_SO_STATISTICS stats{};
                std::memcpy(&stats, base + SOOffset + stream * sizeof(stats), sizeof(stats));
                if (stream == 0) {
                    add(QueryType::PrimitivesGenerated, stats.PrimitivesStorageNeeded);
                }
                totals[static_cast<size_t>(QueryType::VtgPrimitivesOut)][stream] +=
                    stats.PrimitivesStorageNeeded;
                totals[static_cast<size_t>(QueryType::StreamingPrimitivesSucceeded)][stream] +=
                    stats.NumPrimitivesWritten;
                totals[static_cast<size_t>(QueryType::StreamingPrimitivesNeeded)][stream] +=
                    stats.PrimitivesStorageNeeded;
                const auto overflow =
                    StreamOutputOverflow(stats.PrimitivesStorageNeeded, stats.NumPrimitivesWritten);
                totals[static_cast<size_t>(QueryType::StreamingPrimitivesNeededMinusSucceeded)]
                      [stream] += overflow;
                totals[static_cast<size_t>(QueryType::TotalStreamingPrimitivesNeededMinusSucceeded)]
                      [0] += overflow;
            }
        }
    }
    u64 ReadGuest(VideoCommon::LookupData object) {
        if (!object.address) {
            throw std::invalid_argument("Invalid conditional rendering address");
        }
        // SyncWrites has made query results visible to the buffer cache. Reading
        // through that cache also covers results produced by guest storage writes.
        auto result = readback.Request(8);
        {
            std::scoped_lock lock(cache.mutex);
            const auto [buffer, offset] = cache.ObtainCPUBuffer(
                object.address, 8, VideoCommon::ObtainBufferSynchronize::FullSynchronize,
                VideoCommon::ObtainBufferOperation::DoNothing);
            auto resource = buffer->Handle();
            context.Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
            context.Retain(result.buffer);
            context.MarkWork("query readback/copy");
            context.BeforeCopyWrite(result.buffer, result.offset, 8);
            context.Commands()->CopyBufferRegion(result.buffer->handle.Get(), result.offset,
                                                 resource->handle.Get(), offset, 8);
        }
        context.Finish(FrameCount::ForcedSubmitQueryReadback);
        u64 value{};
        std::memcpy(&value, result.mapped_span.data(), 8);
        return value;
    }
    Context& context;
    [[maybe_unused]] Tegra::MaxwellDeviceMemoryManager& memory;
    BufferCache& cache;
    TransformFeedback& feedback;
    StagingRing upload, readback;
    VideoCommon::GuestStreamer<QueryCacheParams> guest;
    std::array<std::unique_ptr<CounterStreamer>, TypeCount> streamers;
    ComPtr<ID3D12QueryHeap> occlusion, pipeline;
    std::array<ComPtr<ID3D12QueryHeap>, 4> so;
    ResourcePtr results, predicate;
    struct Draw {
        bool occlusion{}, geometry{}, pipeline{}, recorded{};
        u32 streams{};
    };
    std::array<Draw, SlotCount> draws{};
    std::array<u32, BankCount> bank_count{};
    std::array<u64, BankCount> bank_tick{};
    u32 bank{};
    std::array<std::array<u64, 4>, TypeCount> totals{}, baselines{};
    std::array<bool, TypeCount> enabled{};
    std::atomic<u64> pending_reports{}, completed_reports{}, invalidated_reports{}, report_copies{};
    u32 count{};
    bool drawing{}, predicate_set{};
    // Pipeline statistics are recorded only once the guest has read one of those counters.
    // Recording them on every draw filled the 256-slot bank ~14 times per frame in BotW, and
    // each full bank made the CPU wait for the GPU.
    bool pipeline_wanted{};
};

QueryCacheRuntime::QueryCacheRuntime(Context& context, Tegra::MaxwellDeviceMemoryManager& memory,
                                     BufferCache& cache, TransformFeedback& feedback)
    : impl{std::make_unique<QueryCacheRuntimeImpl>(*this, context, memory, cache, feedback)} {}
QueryCacheRuntime::~QueryCacheRuntime() {
    try {
        impl->context.Finish(FrameCount::ForcedSubmitQueryReadback);
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "D3D12 query teardown: {}", error.what());
    }
}
VideoCommon::StreamerInterface* QueryCacheRuntime::GetStreamerInterface(QueryType type) {
    if (type == QueryType::Payload) {
        return &impl->guest;
    }
    const auto index = static_cast<size_t>(type);
    return index < TypeCount ? impl->streamers[index].get() : nullptr;
}
void QueryCacheRuntime::ReportPending() { ++impl->pending_reports; }
void QueryCacheRuntime::ReportCompleted(bool invalidated) {
    --impl->pending_reports;
    if (invalidated)
        ++impl->invalidated_reports;
    else
        ++impl->completed_reports;
}
std::string QueryCacheRuntime::VisibilityDiagnostics() const {
    return " pending_guest_queries=" + std::to_string(impl->pending_reports.load()) +
           " guest_query_writes=" + std::to_string(impl->completed_reports.load()) +
           " guest_query_invalidations=" + std::to_string(impl->invalidated_reports.load()) +
           " query_buffer_copies=" + std::to_string(impl->report_copies.load()) +
           " pending_native_queries=" + std::to_string(impl->count);
}
void QueryCacheRuntime::PrepareDraw() {
    if (BisectEnabled("NXBOX_D3D12_NO_QUERIES"))
        return;
    impl->RequireClosed();
    if (impl->count == BankSize) {
        impl->Rotate();
    }
}
void QueryCacheRuntime::BeginDraw(bool geometry) {
    if (BisectEnabled("NXBOX_D3D12_NO_QUERIES"))
        return;
    impl->RequireClosed();
    if (impl->count == BankSize) {
        throw std::logic_error("PrepareDraw must precede draw state binding");
    }
    ResumeHostConditionalRendering();
    const u32 slot = impl->Slot();
    auto& draw = impl->draws[slot];
    draw = {impl->enabled[static_cast<size_t>(QueryType::ZPassPixelCount)] ||
                impl->enabled[static_cast<size_t>(QueryType::ZPassPixelCount64)],
            geometry, impl->pipeline_wanted, false, impl->feedback.StreamMask()};
    draw.recorded = draw.occlusion || draw.pipeline || draw.streams;
    impl->drawing = true;
    if (!draw.recorded)
        return;
    auto* commands = impl->context.Commands();
    impl->context.MarkWork("queries");
    if (draw.pipeline)
        commands->BeginQuery(impl->pipeline.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, slot);
    if (draw.occlusion) {
        commands->BeginQuery(impl->occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, slot);
    }
    for (u32 stream = 0; stream < 4; ++stream) {
        if (draw.streams & (1U << stream)) {
            commands->BeginQuery(impl->so[stream].Get(), SOType(stream), slot);
        }
    }
}
void QueryCacheRuntime::EndDraw() {
    if (BisectEnabled("NXBOX_D3D12_NO_QUERIES"))
        return;
    if (!impl->drawing) {
        throw std::logic_error("D3D12 EndDraw without BeginDraw");
    }
    const u32 slot = impl->Slot();
    const auto& draw = impl->draws[slot];
    if (!draw.recorded) {
        impl->drawing = false;
        return;
    }
    auto& context = impl->context;
    context.Retain(impl->results);
    // EndQuery and ResolveQueryData leave graphics state alone; keep the draw cache.
    auto* commands = context.Commands();
    const auto end = [&](ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, u64 offset) {
        commands->EndQuery(heap, type, slot);
        commands->ResolveQueryData(heap, type, slot, 1, impl->results->handle.Get(),
                                   slot * ResultStride + offset);
    };
    if (draw.pipeline)
        end(impl->pipeline.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, PipelineOffset);
    if (draw.occlusion) {
        end(impl->occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    }
    for (u32 stream = 0; stream < 4; ++stream) {
        if (draw.streams & (1U << stream)) {
            end(impl->so[stream].Get(), SOType(stream),
                SOOffset + stream * sizeof(D3D12_QUERY_DATA_SO_STATISTICS));
        }
    }
    ++impl->count;
    impl->drawing = false;
}
void QueryCacheRuntime::CancelDraw() {
    // A draw that threw after BeginDraw must still close its queries before
    // submitting. Keep any partial native statistics instead of leaking a bank.
    if (impl->drawing)
        EndDraw();
    PauseHostConditionalRendering();
}
u64 QueryCacheRuntime::ReadCounter(QueryType type, u32 stream) {
    if (BisectEnabled("NXBOX_D3D12_NO_QUERIES"))
        return 0;
    if (stream >= 4 || static_cast<size_t>(type) >= TypeCount) {
        throw std::out_of_range("Invalid D3D12 query counter");
    }
    if (type != QueryType::StreamingByteCount && type != QueryType::ZPassPixelCount &&
        type != QueryType::ZPassPixelCount64)
        impl->pipeline_wanted = true;
    impl->Collect();
    if (type == QueryType::StreamingByteCount) {
        return impl->feedback.ReadFilledSize(stream);
    }
    const auto index = static_cast<size_t>(type);
    return CounterDifference(impl->totals[index][stream], impl->baselines[index][stream]);
}
void QueryCacheRuntime::ResetCounter(QueryType type) {
    impl->Collect();
    const auto index = static_cast<size_t>(type);
    if (index >= TypeCount) {
        throw std::out_of_range("Invalid D3D12 query reset");
    }
    impl->baselines[index] = impl->totals[index];
}
void QueryCacheRuntime::EnableCounter(QueryType type, bool enabled) {
    impl->RequireClosed();
    const auto index = static_cast<size_t>(type);
    if (index >= TypeCount) {
        throw std::out_of_range("Invalid D3D12 query enable");
    }
    impl->enabled[index] = enabled;
}
void QueryCacheRuntime::WriteValue(DAddr address, const void* value, u64 size) {
    if (size != 4 && size != 8) {
        throw std::invalid_argument("Invalid guest query write size");
    }
    impl->context.Profile().Count(FrameCount::BufferUploadBytes, size);
    impl->context.Profile().Count(FrameCount::OtherUploadBytes, size);
    auto upload = impl->upload.Request(static_cast<size_t>(size));
    std::memcpy(upload.mapped_span.data(), value, static_cast<size_t>(size));
    CopyValue(address, upload.buffer, upload.offset, size);
}
void QueryCacheRuntime::CopyValue(DAddr address, const ResourcePtr& source, u64 offset, u64 size) {
    impl->RequireClosed();
    if (!source || (size != 4 && size != 8) || offset > source->handle->GetDesc().Width ||
        size > source->handle->GetDesc().Width - offset) {
        throw std::invalid_argument("Invalid guest query copy");
    }
    PauseHostConditionalRendering();
    std::scoped_lock lock(impl->cache.mutex);
    const auto [buffer, destination] = impl->cache.ObtainCPUBuffer(
        address, static_cast<u32>(size), VideoCommon::ObtainBufferSynchronize::FullSynchronize,
        VideoCommon::ObtainBufferOperation::MarkAsWritten);
    auto resource = buffer->Handle();
    impl->context.Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE);
    impl->context.Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST);
    impl->context.Profile().Count(source->heap == D3D12_HEAP_TYPE_UPLOAD
                                      ? FrameCount::StagingCopyBytes
                                      : FrameCount::GpuBufferCopyBytes,
                                  size);
    ++impl->report_copies;
    impl->context.MarkWork("query report/predicate copy");
    impl->context.BeforeCopyWrite(resource, destination, size);
    impl->context.Commands()->CopyBufferRegion(resource->handle.Get(), destination,
                                               source->handle.Get(), offset, size);
}
void QueryCacheRuntime::Barriers(bool) {
    impl->RequireClosed(); // Explicit resource transitions order every resolve/copy.
}
void QueryCacheRuntime::PauseHostConditionalRendering() {
    if (BisectEnabled("NXBOX_D3D12_NO_QUERIES"))
        return;
    // Predication is always recorded and does not alter any cached draw state.
    impl->context.Commands()->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
}
void QueryCacheRuntime::EndHostConditionalRendering() {
    PauseHostConditionalRendering();
    impl->predicate_set = false;
    impl->predicate.reset();
}
void QueryCacheRuntime::ResumeHostConditionalRendering() {
    if (BisectEnabled("NXBOX_D3D12_NO_QUERIES"))
        return;
    if (impl->predicate_set) {
        impl->context.Transition(impl->predicate, D3D12_RESOURCE_STATE_PREDICATION);
        // D3D12 EQUAL_ZERO skips commands when zero, so a one means execute.
        impl->context.Commands()->SetPredication(impl->predicate->handle.Get(), 0,
                                                 D3D12_PREDICATION_OP_EQUAL_ZERO);
    }
}
bool QueryCacheRuntime::HostConditionalRenderingCompareValue(VideoCommon::LookupData object, bool) {
    return CompareConditional(object, {0, nullptr}, false);
}
bool QueryCacheRuntime::HostConditionalRenderingCompareValues(VideoCommon::LookupData first,
                                                              VideoCommon::LookupData second, bool,
                                                              bool equal) {
    if (!second.address) {
        EndHostConditionalRendering();
        return false;
    }
    return CompareConditional(first, second, equal);
}
bool QueryCacheRuntime::CompareConditional(VideoCommon::LookupData first,
                                           VideoCommon::LookupData second, bool equal) {
    if (BisectEnabled("NXBOX_D3D12_NO_QUERIES"))
        return false;
    impl->RequireClosed();
    if (!first.address) {
        EndHostConditionalRendering();
        return false;
    }
    PauseHostConditionalRendering();
    impl->guest.SyncWrites();
    for (auto& streamer : impl->streamers) {
        if (streamer) {
            streamer->SyncWrites();
        }
    }
    const auto a = impl->ReadGuest(first);
    const auto b = second.address ? impl->ReadGuest(second) : 0;
    const u64 predicate = (a == b) == equal;
    impl->context.Profile().Count(FrameCount::BufferUploadBytes, 8);
    impl->context.Profile().Count(FrameCount::OtherUploadBytes, 8);
    impl->context.Profile().Count(FrameCount::StagingCopyBytes, 8);
    auto upload = impl->upload.Request(8);
    std::memcpy(upload.mapped_span.data(), &predicate, 8);
    // Fresh storage: a previous SetPredication may still reference the old value.
    impl->predicate = impl->context.CreateBuffer(8, D3D12_HEAP_TYPE_DEFAULT);
    impl->context.Transition(impl->predicate, D3D12_RESOURCE_STATE_COPY_DEST);
    impl->context.Retain(upload.buffer);
    impl->context.MarkWork("query report/predicate copy");
    impl->context.BeforeCopyWrite(impl->predicate, 0, 8);
    impl->context.Commands()->CopyBufferRegion(impl->predicate->handle.Get(), 0,
                                               upload.buffer->handle.Get(), upload.offset, 8);
    impl->predicate_set = true;
    ResumeHostConditionalRendering();
    return true;
}
QueryCacheRuntime::Timestamp QueryCacheRuntime::CaptureTimestamp() {
    if (BisectEnabled("NXBOX_D3D12_NO_QUERIES"))
        return {};
    impl->context.MarkWork("timestamp query");
    impl->RequireClosed();
    auto heap = CreateHeap(impl->context, D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1);
    Timestamp timestamp{impl->readback.Request(8), impl->context.CurrentTick(), 0};
    Check(impl->context.Queue()->GetTimestampFrequency(&timestamp.frequency),
          "GetTimestampFrequency", impl->context.Device());
    auto* commands = impl->context.Commands();
    commands->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    impl->context.Retain(timestamp.result.buffer);
    commands->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 1,
                               timestamp.result.buffer->handle.Get(), timestamp.result.offset);
    impl->context.AfterCompletion([heap = std::move(heap)] {});
    return timestamp;
}
u64 QueryCacheRuntime::ReadTimestamp(const Timestamp& timestamp) {
    if (BisectEnabled("NXBOX_D3D12_NO_QUERIES"))
        return 0;
    impl->RequireClosed();
    if (!timestamp.result.buffer || timestamp.result.mapped_span.size() < 8) {
        throw std::invalid_argument("Invalid D3D12 timestamp");
    }
    impl->context.Wait(timestamp.tick);
    u64 ticks{};
    std::memcpy(&ticks, timestamp.result.mapped_span.data(), 8);
    return TimestampNanoseconds(ticks, timestamp.frequency);
}
} // namespace D3D12
namespace VideoCommon {
template class QueryCacheBase<D3D12::QueryCacheParams>;
} // namespace VideoCommon
