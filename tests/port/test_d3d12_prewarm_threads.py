# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise temporary prewarm resizing, admission and stalled worker teardown."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <thread>
#include "video_core/renderer_d3d12/d3d12_async_pso.h"
using namespace std::chrono_literals;
using Pool = D3D12::AsyncPsoWorkerPool<int, int>;
template <typename Predicate>
void Until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
int main() {
    using D3D12::ParsePrewarmThreads;
    for (unsigned i = 1; i <= 6; ++i)
        assert(ParsePrewarmThreads(std::to_string(i)) == i);
    for (auto bad : {"", "0", "7", "-1", "+4", "4 ", " 4", "4x", "1.5", "junk",
                     "999999999999999999999999999999999"})
        assert(ParsePrewarmThreads(bad) == 2);
    unsetenv("NXBOX_D3D12_PREWARM_THREADS");
    assert(D3D12::PrewarmThreads() == 2);
    setenv("NXBOX_D3D12_PREWARM_THREADS", "4", 1);
    assert(D3D12::PrewarmThreads() == 4);
    setenv("NXBOX_D3D12_PREWARM_THREADS", "invalid", 1);
    assert(D3D12::PrewarmThreads() == 2);

    Pool pool;
    pool.SetWorkerTarget(4);
    assert(pool.WorkerCount() == 4);
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::atomic<int> entered{}, running{}, maximum{};
    auto job = [&] {
        const int count = ++running;
        int old = maximum.load();
        while (old < count && !maximum.compare_exchange_weak(old, count)) {
        }
        ++entered;
        gate.wait();
        --running;
        return std::make_shared<int>(1);
    };
    for (int i = 0; i < 8; ++i)
        assert(pool.Submit(i, job));
    Until([&] { return entered == 4; });
    assert(maximum == 4);
    assert(!pool.CanSubmit());
    assert(!pool.Submit(8, job));
    pool.SetWorkerTarget(2);
    assert(pool.WorkerCount() == 4); // Busy workers finish before retiring.
    release.set_value();
    for (int i = 0; i < 8; ++i)
        assert(pool.WaitFor(i, 2s).status == Pool::Status::Ready);
    Until([&] { return pool.WorkerCount() == 2; });
    assert(pool.Idle());

    std::promise<void> release_base;
    auto base_gate = release_base.get_future().share();
    entered = running = maximum = 0;
    auto base_job = [&] {
        const int count = ++running;
        int old = maximum.load();
        while (old < count && !maximum.compare_exchange_weak(old, count)) {
        }
        ++entered;
        base_gate.wait();
        --running;
        return std::make_shared<int>(2);
    };
    for (int i = 10; i < 14; ++i)
        assert(pool.Submit(i, base_job));
    Until([&] { return entered == 2; });
    assert(!pool.CanSubmit()); // Original capacity is four again.
    assert(!pool.Submit(14, base_job));
    release_base.set_value();
    for (int i = 10; i < 14; ++i)
        assert(pool.WaitFor(i, 2s).status == Pool::Status::Ready);
    assert(maximum == 2);
    // Retired slots are safe to reuse, including both target clamps.
    pool.SetWorkerTarget(99);
    assert(pool.WorkerCount() == 6);
    pool.SetWorkerTarget(0);
    Until([&] { return pool.WorkerCount() == 1; });
    pool.SetWorkerTarget(4);
    assert(pool.WorkerCount() == 4);
    assert(pool.Submit(20, [] { return std::make_shared<int>(20); }));
    assert(pool.WaitFor(20, 2s).status == Pool::Status::Ready);
    assert(pool.Stop() == 0);
    pool.SetWorkerTarget(6);
    assert(!pool.CanSubmit());

    // A raised target retains the constructor's allowance for stalled replacements.
    Pool stalled{2, 4, 20ms, 4};
    stalled.SetWorkerTarget(4);
    std::promise<void> started, unblock;
    auto stuck_gate = unblock.get_future().share();
    assert(stalled.Submit(1, [&] {
        started.set_value();
        stuck_gate.wait();
        return std::make_shared<int>(1);
    }));
    started.get_future().wait();
    Until([&] { return stalled.Stalled() == 1; });
    assert(stalled.CanSubmit());
    assert(stalled.WorkerCount() == 5);
    assert(stalled.Submit(2, [] { return std::make_shared<int>(2); }));
    assert(stalled.WaitFor(2, 2s).status == Pool::Status::Ready);
    stalled.SetWorkerTarget(2);
    Until([&] { return stalled.WorkerCount() == 3; });
    assert(stalled.Idle());
    assert(stalled.Stop() == 1);
    assert(stalled.Stop() == 0);
    unblock.set_value();
    assert(stalled.WaitFor(1, 2s).status == Pool::Status::Ready);
}
"""


class PrewarmThreadTests(unittest.TestCase):
    def test_resize_and_policy(self):
        with tempfile.TemporaryDirectory(dir=ROOT) as directory:
            source = Path(directory) / "prewarm.cpp"
            binary = Path(directory) / "prewarm"
            source.write_text(DRIVER)
            result = subprocess.run(
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
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_prewarm_wiring(self):
        source = (ROOT / "src/video_core/renderer_d3d12/d3d12_pipeline_cache.cpp").read_text()
        load = source.split("void PipelineCache::LoadDiskResources(", 1)[1].split(
            "void PipelineCache::BindToChannel(", 1
        )[0]
        guard = load.split("SCOPE_EXIT {", 1)[1].split("};", 1)[0]
        self.assertIn("if (preload)", guard)
        self.assertIn("async->SetWorkerTarget(2)", guard)
        self.assertIn("async_warmup && wait_for_warmup", load)
        self.assertLess(load.index("SCOPE_EXIT {"), load.index("SetWorkerTarget(threads)"))
        self.assertLess(load.index("SetWorkerTarget(threads)"), load.index("disk->StartWarmup("))
        self.assertIn("const size_t threads = PrewarmThreads();", load)
        self.assertIn('Diagnostic(fmt::format("D3D12 prewarm threads={}", threads))', load)
        completion = load.split("if (finished && pending == 0)", 1)[1]
        self.assertLess(
            completion.index("async->SetWorkerTarget(2)"),
            completion.index("callback(VideoCore::LoadCallbackStage::Complete"),
        )
        self.assertIn("disk->WaitWarmup();", completion)
        self.assertIn("DiskWarmupStage::Cancelled", load)
        self.assertIn("DiskWarmupStage::Failed", load)
        self.assertIn("warmup_pool->SetWorkerTarget(2)", load)
        self.assertIn("!CompileHeadroomAvailable(false)", load)
