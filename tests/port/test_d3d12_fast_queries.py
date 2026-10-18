# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the production counter snapshot path with ordered command-list mocks."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


def function_body(source, signature):
    start = source.index("{", source.index(signature))
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start + 1 : end - 1]


class FastQueryTests(unittest.TestCase):
    def test_ordered_total_and_baseline_snapshots(self):
        source = (D3D12 / "d3d12_query_cache.cpp").read_text()
        read = function_body(source, "QueryCacheRuntime::ReadCounter(")
        driver = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>
using u32=std::uint32_t; using u64=std::uint64_t;
enum class QueryType { ZPassPixelCount, ZPassPixelCount64, Pipeline, StreamingByteCount };
constexpr size_t TypeCount=4;
constexpr u64 CounterBytes=TypeCount*4*sizeof(u64);
constexpr int D3D12_RESOURCE_STATE_COPY_SOURCE=1;
enum class FrameCount { FastQueryReports, GpuResolvedQueryReports };
enum class FramePhase { QueryReadCounter };
struct Profile { void Count(FrameCount) {} };
struct FrameProfile { struct Scope { Scope(::Profile&,FramePhase) {} }; };
bool disabled{};
bool BisectEnabled(const char*) { return disabled; }
u64 CounterDifference(u64 value,u64 baseline) { return value>=baseline ? value-baseline : 0; }
struct Handle {
    std::array<u64,64> data{};
    Handle* Get() { return this; }
};
struct Resource { Handle handle; };
using ResourcePtr=std::shared_ptr<Resource>;
struct StagingBufferRef { ResourcePtr buffer; size_t offset{}; std::span<unsigned char> mapped_span; };
struct Ring {
    StagingBufferRef Request(size_t size) {
        auto buffer=std::make_shared<Resource>();
        return {buffer, 16, {reinterpret_cast<unsigned char*>(buffer->handle.data.data())+16,size}};
    }
};
struct Commands {
    int copies{};
    void CopyBufferRegion(Handle* dest,size_t offset,Handle* src,size_t source,size_t size) {
        ++copies;
        std::memcpy(reinterpret_cast<unsigned char*>(dest->data.data())+offset,
                    reinterpret_cast<unsigned char*>(src->data.data())+source,size);
    }
};
struct Context {
    ::Profile profile; ::Commands commands;
    bool transitioned{}, retained{}, write_checked{};
    ::Profile& Profile() { return profile; }
    ::Commands* Commands() { return &commands; }
    void Transition(const ResourcePtr&,int) { transitioned=true; }
    void Retain(const ResourcePtr&) { retained=true; }
    void MarkWork(const char*) {}
    void BeforeCopyWrite(const ResourcePtr&,size_t,size_t size) {
        assert(transitioned && retained && (size==8 || size==16)); write_checked=true;
    }
};
struct Feedback {
    u64 ReadFilledSize(u32) { return 19; }
    void CopyFilledSize(u32,const ResourcePtr& resource) { resource->handle.data[0]=19; }
};
struct Impl {
    Context context;
    Feedback feedback;
    Ring readback;
    ResourcePtr gpu_counters=std::make_shared<Resource>(), gpu_report=std::make_shared<Resource>();
    bool fast_queries=true,synchronous{},pipeline_wanted{};
    int accumulated{}, dispatched{},collected{};
    std::array<std::array<u64,4>,TypeCount> totals{},baselines{};
    void RequireClosed() {}
    void AccumulateGpu() { ++accumulated; }
    void Collect() { ++collected; }
    void DispatchResolve(u32,u32,u32) { ++dispatched; }
};
struct QueryCacheRuntime {
    struct CounterResult { u64 value; StagingBufferRef result; };
    Impl storage; Impl* impl=&storage;
    void PauseHostConditionalRendering() {}
    CounterResult ReadCounter(QueryType type,u32 stream) { __READ__ }
};
u64 Word(const StagingBufferRef& result,size_t index) {
    u64 value{}; std::memcpy(&value,result.mapped_span.data()+index*8,8); return value;
}
int main() {
    QueryCacheRuntime runtime;
    auto& state=runtime.storage;
    // Every supported type/stream must capture the correct offset, including 64-bit borrow.
    for(u32 type=0;type<3;++type) {
        for(u32 stream=0;stream<4;++stream) {
            const auto index=type*4+stream;
            state.gpu_counters->handle.data[index]=0x200000001ULL+index;
            state.gpu_counters->handle.data[CounterBytes/8+index]=0x1ffffffffULL;
            const auto snapshot=runtime.ReadCounter(static_cast<QueryType>(type),stream);
            assert(snapshot.result.mapped_span.size()==16);
            assert(Word(snapshot.result,0)==0x200000001ULL+index);
            assert(Word(snapshot.result,1)==0x1ffffffffULL);
            // A later reset/reduction cannot change this report's inputs.
            state.gpu_counters->handle.data.fill(0);
            assert(Word(snapshot.result,0)-Word(snapshot.result,1)==2+index);
        }
    }
    assert(state.context.commands.copies==24 && state.accumulated==12);
    assert(state.dispatched==0 && state.collected==0 && state.context.write_checked);
    // SO byte cursors retain their specialized path, even with the knob enabled.
    const auto bytes=runtime.ReadCounter(QueryType::StreamingByteCount,0);
    assert(bytes.result.mapped_span.size()==8 && Word(bytes.result,0)==19);
    assert(state.dispatched==1);
    state.fast_queries=false;
    const auto legacy=runtime.ReadCounter(QueryType::ZPassPixelCount,0);
    assert(legacy.result.mapped_span.size()==8 && state.dispatched==2);
    bool rejected{};
    try { runtime.ReadCounter(QueryType::Pipeline,4); } catch(const std::out_of_range&) { rejected=true; }
    assert(rejected);
    state.synchronous=true;
    state.totals[0][0]=100; state.baselines[0][0]=90;
    assert(runtime.ReadCounter(QueryType::ZPassPixelCount,0).value==10 && state.collected==1);
    assert(runtime.ReadCounter(QueryType::StreamingByteCount,0).value==19);
    disabled=true;
    assert(!runtime.ReadCounter(QueryType::ZPassPixelCount,0).result.buffer);
}
""".replace("__READ__", read)
        with tempfile.TemporaryDirectory(prefix="nxbox-fast-query-") as folder:
            cpp, binary = Path(folder) / "snapshot.cpp", Path(folder) / "snapshot"
            cpp.write_text(driver)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(cpp),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_opt_in_and_default_fallback(self):
        source = (D3D12 / "d3d12_query_cache.cpp").read_text()
        context = (D3D12 / "d3d12_context.cpp").read_text()
        self.assertIn('fast_queries{BisectEnabled("NXBOX_D3D12_FAST_QUERIES")}', source)
        self.assertIn('"NXBOX_D3D12_FAST_QUERIES"', context)
        fast = function_body(
            source, "if (impl->fast_queries && type != QueryType::StreamingByteCount)"
        )
        self.assertNotIn("DispatchResolve", fast)
        self.assertNotIn("GlobalUavBarrier", fast)
        self.assertEqual(fast.count("CopyBufferRegion("), 2)
        self.assertIn("CounterBytes + offset", fast)
        self.assertIn('"query_read_counter"', context)
        self.assertIn('"fast_query_reports"', context)
