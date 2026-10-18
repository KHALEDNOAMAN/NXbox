// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_flush_policy.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

using D3D12::DrawFlushCadence;
using D3D12::ParseFlushDraws;

// The production helpers are constexpr, so the contract is also checked at
// compile time.
static_assert(ParseFlushDraws("128") == 128);
static_assert(ParseFlushDraws("0") == 0);
static_assert(ParseFlushDraws("-4") == 0);
static_assert(ParseFlushDraws("4294967295") == 4294967295u);
static_assert(ParseFlushDraws("4294967296") == 0);

namespace {
// What the draw path does for each guest draw: ask Due() before the draw is
// touched, cut the list if it is, record the draw (Drew), and let anything else
// that submits (a guest fence) reset the count. A skipped draw is never
// recorded, so it never counts, but it still passes the check.
struct Step {
  bool skipped;
  bool external_submit_after;
};
struct Replay {
  std::vector<std::size_t> lists; // draws per submitted list, the open one last
  std::size_t flushes{};
};
Replay Run(std::uint32_t every, const std::vector<Step> &steps) {
  DrawFlushCadence cadence{every};
  Replay result;
  result.lists.push_back(0);
  for (const auto &step : steps) {
    if (cadence.Due()) {
      cadence.Submitted();
      ++result.flushes;
      result.lists.push_back(0);
    }
    if (!step.skipped) {
      cadence.Drew();
      ++result.lists.back();
    }
    if (step.external_submit_after) {
      cadence.Submitted();
      result.lists.push_back(0);
    }
  }
  return result;
}
std::vector<Step> Draws(std::size_t count) {
  return std::vector<Step>(count, Step{false, false});
}
} // namespace

int main() {
  // Unset, empty and garbage leave the knob off; so do signs, hex, fractions
  // and trailing text.
  for (const char *off :
       {"", "0", "00", "-1", "-0", "+5", "abc", "12x", "x12", "1.5", "0x10",
        "1 2", " ", "4294967296", "99999999999999999999"})
    assert(ParseFlushDraws(off) == 0);
  assert(ParseFlushDraws(nullptr) == 0);
  assert(ParseFlushDraws("1") == 1);
  assert(ParseFlushDraws("32") == 32);
  assert(ParseFlushDraws("128") == 128);
  assert(ParseFlushDraws("512") == 512);
  assert(ParseFlushDraws("007") == 7);
  assert(ParseFlushDraws(" 64\t") ==
         64); // cmd.exe `set X=64 && app` keeps the blank

  // Off: one list however many draws, and Due() never fires.
  {
    const auto off = Run(0, Draws(2000));
    assert(off.flushes == 0 && off.lists == std::vector<std::size_t>{2000});
  }
  // The heavy Odyssey frame: 576 draws.
  {
    const auto every32 = Run(32, Draws(576));
    assert(every32.flushes == 17);
    assert(every32.lists.size() == 18);
    for (const auto size : every32.lists)
      assert(size == 32);
    const auto every128 = Run(128, Draws(576));
    assert(
        (every128.lists == std::vector<std::size_t>{128, 128, 128, 128, 64}));
    assert(every128.flushes == 4);
    const auto every512 = Run(512, Draws(576));
    assert((every512.lists == std::vector<std::size_t>{512, 64}));
    assert(every512.flushes == 1);
  }
  // Exactly N draws leave no empty trailing flush: the check runs before the
  // next draw.
  {
    const auto exact = Run(4, Draws(8));
    assert((exact.lists == std::vector<std::size_t>{4, 4}));
    assert(exact.flushes == 1);
  }
  // N larger than the stream never fires; N = 1 cuts before every draw but the
  // first.
  assert(Run(1000, Draws(576)).flushes == 0);
  {
    const auto one = Run(1, Draws(5));
    assert(one.flushes == 4);
    for (const auto size : one.lists)
      assert(size == 1);
  }
  // A guest fence submits on its own: the next list restarts at zero instead of
  // being cut early.
  {
    auto steps = Draws(100);
    steps[19].external_submit_after = true; // after the 20th draw
    const auto fenced = Run(32, steps);
    assert((fenced.lists == std::vector<std::size_t>{20, 32, 32, 16}));
    assert(fenced.flushes == 2);
  }
  // Skipped draws do not advance the cadence: 6 recorded draws make two lists
  // of 3, and the skipped draw that follows the third one is the call that cuts
  // it.
  {
    std::vector<Step> steps;
    for (std::size_t i = 0; i < 12; ++i)
      steps.push_back(Step{i % 2 == 1, false});
    const auto skipped = Run(3, steps);
    assert((skipped.lists == std::vector<std::size_t>{3, 3, 0}));
    assert(skipped.flushes == 2);
  }
  std::cout << "flush draws parse+cadence ok\n";
}
