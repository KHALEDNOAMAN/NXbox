// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_query_cache.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <unordered_map>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/query_cache/query_cache.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_query_helpers.h"
#include "video_core/renderer_d3d12/d3d12_query_resolve.h"
#include "video_core/renderer_d3d12/d3d12_shader.h"
#include "video_core/renderer_d3d12/d3d12_transform_feedback.h"
#include "video_core/submission_trace.h"

namespace D3D12 {
using VideoCommon::QueryType;
namespace {
constexpr u32 BankSize = 256;
// Full banks rotate instead of waiting: a bank is consumed only when it comes around again,
// several submits later, or when the guest reads a counter. BotW records ~3600 occlusion
// queries per frame: 8 banks wrapped inside one command list and the render thread waited
// ~11 ms per frame for the GPU. 64 banks span several frames.
constexpr u32 BankCount = 64;
constexpr u32 SlotCount = BankSize * BankCount;
constexpr u64 PipelineOffset = sizeof(u64);
constexpr u64 SOOffset = PipelineOffset + sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
constexpr u64 ResultStride = SOOffset + 4 * sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
constexpr size_t TypeCount = static_cast<size_t>(QueryType::MaxQueryTypes);
constexpr u64 CounterBytes = TypeCount * 4 * sizeof(u64);
// Not tied to GPU accuracy: the default (High) would keep every report on the per-report
// submit-and-wait path, as Persona 5 Royal showed (641 forced submits per frame).
bool SynchronousQueries() {
    return BisectEnabled("NXBOX_D3D12_SYNC_QUERIES");
}
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
        VideoCommon::SubmissionTrace::Log("query_report_begin cpu={:x} type={}", address,
                                          static_cast<u32>(type));
        const auto counter = runtime.ReadCounter(type, stream.value_or(0));
        VideoCommon::SubmissionTrace::Log("query_report_end cpu={:x} type={} value={} pending={}",
                                          address, static_cast<u32>(type), counter.value,
                                          static_cast<bool>(counter.result.buffer));
        const auto query = BuildQuery(timestamp, address, counter.value);
        if (counter.result.buffer) {
            auto& entry = slot_queries[query];
            entry.flags = VideoCommon::QueryFlagBits::IsHostManaged |
                          (timestamp ? VideoCommon::QueryFlagBits::HasTimestamp
                                     : VideoCommon::QueryFlagBits{});
            std::scoped_lock lock{pending_mutex};
            snapshots.emplace(query, counter.result);
            if (VideoCommon::SubmissionTrace::Active())
                trace_frames.emplace(query, VideoCommon::SubmissionTrace::frame.load());
            unsynced.push_back(query);
            return query;
        }
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
    bool HasUnsyncedQueries() const override {
        std::scoped_lock lock{pending_mutex};
        return !unsynced.empty();
    }
    void PushUnsyncedQueries() override {
        std::scoped_lock lock{pending_mutex};
        batches.push_back(std::move(unsynced));
        unsynced.clear();
    }
    void PopUnsyncedQueries() override {
        // FenceManager calls this only after the batch's native fence completes.
        // No command recording or wait is allowed on its completion thread.
        // `guard` keeps BuildQuery on the GPU thread from growing slot_queries meanwhile.
        // Lock order matches Free(): pending_mutex, then guard.
        std::scoped_lock lock{pending_mutex};
        std::scoped_lock slots{guard};
        for (const auto index : batches.front()) {
            const auto snapshot = snapshots.find(index);
            if (snapshot == snapshots.end())
                continue; // Freed before its fence completed.
            auto& query = slot_queries[index];
            std::memcpy(&query.value, snapshot->second.mapped_span.data(), sizeof(u64));
            query.flags |= VideoCommon::QueryFlagBits::IsFinalValueSynced;
            if (const auto trace = trace_frames.find(index); trace != trace_frames.end()) {
                Diagnostic(fmt::format("D3D12_QUERY_RESOLVED frame={} cpu={:x} value={}",
                                       trace->second, query.guest_address, query.value));
                trace_frames.erase(trace);
            }
        }
        batches.pop_front();
    }
    void Free(size_t index) override {
        std::scoped_lock lock{pending_mutex};
        std::erase(pending, index);
        snapshots.erase(index);
        trace_frames.erase(index);
        SimpleStreamer::Free(index);
    }

private:
    std::unordered_map<size_t, u64> trace_frames;
    QueryCacheRuntime& runtime;
    QueryType type;
    mutable std::mutex pending_mutex;
    std::vector<size_t> pending, unsynced;
    std::deque<std::vector<size_t>> batches;
    std::unordered_map<size_t, StagingBufferRef> snapshots;
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
          results{context.CreateBuffer(ResultStride * SlotCount, synchronous
                                                                     ? D3D12_HEAP_TYPE_READBACK
                                                                     : D3D12_HEAP_TYPE_DEFAULT)} {
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
    void EnsureGpuCounters() {
        if (gpu_counters)
            return;
        std::string shader;
        const auto define = [&](const char* name, u64 value) {
            shader += "#define " + std::string{name} + " " + std::to_string(value) + "\n";
        };
        define("TYPE_COUNT", TypeCount);
        define("RESULT_STRIDE", ResultStride);
        define("SO_OFFSET", SOOffset);
        define("ZPassPixelCount", static_cast<u32>(QueryType::ZPassPixelCount));
        define("ZPassPixelCount64", static_cast<u32>(QueryType::ZPassPixelCount64));
        define("StreamingPrimitivesSucceeded",
               static_cast<u32>(QueryType::StreamingPrimitivesSucceeded));
        define("StreamingPrimitivesNeeded", static_cast<u32>(QueryType::StreamingPrimitivesNeeded));
        define("StreamingPrimitivesNeededMinusSucceeded",
               static_cast<u32>(QueryType::StreamingPrimitivesNeededMinusSucceeded));
        define("TotalStreamingPrimitivesNeededMinusSucceeded",
               static_cast<u32>(QueryType::TotalStreamingPrimitivesNeededMinusSucceeded));
        define("PrimitivesGenerated", static_cast<u32>(QueryType::PrimitivesGenerated));
        define("VtgPrimitivesOut", static_cast<u32>(QueryType::VtgPrimitivesOut));
        define("VerticesGenerated", static_cast<u32>(QueryType::VerticesGenerated));
        define("VertexShaderInvocations", static_cast<u32>(QueryType::VertexShaderInvocations));
        define("GeometryShaderInvocations", static_cast<u32>(QueryType::GeometryShaderInvocations));
        define("GeometryShaderPrimitivesGenerated",
               static_cast<u32>(QueryType::GeometryShaderPrimitivesGenerated));
        define("ClipperInvocations", static_cast<u32>(QueryType::ClipperInvocations));
        define("ClipperPrimitivesGenerated",
               static_cast<u32>(QueryType::ClipperPrimitivesGenerated));
        define("PixelShaderInvocations", static_cast<u32>(QueryType::PixelShaderInvocations));
        define("TessellationInitInvocations",
               static_cast<u32>(QueryType::TessellationInitInvocations));
        define("TessellationShaderInvocations",
               static_cast<u32>(QueryType::TessellationShaderInvocations));
        shader += QueryResolveShader;
        std::array<D3D12_ROOT_PARAMETER, 5> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0, 0, 5};
        for (u32 i = 1; i < parameters.size(); ++i) {
            parameters[i].ParameterType =
                i < 3 ? D3D12_ROOT_PARAMETER_TYPE_SRV : D3D12_ROOT_PARAMETER_TYPE_UAV;
            parameters[i].Descriptor = {(i - 1) % 2, 0};
        }
        D3D12_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.NumParameters = static_cast<UINT>(parameters.size());
        root_desc.pParameters = parameters.data();
        ComPtr<ID3DBlob> blob, errors;
        Check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors),
              "serialize query resolve root", context.Device());
        Check(context.Device()->CreateRootSignature(
                  0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&resolve_root)),
              "create query resolve root", context.Device());
        ShaderCompiler compiler;
        const auto code = compiler.Compile(shader, L"main", L"cs_6_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = resolve_root.Get();
        desc.CS = {code.data(), code.size()};
        Check(context.Device()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&resolve_pipeline)),
              "create query resolve pipeline", context.Device());
        gpu_counters = context.CreateBuffer(CounterBytes * 2, D3D12_HEAP_TYPE_DEFAULT);
        gpu_report = context.CreateBuffer(8, D3D12_HEAP_TYPE_DEFAULT);
        auto zero = upload.Request(CounterBytes * 2);
        std::memset(zero.mapped_span.data(), 0, CounterBytes * 2);
        context.Commands()->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        context.Transition(gpu_counters, D3D12_RESOURCE_STATE_COPY_DEST);
        context.Retain(zero.buffer);
        context.MarkWork("query accumulator initialization");
        context.BeforeCopyWrite(gpu_counters, 0, CounterBytes * 2);
        context.Commands()->CopyBufferRegion(gpu_counters->handle.Get(), 0,
                                             zero.buffer->handle.Get(), zero.offset,
                                             CounterBytes * 2);
    }
    void DispatchResolve(u32 operation, u32 type = 0, u32 stream = 0,
                         const StagingBufferRef& metadata = {}) {
        EnsureGpuCounters();
        context.Commands()->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        context.InvalidateRecordedState();
        context.Transition(results, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.Transition(gpu_counters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.Transition(gpu_report, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.Retain(results);
        context.Retain(gpu_counters);
        context.Retain(gpu_report);
        if (metadata.buffer)
            context.Retain(metadata.buffer);
        context.GlobalUavBarrier();
        auto* commands = context.Commands();
        commands->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        commands->SetComputeRootSignature(resolve_root.Get());
        commands->SetPipelineState(resolve_pipeline.Get());
        const std::array constants{operation, bank * BankSize + accumulated, count - accumulated,
                                   type, stream};
        commands->SetComputeRoot32BitConstants(0, 5, constants.data(), 0);
        commands->SetComputeRootShaderResourceView(1, results->handle->GetGPUVirtualAddress());
        // Reset/report do not read metadata, but still bind a valid buffer address.
        commands->SetComputeRootShaderResourceView(
            2, metadata.buffer ? metadata.buffer->handle->GetGPUVirtualAddress() + metadata.offset
                               : results->handle->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(3,
                                                    gpu_counters->handle->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(4, gpu_report->handle->GetGPUVirtualAddress());
        context.MarkWork("query GPU reduction");
        commands->Dispatch(static_cast<UINT>((TypeCount * 4 + 31) / 32), 1, 1);
        context.Profile().Count(FrameCount::Dispatches);
        context.GlobalUavBarrier();
    }
    void AccumulateGpu() {
        RequireClosed();
        EnsureGpuCounters();
        if (accumulated == count)
            return;
        auto metadata = upload.Request((count - accumulated) * 4 * sizeof(u32));
        for (u32 i = accumulated; i < count; ++i) {
            const auto& draw = draws[bank * BankSize + i];
            const std::array<u32, 4> flags{draw.occlusion, draw.geometry, draw.pipeline,
                                           draw.streams};
            std::memcpy(metadata.mapped_span.data() + (i - accumulated) * sizeof(flags),
                        flags.data(), sizeof(flags));
        }
        DispatchResolve(0, 0, 0, metadata);
        accumulated = count;
    }
    void Collect() {
        RequireClosed();
        bool any = count != 0;
        for (const u32 used : bank_count)
            any |= used != 0;
        if (!any) {
            return;
        }
        VideoCommon::SubmissionTrace::Log("query_bank_finish_begin");
        context.Finish(FrameCount::ForcedSubmitQueryBank);
        VideoCommon::SubmissionTrace::Log("query_bank_finish_end");
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
        if (!synchronous) {
            AccumulateGpu();
            bank = (bank + 1) % BankCount;
            count = 0;
            accumulated = 0;
            return; // Resolve, reduction and reuse are ordered on the same queue.
        }
        bank_count[bank] = count;
        bank_tick[bank] = context.CurrentTick();
        bank = (bank + 1) % BankCount;
        count = 0;
        if (bank_count[bank]) {
            VideoCommon::SubmissionTrace::Log("query_bank_wait_begin tick={}", bank_tick[bank]);
            context.Wait(bank_tick[bank], "query_bank");
            VideoCommon::SubmissionTrace::Log("query_bank_wait_end tick={}", bank_tick[bank]);
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
        VideoCommon::SubmissionTrace::Log("query_readback_wait_begin cpu={:x}", object.address);
        context.Finish(FrameCount::ForcedSubmitQueryReadback);
        VideoCommon::SubmissionTrace::Log("query_readback_wait_end cpu={:x}", object.address);
        u64 value{};
        std::memcpy(&value, result.mapped_span.data(), 8);
        return value;
    }
    const bool synchronous{SynchronousQueries()};
    Context& context;
    Tegra::MaxwellDeviceMemoryManager& memory;
    BufferCache& cache;
    TransformFeedback& feedback;
    StagingRing upload, readback;
    VideoCommon::GuestStreamer<QueryCacheParams> guest;
    std::array<std::unique_ptr<CounterStreamer>, TypeCount> streamers;
    ComPtr<ID3D12QueryHeap> occlusion, pipeline;
    std::array<ComPtr<ID3D12QueryHeap>, 4> so;
    ResourcePtr results, predicate, gpu_counters, gpu_report;
    ComPtr<ID3D12RootSignature> resolve_root;
    ComPtr<ID3D12PipelineState> resolve_pipeline;
    u32 accumulated{};
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
    // EQUAL_ZERO skips commands when the predicate is zero; NOT_EQUAL_ZERO inverts that.
    D3D12_PREDICATION_OP predicate_op{D3D12_PREDICATION_OP_EQUAL_ZERO};
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
void QueryCacheRuntime::ReportQueryFenceWait() {
    impl->context.Profile().Count(FrameCount::QueryReportFenceWaits);
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
    if (!impl->synchronous)
        context.Transition(impl->results, D3D12_RESOURCE_STATE_COPY_DEST);
    // Nearly every draw records a query, so the cache has to survive this point or it never skips
    // a call (redundant_state_skips was 0 in production). Everything stays valid except the
    // stream-output targets: forgetting them makes the next draw re-issue SOSetTargets, which
    // vkd3d-proton implements as end_current_render_pass(). Under Wine, draws that share a render
    // pass with the previous query draw render stretched geometry in SM3DW 1-1 and Odyssey;
    // bisected, re-recording any other category does not help and neither does predicating on
    // the CPU. The cause of the sharing problem is still open; drop this line when it is found.
    context.DrawState().stream_output = {};
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
QueryCacheRuntime::CounterResult QueryCacheRuntime::ReadCounter(QueryType type, u32 stream) {
    if (BisectEnabled("NXBOX_D3D12_NO_QUERIES"))
        return {};
    impl->RequireClosed();
    if (stream >= 4 || static_cast<size_t>(type) >= TypeCount) {
        throw std::out_of_range("Invalid D3D12 query counter");
    }
    if (type != QueryType::StreamingByteCount && type != QueryType::ZPassPixelCount &&
        type != QueryType::ZPassPixelCount64)
        impl->pipeline_wanted = true;
    if (impl->synchronous) {
        impl->Collect();
        if (type == QueryType::StreamingByteCount)
            return {impl->feedback.ReadFilledSize(stream), {}};
        const auto index = static_cast<size_t>(type);
        return {CounterDifference(impl->totals[index][stream], impl->baselines[index][stream]), {}};
    }
    PauseHostConditionalRendering();
    impl->AccumulateGpu();
    impl->DispatchResolve(1, static_cast<u32>(type), stream);
    if (type == QueryType::StreamingByteCount) {
        // The byte counter is the SO cursor, not a sum of primitive statistics.
        impl->feedback.CopyFilledSize(stream, impl->gpu_report);
    }
    auto result = impl->readback.Request(8);
    auto& context = impl->context;
    context.Transition(impl->gpu_report, D3D12_RESOURCE_STATE_COPY_SOURCE);
    context.MarkWork("query snapshot copy");
    context.BeforeCopyWrite(result.buffer, result.offset, 8);
    context.Commands()->CopyBufferRegion(result.buffer->handle.Get(), result.offset,
                                         impl->gpu_report->handle.Get(), 0, 8);
    context.Profile().Count(FrameCount::GpuResolvedQueryReports);
    return {0, result};
}
void QueryCacheRuntime::ResetCounter(QueryType type) {
    if (BisectEnabled("NXBOX_D3D12_NO_QUERIES"))
        return;
    impl->RequireClosed();
    const auto index = static_cast<size_t>(type);
    if (index >= TypeCount) {
        throw std::out_of_range("Invalid D3D12 query reset");
    }
    if (impl->synchronous) {
        impl->Collect();
        impl->baselines[index] = impl->totals[index];
        return;
    }
    PauseHostConditionalRendering();
    impl->AccumulateGpu();
    impl->DispatchResolve(2, static_cast<u32>(type), 0);
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
                                                 impl->predicate_op);
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
    VideoCommon::SubmissionTrace::Log("predicate_compare first_cpu={:x} second_cpu={:x} equal={}",
                                      first.address, second.address, equal);
    impl->RequireClosed();
    if (!first.address) {
        EndHostConditionalRendering();
        return false;
    }
    // Host-managed reports live in readback snapshots, not in the guest GPU buffer.
    // Let Maxwell read guest memory through the query cache, draining the batch once
    // if necessary, rather than predicate on an old buffer-cache copy.
    const auto host_managed = [](VideoCommon::LookupData object) {
        return object.found_query &&
               True(object.found_query->flags & VideoCommon::QueryFlagBits::IsHostManaged);
    };
    if (host_managed(first) || host_managed(second)) {
        VideoCommon::SubmissionTrace::Log("predicate_fallback host_managed=1");
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
    if (!BisectEnabled("NXBOX_D3D12_CPU_PREDICATE")) {
        if (!second.address) {
            // "Render when the value is non-zero" is exactly EQUAL_ZERO predication. Reading it
            // back on the CPU forced a submit and a fence wait per conditional draw.
            SetGpuPredicate(first.address, D3D12_PREDICATION_OP_EQUAL_ZERO);
            return true;
        }
        // Two reports, as in the Vulkan backend: outside high GPU accuracy only a known-zero
        // operand is resolved on the GPU; anything else renders unconditionally instead of
        // reading both values back (~260 submits and fence waits per frame in MK8 Deluxe).
        const std::array<VideoCommon::LookupData*, 2> objects{&first, &second};
        std::array<bool, 2> in_cache{};
        bool any_gpu_written{};
        {
            std::scoped_lock lock(impl->cache.mutex);
            for (size_t i = 0; i < 2; ++i) {
                const bool gpu_written = !objects[i]->found_query &&
                                         impl->cache.IsRegionGpuModified(objects[i]->address, 8);
                any_gpu_written |= gpu_written;
                in_cache[i] = objects[i]->found_query || gpu_written;
            }
        }
        if (!in_cache[0] && !in_cache[1]) {
            EndHostConditionalRendering();
            return false;
        }
        for (size_t i = 0; i < 2; ++i) {
            if (in_cache[i])
                continue;
            const u8* const pointer = impl->memory.GetPointer<u8>(objects[i]->address);
            u64 value{};
            if (pointer)
                std::memcpy(&value, pointer, sizeof(value));
            if (value != 0)
                continue;
            // other == 0 renders when the other value is zero, so NOT_EQUAL_ZERO skips it.
            SetGpuPredicate(objects[(i + 1) % 2]->address,
                            equal ? D3D12_PREDICATION_OP_NOT_EQUAL_ZERO
                                  : D3D12_PREDICATION_OP_EQUAL_ZERO);
            return true;
        }
        if (!Settings::IsGPULevelHigh() || !any_gpu_written) {
            EndHostConditionalRendering();
            return true;
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
    impl->predicate_op = D3D12_PREDICATION_OP_EQUAL_ZERO;
    ResumeHostConditionalRendering();
    return true;
}
void QueryCacheRuntime::SetGpuPredicate(DAddr address, D3D12_PREDICATION_OP op) {
    // Fresh storage: a previous SetPredication may still reference the old value.
    impl->predicate = impl->context.CreateBuffer(8, D3D12_HEAP_TYPE_DEFAULT);
    {
        std::scoped_lock lock(impl->cache.mutex);
        const auto [buffer, offset] = impl->cache.ObtainCPUBuffer(
            address, 8, VideoCommon::ObtainBufferSynchronize::FullSynchronize,
            VideoCommon::ObtainBufferOperation::DoNothing);
        auto resource = buffer->Handle();
        impl->context.Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
        impl->context.Transition(impl->predicate, D3D12_RESOURCE_STATE_COPY_DEST);
        impl->context.MarkWork("query report/predicate copy");
        impl->context.BeforeCopyWrite(impl->predicate, 0, 8);
        impl->context.Commands()->CopyBufferRegion(impl->predicate->handle.Get(), 0,
                                                   resource->handle.Get(), offset, 8);
    }
    impl->predicate_set = true;
    impl->predicate_op = op;
    ResumeHostConditionalRendering();
}
StagingBufferRef QueryCacheRuntime::PreparePredicateTrace() {
    if (!VideoCommon::SubmissionTrace::Active() || !impl->predicate_set)
        return {};
    // Allocation can submit under memory/staging pressure. Do it before binding
    // draw state and before opening native queries.
    impl->RequireClosed();
    return impl->readback.Request(8);
}
// Observe the same predicate bytes the following draw consumes, without forcing a
// CPU decision or a fence wait. Copies themselves must execute unconditionally.
void QueryCacheRuntime::TracePredicate(u64 frame, u64 pass, u64 draw,
                                       const StagingBufferRef& result) {
    if (!VideoCommon::SubmissionTrace::Active())
        return;
    if (!impl->predicate_set) {
        VideoCommon::SubmissionTrace::Log("host_predicate draw={} pass={} active=0", draw, pass);
        return;
    }
    const auto op = impl->predicate_op;
    PauseHostConditionalRendering();
    impl->context.Transition(impl->predicate, D3D12_RESOURCE_STATE_COPY_SOURCE);
    impl->context.Retain(result.buffer);
    impl->context.BeforeCopyWrite(result.buffer, result.offset, 8);
    impl->context.Commands()->CopyBufferRegion(result.buffer->handle.Get(), result.offset,
                                               impl->predicate->handle.Get(), 0, 8);
    ResumeHostConditionalRendering();
    impl->context.AfterCompletion([result, op, frame, pass, draw] {
        u64 value{};
        std::memcpy(&value, result.mapped_span.data(), 8);
        Diagnostic(
            fmt::format("D3D12_PREDICATE frame={} pass={} draw={} value={} op={} executes={}",
                        frame, pass, draw, value, static_cast<u32>(op),
                        op == D3D12_PREDICATION_OP_EQUAL_ZERO ? value != 0 : value == 0));
    });
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
