# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile production transition and root binding logic with recording mocks."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_d3d12_subresource_feedback import MOCK, NATIVE, function


class FastRecordingTests(unittest.TestCase):
    def compile_run(self, source, fast):
        with tempfile.TemporaryDirectory(prefix="nxbox-fast-recording-") as temp:
            path = Path(temp) / "test.cpp"
            binary = Path(temp) / "test"
            path.write_text(source)
            result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    str(path),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
                timeout=60,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            env = dict(os.environ)
            env.pop("FAST", None)
            if fast:
                env["FAST"] = "1"
            subprocess.run([str(binary)], check=True, env=env, timeout=30)

    def test_ordered_batch_and_uniform_subresources(self):
        context = (NATIVE / "d3d12_context.cpp").read_text()
        methods = "\n".join(
            function(context, f"void Context::{name}(")
            for name in ("AppendSubresourceTransitions", "TransitionBatch")
        )
        driver = r"""
int main() {
    Context context;
    auto color = std::make_shared<Resource>();
    auto sampled = std::make_shared<Resource>();
    color->sub_states.assign(6, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const std::pair<ResourcePtr, unsigned> uses[] = {{color, SrvState}};
    const std::pair<ResourcePtr, std::vector<u32>> samples[] = {{sampled, {0, 3, 0}}};
    context.copy_writes[color->handle.Get()] = 1;
    context.TransitionBatch(uses, true, samples, SrvState);
    assert(context.recording.calls.size() == 1);
    const auto& barriers = context.recording.calls.back();
    const bool fast = BisectEnabled("");
    assert(barriers.size() == (fast ? 4u : 9u));
    assert(barriers[0].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV);
    if (fast)
        assert(barriers[1].Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    assert(barriers.back().Transition.pResource == sampled->handle.Get());
    assert(barriers.back().Transition.Subresource == 3);
    assert(context.copy_writes.empty());
    assert(color->sub_states.empty() && color->state == SrvState);
    assert(sampled->sub_states[1] == D3D12_RESOURCE_STATE_RENDER_TARGET);
    // Repeated states remove transitions, never the explicit ordering UAV barrier.
    context.TransitionBatch(uses, true, samples, SrvState);
    assert(context.recording.calls.size() == 2);
    assert(context.recording.calls.back().size() == 1);
    assert(context.recording.calls.back()[0].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV);
    context.TransitionBatch(uses, false, samples, SrvState);
    assert(context.recording.calls.size() == 2);
}
"""
        for fast in (False, True):
            with self.subTest(fast=fast):
                self.compile_run(MOCK + methods + driver, fast)

    def test_disabled_barrier_command_stream(self):
        context = (NATIVE / "d3d12_context.cpp").read_text()
        methods = "\n".join(
            function(context, f"void Context::{name}(")
            for name in (
                "Transition",
                "AppendSubresourceTransitions",
                "TransitionSubresources",
                "TransitionBatch",
            )
        )
        driver = r"""
int main() {
    Context context;
    auto color = std::make_shared<Resource>();
    auto buffer = std::make_shared<Resource>();
    buffer->handle->desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer->state = D3D12_RESOURCE_STATE_COPY_DEST;
    const u32 samples[] = {0, 3, 0};
    context.TransitionSubresources(color, samples, SrvState);
    const std::pair<ResourcePtr, unsigned> uses[] = {
        {color, D3D12_RESOURCE_STATE_RENDER_TARGET},
        {buffer, SrvState}, {buffer, SrvState}};
    context.TransitionBatch(uses);
    context.TransitionBatch(uses);
    context.TransitionSubresources(color, samples, SrvState);
    context.TransitionSubresources(color, samples, SrvState);
    context.Transition(color, SrvState);
    // Golden pre-optimization command payload, including ResourceBarrier call boundaries.
    // Resource pointers become stable IDs; padding is excluded from API argument encoding.
    std::vector<u64> stream;
    for (const auto& call : context.recording.calls) {
        stream.push_back(call.size());
        for (const auto& barrier : call) {
            stream.insert(stream.end(), {barrier.Type,
                barrier.Transition.pResource == color->handle.Get() ? 1u : 2u,
                barrier.Transition.Subresource, barrier.Transition.StateBefore,
                barrier.Transition.StateAfter});
        }
    }
    const u64 all = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    const u64 rt = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const std::vector<u64> expected{
        2, 0,1,0,rt,SrvState, 0,1,3,rt,SrvState,
        3, 0,1,0,SrvState,rt, 0,1,3,SrvState,rt,
           0,2,all,D3D12_RESOURCE_STATE_COPY_DEST,SrvState,
        2, 0,1,0,rt,SrvState, 0,1,3,rt,SrvState,
        4, 0,1,1,rt,SrvState, 0,1,2,rt,SrvState,
           0,1,4,rt,SrvState, 0,1,5,rt,SrvState};
    assert(stream.size() == expected.size());
    assert(std::memcmp(stream.data(), expected.data(), stream.size() * sizeof(u64)) == 0);
}
"""
        self.compile_run("#include <cstring>\n" + MOCK + methods + driver, False)

    def test_actual_compute_root_binds(self):
        dispatch = (NATIVE / "d3d12_dispatch.cpp").read_text()
        signature = dispatch[dispatch.index("    auto& roots = context.ComputeRoots();") :]
        signature = signature[: signature.index("    commands->SetPipelineState")]
        cbvs = function(dispatch, "    if (bindings.root_cbvs)")
        header = (NATIVE / "d3d12_context.h").read_text()
        invalidate = function(header, "    void InvalidateRecordedState()")
        graphics = function(header, "    void InvalidateGraphicsState()")
        state = function(header, "    struct ComputeRootState") + ";"
        source = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>
#include "HEADER"
using namespace D3D12;
using UINT = unsigned;
using D3D12_GPU_VIRTUAL_ADDRESS = std::uint64_t;
struct ID3D12RootSignature {};
enum class FrameCount { RootCbvBinds };
struct Context {
    STATE
    ComputeRootState compute_roots;
    std::uint64_t recording_generation{};
    INVALIDATE
    GRAPHICS
    ComputeRootState& ComputeRoots() { return compute_roots; }
    Context& Profile() { return *this; }
    unsigned binds{};
    void Count(FrameCount) { ++binds; }
};
struct Commands {
    unsigned signatures{}, cbvs{};
    std::vector<std::array<std::uint64_t, 3>> stream;
    void SetComputeRootSignature(ID3D12RootSignature* root) {
        ++signatures;
        stream.push_back({0, reinterpret_cast<std::uintptr_t>(root), 0});
    }
    void SetComputeRootConstantBufferView(UINT slot, D3D12_GPU_VIRTUAL_ADDRESS address) {
        ++cbvs;
        stream.push_back({1, slot, address});
    }
};
struct Bindings {
    struct Root { ID3D12RootSignature* ptr; auto Get() const { return ptr; } } root_signature;
    bool root_cbvs = true;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> root_cbv_addresses{256, 512};
};
int main() {
    constexpr UINT kFirstRootCbv = 4;
    Context context;
    Commands recording;
    auto* commands = &recording;
    ID3D12RootSignature first, second;
    Bindings bindings{{&first}};
    bool fast_roots = true;
    const auto bind = [&] { SIGNATURE CBVS };
    bind(); bind();
    assert(recording.cbvs == 2 && recording.signatures == 1);
    bindings.root_cbv_addresses[1] = 768;
    context.InvalidateGraphicsState();
    bind();
    assert(recording.cbvs == 3);
    bindings.root_signature.ptr = &second;
    bind();
    assert(recording.cbvs == 5 && recording.signatures == 2);
    context.InvalidateRecordedState(); // List reset or an external root signature writer.
    bind();
    assert(recording.cbvs == 7 && recording.signatures == 3);
    fast_roots = false;
    recording.stream.clear();
    bind(); bind();
    const std::vector<std::array<std::uint64_t, 3>> expected{
        {0, reinterpret_cast<std::uintptr_t>(&second), 0}, {1, 4, 256}, {1, 5, 768},
        {0, reinterpret_cast<std::uintptr_t>(&second), 0}, {1, 4, 256}, {1, 5, 768}};
    assert(recording.stream == expected); // Disabled path keeps every call and argument.
    assert(recording.cbvs == 11 && recording.signatures == 5);
    assert(context.binds == recording.cbvs);
}
"""
        replacements = {
            "HEADER": str(NATIVE / "d3d12_recorded_state.h"),
            "STATE": state,
            "INVALIDATE": invalidate,
            "GRAPHICS": graphics,
            "SIGNATURE": signature,
            "CBVS": cbvs,
        }
        for key, value in replacements.items():
            source = source.replace(key, value)
        self.compile_run(source, True)

    def test_knobs_logged_and_dispatch_classification(self):
        context = (NATIVE / "d3d12_context.cpp").read_text()
        for knob in ("NXBOX_D3D12_FAST_BARRIERS", "NXBOX_D3D12_FAST_ROOT_BINDS"):
            self.assertIn(f'BisectEnabled("{knob}")', context)
            self.assertIn(f'"{knob}"', context.split('Diagnostic(fmt::format("D3D12 bisect')[0])
        self.assertIn('"guest_dispatches"', context)
        self.assertIn('"helper_dispatches"', context)
        self.assertIn('"query_dispatches"', context)


if __name__ == "__main__":
    unittest.main()
