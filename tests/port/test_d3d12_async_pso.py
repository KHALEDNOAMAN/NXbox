# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise actual queue synchronization, backpressure, failure publication and teardown."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <atomic>
#include <cassert>
#include <future>
#include <stdexcept>
#include <thread>
#include <chrono>
#include "video_core/renderer_d3d12/d3d12_async_pso.h"
#include "video_core/renderer_d3d12/d3d12_context.h"
#include "video_core/renderer_d3d12/d3d12_pso_diagnostics.h"
using Cache = D3D12::AsyncPsoWorkerPool<int, int>;
int main() {
    assert(D3D12::CountNonliteralIndices("lmem[u]", "lmem[") == 1);
    assert(D3D12::CountNonliteralIndices("uint lmem[256]; lmem[0u] = lmem[word]; lmem[other + 1] = 0;", "lmem[") == 2);

    assert(D3D12::CountLlvmInstructions("; header\ndefine void @main() {\nentry:\n"
        "  %1 = add i32 1, 2\n  store i32 %1, i32* %p\n  br label %end\n"
        "  switch i32 %1, label %end [\n    i32 1, label %end\n  ]\n"
        "  ret void\n}\n!0 = !{i32 1}\n") == 5);

    // Small draws and stream output wait; the count is the drawn one only.
    using D3D12::CanSkipPendingGraphicsPso;
    assert(!CanSkipPendingGraphicsPso(false, 100, false));
    assert(!CanSkipPendingGraphicsPso(true, 6, false));
    assert(!CanSkipPendingGraphicsPso(true, 0, false));
    assert(!CanSkipPendingGraphicsPso(true, 100, true));
    assert(CanSkipPendingGraphicsPso(true, 7, false));
    {
        Cache pending{1, 1};
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        assert(pending.Submit(1, [&] {
            entered.set_value();
            gate.wait();
            return std::make_shared<int>(42);
        }));
        entered.get_future().wait();
        // A pending draw can return without waiting for the blocked compiler.
        assert(CanSkipPendingGraphicsPso(true, 100, false));
        assert(pending.Lookup(1).status == Cache::Status::Pending);
        assert(!pending.CanSubmit());
        assert(!pending.Submit(2, [] { return std::make_shared<int>(7); }));
        assert(pending.Lookup(2).status == Cache::Status::Missing);
        // Skipping must not poison the request: protected draws using the same key
        // can still wait, and later draws consume the published result.
        assert(!CanSkipPendingGraphicsPso(true, 3, false));
        release.set_value();
        auto ready = pending.WaitFor(1, std::chrono::seconds{2});
        assert(ready.status == Cache::Status::Ready && *ready.result == 42);
        assert(pending.Lookup(1).result == ready.result);
    }

    Cache cache{2, 2};
    std::promise<void> release;
    auto wait = release.get_future().share();
    std::atomic<int> running{}, maximum{}, builds{};
    auto job = [&] {
        ++builds;
        const int current = ++running;
        int old = maximum.load();
        while (old < current && !maximum.compare_exchange_weak(old, current)) {}
        wait.wait();
        --running;
        return std::make_shared<int>(42);
    };
    assert(cache.Submit(1, job, true));
    assert(cache.Lookup(1).prewarmed);
    assert(!cache.Submit(1, job));
    assert(cache.Submit(2, job));
    assert(!cache.CanSubmit());
    assert(!cache.Submit(3, job));
    while (running.load() < 2) std::this_thread::yield();
    assert(cache.Lookup(1).status == Cache::Status::Pending);
    assert(cache.Lookup(7).status == Cache::Status::Missing);
    release.set_value();
    while (cache.Lookup(1).status == Cache::Status::Pending ||
           cache.Lookup(2).status == Cache::Status::Pending) std::this_thread::yield();
    assert(*cache.Lookup(1).result == 42 && builds == 2 && maximum == 2);
    assert(cache.Lookup(1).prewarmed && !cache.Lookup(2).prewarmed);
    assert(cache.Submit(3, []() -> std::shared_ptr<int> { throw std::runtime_error("negative"); }));
    while (cache.Lookup(3).status == Cache::Status::Pending) std::this_thread::yield();
    assert(cache.Lookup(3).status == Cache::Status::Failed);
    assert(cache.Lookup(3).error == "negative");
    assert(!cache.Submit(3, job));
    assert(cache.Submit(4, []() -> std::shared_ptr<int> { throw 1; }));
    while (cache.Lookup(4).status == Cache::Status::Pending) std::this_thread::yield();
    assert(cache.Lookup(4).status == Cache::Status::Failed);
    cache.Stop();
    cache.Stop();
    assert(!cache.Submit(5, job));
    // Shutdown cancels queued jobs and detaches active ones; their results still land.
    Cache shutdown{1, 3};
    std::promise<void> started, unblock;
    auto gate = unblock.get_future().share();
    assert(shutdown.Submit(1, [&] { started.set_value(); gate.wait(); return std::make_shared<int>(1); }));
    started.get_future().wait();
    assert(shutdown.Submit(2, [] { return std::make_shared<int>(2); }));
    assert(shutdown.Stop() == 1);
    assert(shutdown.Lookup(2).status == Cache::Status::Failed);
    unblock.set_value();
    while (shutdown.Lookup(1).status == Cache::Status::Pending) std::this_thread::yield();
    assert(shutdown.Lookup(1).status == Cache::Status::Ready);
    // A job stuck past the stall limit frees its queue slot and gets a replacement worker;
    // shutdown detaches it instead of waiting forever.
    {
        std::promise<void> entered, freed;
        auto stuck_gate = freed.get_future().share();
        std::atomic<bool> stuck_done{};
        {
            Cache stall{1, 1, std::chrono::milliseconds{50}, 2};
            assert(stall.Idle());
            assert(stall.Submit(1, [&] {
                entered.set_value();
                stuck_gate.wait();
                stuck_done = true;
                return std::make_shared<int>(1);
            }));
            entered.get_future().wait();
            assert(!stall.CanSubmit());
            assert(!stall.Idle());
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            assert(stall.Stalled() == 1);
            // A stalled job never finishes; it must not keep the memory guard deferring forever.
            assert(stall.Idle());
            assert(stall.Submit(2, [] { return std::make_shared<int>(2); }));
            while (stall.Lookup(2).status == Cache::Status::Pending) std::this_thread::yield();
            assert(*stall.Lookup(2).result == 2);
            assert(stall.Idle());
            assert(stall.Lookup(1).status == Cache::Status::Pending);
            assert(stall.IsStalled(1, std::chrono::milliseconds{50}) &&
                   !stall.IsStalled(2, std::chrono::milliseconds{50}));
            assert(!stall.IsStalled(1, std::chrono::seconds{60}));
        }
        freed.set_value();
        while (!stuck_done) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return 0;
}
"""


class AsyncPsoTests(unittest.TestCase):
    def test_worker_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "worker.cpp"
            binary = Path(directory) / "worker"
            source.write_text(DRIVER)
            subprocess.run(
                [
                    os.environ.get("CXX", "c++"),
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pthread",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_worker_inputs_and_gate(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp").read_text()
        worker = source.split("BuildAsyncGraphics(", 1)[1].split("} // namespace", 1)[0]
        self.assertNotIn("context.", worker)
        self.assertNotIn("maxwell3d->", worker)
        self.assertIn("live.envs[index].Snapshot(texture_source)", source)
        self.assertIn("DrawsSkippedPendingPso", source)
        self.assertIn("DispatchesSkippedPendingPso", source)
        graphics = source.split("const bool skip_pending =", 1)[1].split(
            "Count(FrameCount::DrawsSkippedPendingPso)", 1
        )[0]
        self.assertIn('GetEnvironmentVariableA("NXBOX_D3D12_ASYNC_SHADERS"', graphics)
        # The other count is stale from an earlier draw, so only the drawn one decides.
        self.assertIn(
            "draw_count = is_indexed ? draw.index_buffer.count : draw.vertex_buffer.count", graphics
        )
        self.assertIn("CanSkipPendingGraphicsPso(\n                skip_pending, draw_count,", graphics)
        self.assertIn("maxwell3d->regs.transform_feedback_enabled != 0", graphics)
        self.assertIn("if (!may_skip && pso_waited.insert(request_hash).second)", graphics)
        # Only Ready results enter the guest cache; pending/missing remain retryable.
        after_wait = graphics.split("if (settled.status == AsyncCache::Status::Ready)", 1)[1]
        self.assertNotIn("StoreGuestGraphics", after_wait.split("return settled.result", 1)[1])
        emitter = (ROOT / "src/shader_recompiler/backend/hlsl/emit_hlsl.cpp").read_text()
        self.assertIn("[loop] while (true)", emitter)
