# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute descriptor plan reuse with the production plan and finish methods."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from test_d3d12_subresource_feedback import function

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"

MOCK = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using ResourcePtr = std::shared_ptr<int>;
"""
ALLOCATOR = r"""
bool BisectEnabled(const char*) { return false; }
enum class FrameCount { DescriptorTableReuses };
constexpr int D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER = 0;
constexpr int D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV = 1;
struct DescriptorTable {
    u32 first = 0;
    u32 Slot(u32 index) const { return first + index; }
};
struct Context {
    u64 tick = 1;
    unsigned reuses = 0;
    u64 CurrentTick() const { return tick; }
    Context& Profile() { return *this; }
    void Count(FrameCount) { ++reuses; }
};
struct D3D12DescriptorAllocator {
    struct CopyBatch {};
    void FlushCopies(int, CopyBatch&) {}
    struct PreviousDrawTable {
        u64 tick = 0;
        DescriptorPlan plan{{0, 0, 0}};
        DescriptorTable table;
    };
    std::array<PreviousDrawTable, 2> previous_draw_tables;
    Context context;
    u32 allocations = 0, writes = 0;
    DescriptorTable AllocateViews(u32) { return {++allocations}; }
    DescriptorTable AllocateSamplers(u32) { return {++allocations}; }
    void WriteCached(int, const ResourcePtr&, u32, const void*, u32, u32, CopyBatch*) { ++writes; }
    bool CanReuseDrawTable(const DescriptorPlan&, bool = false) const;
    DescriptorTable FinishDrawTable(DescriptorPlan&, bool = false);
};
"""
DRIVER = r"""
int main() {
    D3D12DescriptorAllocator allocator;
    DescriptorPlan plan{{0, 0, 0}};
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = resource;
    auto prepare = [&](u32 kind, std::array<u32, 3> sizes = {2, 1, 1}) {
        plan.Reset(sizes);
        for (u32 i = 0; i < plan.keys.size(); ++i) {
            plan.keys[i] = {};
            plan.keys[i].kind = kind;
            plan.keys[i].resource = reinterpret_cast<std::uintptr_t>(resource.get());
            plan.resources[i] = resource;
        }
    };
    prepare(1);
    const auto first = allocator.FinishDrawTable(plan);
    assert(allocator.allocations == 1 && allocator.writes == 4);
    plan.resources.clear();
    prepare(1);
    assert(allocator.CanReuseDrawTable(plan));
    assert(allocator.FinishDrawTable(plan).first == first.first);
    assert(allocator.allocations == 1 && allocator.context.reuses == 1);
    // Both scratch and remembered buffers have now been warmed. Alternating
    // contents must replace the immutable table without discarding either buffer.
    const auto* a = plan.keys.data();
    const auto* b = allocator.previous_draw_tables[0].plan.keys.data();
    const auto* ra = plan.resources.data();
    const auto* rb = allocator.previous_draw_tables[0].plan.resources.data();
    for (u32 i = 0; i < 20; ++i) {
        prepare(2 + i % 2);
        assert(!allocator.CanReuseDrawTable(plan));
        allocator.FinishDrawTable(plan);
        assert(plan.keys.data() == (i % 2 ? a : b));
        assert(plan.resources.data() == (i % 2 ? ra : rb));
        plan.resources.clear();
    }
    // Identical slot keys with different table boundaries cannot reuse a table.
    prepare(3, {1, 2, 1});
    assert(!allocator.CanReuseDrawTable(plan));
    allocator.FinishDrawTable(plan);
    prepare(3, {1, 2, 1});
    assert(allocator.CanReuseDrawTable(plan));
    ++allocator.context.tick;
    assert(!allocator.CanReuseDrawTable(plan));
    allocator.FinishDrawTable(plan);
    prepare(3, {1, 2, 1});
    assert(!allocator.CanReuseDrawTable(plan, true));
    allocator.FinishDrawTable(plan, true);
    // Cleanup of scratch (also on skipped draws) does not release the resources
    // owned by the remembered tables. Replacing both tables releases them.
    plan.resources.clear();
    resource.reset();
    assert(!lifetime.expired());
    prepare(0, {0, 0, 0});
    allocator.FinishDrawTable(plan);
    plan.resources.clear();
    prepare(0, {0, 0, 0});
    allocator.FinishDrawTable(plan, true);
    plan.resources.clear();
    assert(lifetime.expired());
}
"""


class DrawScratchTests(unittest.TestCase):
    def test_plan_storage_and_tick_lifetime(self):
        header = (NATIVE / "d3d12_descriptor.h").read_text()
        source = (NATIVE / "d3d12_descriptor.cpp").read_text()
        structs = header.split("struct DescriptorKey {", 1)[1].split("struct DescriptorTable {", 1)[
            0
        ]
        methods = "\n".join(
            function(source, signature)
            for signature in (
                "bool D3D12DescriptorAllocator::CanReuseDrawTable(",
                "DescriptorTable D3D12DescriptorAllocator::FinishDrawTable(",
            )
        )
        with tempfile.TemporaryDirectory(prefix="nxbox-draw-scratch-") as temp:
            cpp = Path(temp) / "scratch.cpp"
            binary = Path(temp) / "scratch"
            cpp.write_text(MOCK + "struct DescriptorKey {" + structs + ALLOCATOR + methods + DRIVER)
            subprocess.run(
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
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
