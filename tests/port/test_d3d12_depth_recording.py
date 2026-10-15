# SPDX-License-Identifier: GPL-3.0-or-later
"""Attachment intersection and abandoned descriptor recordings on a host compiler."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RENDERER = ROOT / "src/video_core/renderer_d3d12"


def function_body(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def run_cpp(source):
    with tempfile.TemporaryDirectory(prefix="nxbox-depth-recording-") as directory:
        path = Path(directory) / "test.cpp"
        binary = path.with_suffix("")
        path.write_text(source)
        subprocess.run(
            [
                "clang++",
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-I",
                str(ROOT / "src"),
                str(path),
                "-o",
                str(binary),
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        subprocess.run([str(binary)], check=True, timeout=10)


class DepthRecordingTests(unittest.TestCase):
    def test_intersection_keeps_depth_and_normalizes_all_colors(self):
        run_cpp(r"""
#include <cassert>
#include "video_core/renderer_d3d12/d3d12_render_area.h"
int main() {
    using namespace D3D12;
    constexpr AttachmentExtent depth{208, 112, 1};
    constexpr auto area = IntersectAttachments({224, 112, 1}, depth);
    static_assert(area == depth);
    static_assert(MatchingAttachmentExtent(area, depth, true) == depth);
    // Smaller colors keep a larger DSV; rendering/copyback still use the intersection.
    constexpr AttachmentExtent large_depth{832, 480, 4};
    constexpr auto small = IntersectAttachments({800, 450, 2}, large_depth);
    static_assert(small == AttachmentExtent{800, 450, 2});
    static_assert(MatchingAttachmentExtent(small, large_depth, true) ==
                  AttachmentExtent{832, 480, 2});
    // MRT without depth uses the smallest attachment; distinct mip/layer views
    // feed their effective extents rather than full-resource dimensions.
    constexpr auto mrt = IntersectAttachments({832, 450, 3}, {800, 450, 1});
    static_assert(MatchingAttachmentExtent(mrt, {}, false) == AttachmentExtent{800,450,1});
    static_assert(IntersectAttachments(mrt, {0, 16, 1}).width == 0);
}
""")

    def test_real_allocator_recovers_abandoned_producer(self):
        # Compile the production method, not a reimplementation of its wait loop.
        method = function_body(
            (RENDERER / "d3d12_descriptor.cpp").read_text(),
            "DescriptorTable D3D12DescriptorAllocator::Allocate(",
        )
        run_cpp(
            r"""
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include "video_core/renderer_d3d12/d3d12_ring_allocator.h"
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using SIZE_T = std::uint64_t;
using UINT64 = std::uint64_t;
struct Handle { u64 ptr{}; };
struct DescriptorTable { Handle cpu{}, gpu{}; u32 count{}, stride{}; };
enum class FrameCount { ForcedSubmitDescriptorExhaustion };
struct FakeContext {
    u64 submitted{};
    bool recording{};
    unsigned calls{};
    void Commands() {
        assert(++calls < 100); // Catch a busy loop without hanging the suite.
        recording = true;
    }
    u64 CurrentTick() const { return submitted + 1; }
    bool IsFree(u64 tick) const { return tick <= submitted; }
    void Submit(FrameCount reason) { assert(reason == FrameCount::ForcedSubmitDescriptorExhaustion); if (recording) { ++submitted; recording = false; } }
    void Wait(u64 tick, std::string_view = {}) { assert(tick <= submitted); }
};
class D3D12DescriptorAllocator {
public:
    struct Ring {
        D3D12::RingAllocator math{2};
        u32 stride{1};
        Handle cpu{}, gpu{};
        bool shader_visible{true};
    };
    DescriptorTable Allocate(Ring& ring, u32 count);
    FakeContext context;
};
"""
            + method
            + r"""
int main() {
    D3D12DescriptorAllocator allocator;
    D3D12DescriptorAllocator::Ring ring;
    // A skipped draw left slots at CurrentTick, with no native draw recorded.
    assert(ring.math.Allocate(2, allocator.context.CurrentTick()));
    auto table = allocator.Allocate(ring, 1);
    assert(table.count == 1 && allocator.context.submitted == 1);
    assert(ring.math.Used() == 1 && *ring.math.OldestTick() == 2);
    for (auto invalid : {0u, 3u}) {
        bool rejected = false;
        try { allocator.Allocate(ring, invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
}
"""
        )

    def test_skip_closes_queries_and_submits_reserved_slots(self):
        source = (RENDERER / "d3d12_rasterizer.cpp").read_text()
        skip = function_body(source, "void RasterizerD3D12::SkipDraw(")
        for cleanup in ["stage.Clear()", "state_scratch.clear()", "converted_indices.reset()"]:
            self.assertIn(cleanup, skip)
        self.assertLess(skip.index("query_runtime.CancelDraw()"), skip.index("context.Submit(FrameCount::ForcedSubmitSkippedDraw)"))
        prepare = function_body(source, "void RasterizerD3D12::PrepareDraw(")
        self.assertIn("SCOPE_EXIT", prepare)
        self.assertLess(
            prepare.index("state_scratch.clear()"), prepare.index("if (!pipeline_cache)")
        )
        query = function_body(
            (RENDERER / "d3d12_query_cache.cpp").read_text(), "void QueryCacheRuntime::CancelDraw("
        )
        self.assertIn("if (impl->drawing)", query)
        self.assertIn("EndDraw()", query)
        self.assertIn("PauseHostConditionalRendering()", query)

    def test_allocations_precede_descriptor_reservation(self):
        prepare = function_body(
            (RENDERER / "d3d12_rasterizer.cpp").read_text(), "void RasterizerD3D12::PrepareDraw("
        )
        reserve = prepare.index("descriptors.Reserve(")
        for allocating in [
            "context.CreateResource(",
            "context.CreateBuffer(",
            "buffer_cache.ObtainBuffer(",
            "transform_feedback.BindBuffer(",
        ]:
            self.assertLess(prepare.rindex(allocating), reserve, allocating)
        self.assertIn("CopyFeedback(sampled, &texture_cache.GetImageView(item.id).range)", prepare[:reserve])
        injection = prepare.index('SkipDraw("injected descriptor recording failure")')
        self.assertLess(reserve, injection)
        self.assertLess(injection, prepare.index("commands->OMSetRenderTargets("))
        self.assertLess(
            prepare.index("query_runtime.EndDraw()"),
            prepare.index("std::swap(region.src_subresource"),
        )
        self.assertIn("need(color_resources[slot]", prepare)

    def test_draw_texture_gets_a_fresh_feedback_snapshot(self):
        draw = function_body(
            (RENDERER / "d3d12_rasterizer.cpp").read_text(), "void RasterizerD3D12::DrawTexture("
        )
        self.assertLess(draw.index("++feedback_draw"), draw.index("CopyFeedback("))

    def test_feedback_target_does_not_reject_large_copies(self):
        copy = function_body(
            (RENDERER / "d3d12_rasterizer.cpp").read_text(),
            "ResourcePtr RasterizerD3D12::CopyFeedback(",
        )
        self.assertNotIn("PoolLimit", copy)
        self.assertNotIn("MemoryBudget::Limit -", copy)
        self.assertIn("context.CreateResource(", copy)
        self.assertIn("if (!feedback_failure_logged)", copy)
        self.assertIn("D3D12_FEEDBACK_ALLOCATION_FAILED", copy)
        self.assertIn("pool_bytes={} memory_bytes={}", copy)


if __name__ == "__main__":
    unittest.main()
