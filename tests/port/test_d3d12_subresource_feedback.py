# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the actual transition methods with a recording-only D3D12 mock."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


MOCK = r"""
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using UINT = unsigned;
using D3D12_RESOURCE_STATES = unsigned;
inline constexpr unsigned D3D12_RESOURCE_STATE_RENDER_TARGET = 4;
inline constexpr unsigned D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 8;
inline constexpr unsigned D3D12_RESOURCE_STATE_STREAM_OUT = 256;
inline constexpr unsigned D3D12_RESOURCE_STATE_COPY_DEST = 1024;
inline constexpr unsigned SrvState = 64 | 128;
inline constexpr unsigned D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES = ~0u;
inline constexpr unsigned D3D12_RESOURCE_BARRIER_TYPE_TRANSITION = 0;
inline constexpr unsigned D3D12_RESOURCE_BARRIER_TYPE_UAV = 1;
inline constexpr unsigned D3D12_RESOURCE_STATE_COMMON = 0;
bool BisectEnabled(const char*) { return std::getenv("FAST") != nullptr; }
inline constexpr unsigned D3D12_HEAP_TYPE_DEFAULT = 1;
inline constexpr unsigned D3D12_RESOURCE_DIMENSION_BUFFER = 1;
inline constexpr unsigned D3D12_RESOURCE_DIMENSION_TEXTURE2D = 3;
inline constexpr unsigned D3D12_RESOURCE_DIMENSION_TEXTURE3D = 4;
inline constexpr unsigned D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL = 2;
inline constexpr unsigned D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE = 8;
struct Desc {
    unsigned Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    unsigned MipLevels = 3;
    unsigned DepthOrArraySize = 2;
    unsigned Flags = 0;
};
struct ID3D12Resource {
    Desc desc;
    Desc GetDesc() const { return desc; }
};
struct Handle {
    ID3D12Resource value;
    ID3D12Resource* Get() { return &value; }
    ID3D12Resource* operator->() { return Get(); }
};
struct Resource {
    Handle handle;
    unsigned state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    std::vector<unsigned> sub_states;
    unsigned heap = D3D12_HEAP_TYPE_DEFAULT;
    u64 content_version = 0;
};
using ResourcePtr = std::shared_ptr<Resource>;
struct D3D12_RESOURCE_BARRIER {
    unsigned Type;
    struct {
        ID3D12Resource* pResource;
        unsigned Subresource;
        unsigned StateBefore;
        unsigned StateAfter;
    } Transition;
};
enum class FramePhase { Barriers };
enum class FrameCount { Barriers };
struct FrameProfile {
    struct Scope { Scope(FrameProfile&, FramePhase) {} };
    bool Enabled() { return true; }
    void Count(FrameCount, std::size_t = 1) {}
    void NoteTransition(unsigned, unsigned, bool) {}
};
struct CommandsMock {
    std::vector<std::vector<D3D12_RESOURCE_BARRIER>> calls;
    void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
        assert(count);
        calls.emplace_back(barriers, barriers + count);
    }
};
struct Context {
    FrameProfile profile;
    CommandsMock recording;
    CommandsMock* commands = &recording;
    std::unordered_map<ID3D12Resource*, unsigned> copy_writes;
    std::vector<D3D12_RESOURCE_BARRIER> transition_barriers;
    CommandsMock* Commands() { return commands; }
    std::function<void()> on_retain;
    void Retain(const ResourcePtr&) {
        Commands();
        if (on_retain) {
            auto callback = std::move(on_retain);
            on_retain = {};
            callback();
        }
    }
    void Transition(const ResourcePtr&, D3D12_RESOURCE_STATES);
    void TransitionBatch(std::span<const std::pair<ResourcePtr, D3D12_RESOURCE_STATES>>,
                         bool = false,
                         std::span<const std::pair<ResourcePtr, std::vector<u32>>> = {},
                         D3D12_RESOURCE_STATES = D3D12_RESOURCE_STATE_COMMON);
    void AppendSubresourceTransitions(const ResourcePtr&, std::span<const u32>, D3D12_RESOURCE_STATES);
    void TransitionSubresources(const ResourcePtr&, std::span<const u32>, D3D12_RESOURCE_STATES);
};
"""
DRIVER = r"""
int main() {
    Context context;
    auto resource = std::make_shared<Resource>();
    auto* handle = resource->handle.Get();
    const u32 sampled[] = {0, 3, 0}; // mip0 in both array layers; duplicate is harmless.
    context.copy_writes[handle] = 1;
    context.TransitionSubresources(resource, sampled, SrvState);
    assert(resource->sub_states.size() == 6);
    assert(context.recording.calls.size() == 1);
    const auto& split = context.recording.calls.back();
    assert(split.size() == 2);
    assert(split[0].Transition.Subresource == 0);
    assert(split[1].Transition.Subresource == 3);
    for (const auto& barrier : split) {
        assert(barrier.Transition.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET);
        assert(barrier.Transition.StateAfter == SrvState);
    }
    assert(resource->sub_states[1] == D3D12_RESOURCE_STATE_RENDER_TARGET);
    assert(context.copy_writes.empty());
    assert(resource->content_version == 0);
    context.TransitionSubresources(resource, sampled, SrvState);
    assert(context.recording.calls.size() == 1);
    // Draw B: uniform state still says RT, but sampled mip0 must be transitioned back.
    context.Transition(resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
    assert(resource->sub_states.empty());
    assert(context.recording.calls.size() == 2);
    const auto& unsplit = context.recording.calls.back();
    assert(unsplit.size() == 2);
    for (const auto& barrier : unsplit) {
        assert(barrier.Transition.StateBefore == SrvState);
        assert(barrier.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET);
        assert(barrier.Transition.Subresource == 0 || barrier.Transition.Subresource == 3);
    }
    context.Transition(resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
    assert(context.recording.calls.size() == 2);
    // Unsplit with every subresource already equal: no barrier or copy-write reset.
    const u32 all[] = {0, 1, 2, 3, 4, 5};
    context.TransitionSubresources(resource, all, SrvState);
    context.copy_writes[handle] = 1;
    const auto calls = context.recording.calls.size();
    context.Transition(resource, SrvState);
    assert(context.recording.calls.size() == calls);
    assert(resource->sub_states.empty() && resource->state == SrvState);
    assert(context.copy_writes.contains(handle));
    // Whole-resource write invalidation occurs exactly once, including split resources.
    context.TransitionSubresources(resource, sampled, D3D12_RESOURCE_STATE_RENDER_TARGET);
    context.Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST);
    assert(resource->content_version == 1 && resource->sub_states.empty());
    assert(context.recording.calls.back().size() == 6);
    assert(context.copy_writes.empty());
    context.copy_writes[handle] = 1;
    context.Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST);
    assert(resource->content_version == 2 && context.copy_writes.contains(handle));
    // Batch must also unsplit when the stale uniform state matches the target.
    context.TransitionSubresources(resource, sampled, SrvState);
    const std::pair<ResourcePtr, unsigned> uses[] = {
        {resource, D3D12_RESOURCE_STATE_COPY_DEST}
    };
    context.TransitionBatch(uses);
    assert(resource->content_version == 3 && resource->sub_states.empty());
    assert(context.recording.calls.back().size() == 2);
    // Volume depth is not an array of subresources.
    auto volume = std::make_shared<Resource>();
    volume->handle->desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    volume->handle->desc.DepthOrArraySize = 16;
    const u32 mip[] = {2};
    context.TransitionSubresources(volume, mip, SrvState);
    assert(volume->sub_states.size() == 3);
    const u32 invalid[] = {3};
    try {
        context.TransitionSubresources(volume, invalid, SrvState);
        assert(false);
    } catch (const std::out_of_range&) {}
    auto depth = std::make_shared<Resource>();
    depth->handle->desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    try {
        context.TransitionSubresources(depth, mip, SrvState);
        assert(false);
    } catch (const std::invalid_argument&) {}
    assert(depth->sub_states.empty());
    // The normal draw path batches unique resources. Preserve transition order,
    // write versions and copy hazards, including uses that need no barrier.
    Context batch_context;
    auto color = std::make_shared<Resource>();
    auto storage = std::make_shared<Resource>();
    auto upload = std::make_shared<Resource>();
    upload->heap = 2;
    const std::pair<ResourcePtr, unsigned> draw_uses[] = {
        {color, SrvState}, {storage, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
        {upload, SrvState}
    };
    batch_context.copy_writes[color->handle.Get()] = 1;
    batch_context.copy_writes[upload->handle.Get()] = 1;
    batch_context.TransitionBatch(draw_uses);
    assert(batch_context.recording.calls.size() == 1);
    const auto& batch = batch_context.recording.calls.back();
    assert(batch.size() == 2);
    assert(batch[0].Transition.pResource == color->handle.Get());
    assert(batch[1].Transition.pResource == storage->handle.Get());
    assert(storage->content_version == 1);
    assert(!batch_context.copy_writes.contains(color->handle.Get()));
    assert(batch_context.copy_writes.contains(upload->handle.Get()));
    const auto* scratch = batch_context.transition_barriers.data();
    batch_context.TransitionBatch(draw_uses);
    assert(batch_context.transition_barriers.data() == scratch);
    assert(batch_context.recording.calls.size() == 1);
    assert(storage->content_version == 2);
    batch_context.TransitionBatch({});
    assert(batch_context.recording.calls.size() == 1);
    // Completion callbacks may recursively transition resources while retaining
    // the draw inputs. Their scratch must be emitted before the outer batch.
    auto nested = std::make_shared<Resource>();
    batch_context.on_retain = [&] {
        const std::pair<ResourcePtr, unsigned> nested_use[] = {{nested, SrvState}};
        batch_context.TransitionBatch(nested_use);
    };
    color->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    batch_context.TransitionBatch(draw_uses);
    assert(batch_context.recording.calls.size() == 3);
    assert(batch_context.recording.calls[1][0].Transition.pResource == nested->handle.Get());
    assert(batch_context.recording.calls[2].size() == 1);
    assert(batch_context.recording.calls[2][0].Transition.pResource == color->handle.Get());
}
"""


VIEW_MOCK = r"""
struct Range {
    struct { int level, layer; } base;
    struct { int levels, layers; } extent;
};
struct ImageView {
    ResourcePtr resource;
    Range range;
    ResourcePtr ImageHandle() const { return resource; }
};
"""
VIEW_DRIVER = r"""
    auto resource = std::make_shared<Resource>();
    ImageView view{resource, {{0, 0}, {1, 1}}};
    ImageView mip1{resource, {{1, 0}, {1, 1}}};
    ImageView layer1{resource, {{0, 1}, {1, 1}}};
    ImageView* color_views[] = {&mip1, nullptr};
    bool subresource_feedback = false;
    bool has_depth = false;
    ImageView* depth_view = &view;
    // SOURCE_LAMBDAS
    assert(color_disjoint(view));
    assert(!can_split_feedback(view));
    subresource_feedback = true;
    assert(can_split_feedback(view));
    color_views[1] = &view;
    assert(!can_split_feedback(view)); // Any overlapping attachment requires a copy.
    color_views[1] = nullptr;
    color_views[0] = &layer1;
    assert(can_split_feedback(view));
    resource->handle->desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    assert(!can_split_feedback(view)); // Different depth slices are the same subresource.
    color_views[0] = &mip1;
    assert(can_split_feedback(view));
    has_depth = true;
    assert(!can_split_feedback(view));
    has_depth = false;
    resource->handle->desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    assert(!can_split_feedback(view));
"""


class SubresourceFeedbackTests(unittest.TestCase):
    def test_source_contract(self):
        header = (NATIVE / "d3d12_context.h").read_text()
        context = (NATIVE / "d3d12_context.cpp").read_text()
        rasterizer = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        self.assertIn("std::vector<D3D12_RESOURCE_STATES> sub_states;", header)
        self.assertIn("void TransitionSubresources(", header)
        transition = function(context, "void Context::Transition(")
        self.assertIn("if (!resource->sub_states.empty())", transition)
        self.assertIn("TransitionBatch(std::span{&use, 1});", transition)
        self.assertLess(
            transition.index("Retain(resource)"), transition.index("sub_states.empty()")
        )
        self.assertIn('BisectEnabled("NXBOX_D3D12_SUBRESOURCE_FEEDBACK")', rasterizer)
        self.assertIn("FeedbackDisjoint,\n    FeedbackAvoided,", header)
        self.assertIn('"feedback_avoided"', context)
        self.assertIn("Count(FrameCount::FeedbackAvoided)", rasterizer)
        self.assertIn("if ((color_feedback || depth_feedback) && !split_feedback)", rasterizer)
        recording = rasterizer.split("// ---- 4. Record state", 1)[1]
        self.assertLess(
            recording.index("TransitionBatch(state_scratch)"),
            recording.index("TransitionSubresources("),
        )
        self.assertIn("need(sampled, SrvState);", rasterizer)

    def test_actual_overlap_and_gate_decisions(self):
        rasterizer = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        lambdas = "\n".join(
            function(rasterizer, f"const auto {name} =") + ";"
            for name in ("color_disjoint", "can_split_feedback")
        )
        driver = VIEW_DRIVER.replace("// SOURCE_LAMBDAS", lambdas)
        with tempfile.TemporaryDirectory(prefix="nxbox-feedback-range-") as temp:
            source = Path(temp) / "ranges.cpp"
            binary = Path(temp) / "ranges"
            source.write_text(MOCK + VIEW_MOCK + "int main() {" + driver + "}")
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
                timeout=60,
            )
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_actual_transition_barriers(self):
        context = (NATIVE / "d3d12_context.cpp").read_text()
        methods = "\n".join(
            function(context, f"void Context::{name}(")
            for name in (
                "Transition",
                "TransitionSubresources",
                "AppendSubresourceTransitions",
                "TransitionBatch",
            )
        )
        with tempfile.TemporaryDirectory(prefix="nxbox-subresource-") as temp:
            source = Path(temp) / "transitions.cpp"
            binary = Path(temp) / "transitions"
            source.write_text(MOCK + methods + DRIVER)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
                timeout=60,
            )
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
