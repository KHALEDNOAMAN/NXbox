# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the production flush policy and check how the context and the draw path use it."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "src/video_core/renderer_d3d12"


class FlushDrawsTests(unittest.TestCase):
    def test_parse_and_cadence(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "flush-draws"
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
                    str(ROOT / "tests/port/d3d12_flush_draws_test.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_counter_sits_at_its_name(self):
        # The profile names are positional; the static_assert only checks the sizes.
        header = (NATIVE / "d3d12_context.h").read_text()
        source = (NATIVE / "d3d12_context.cpp").read_text()
        enum = header.split("enum class FrameCount : size_t {", 1)[1].split("};", 1)[0]
        members = [member.strip() for member in enum.split(",") if member.strip()]
        self.assertEqual(members[-1], "Count")
        names = re.findall(
            r'"([^"]+)"', source.split("constexpr std::array counters{", 1)[1].split("};", 1)[0]
        )
        self.assertEqual(len(names), len(members) - 1)
        self.assertEqual(
            members.index("ForcedSubmitFlushDraws"), names.index("forced_submits_flush_draws")
        )

    def test_context_counts_draws_and_resets_on_every_submit(self):
        header = (NATIVE / "d3d12_context.h").read_text()
        source = (NATIVE / "d3d12_context.cpp").read_text()
        self.assertIn("DrawFlushCadence flush_cadence{FlushDrawsKnob()};", header)
        self.assertIn('GetEnvironmentVariableA("NXBOX_D3D12_FLUSH_DRAWS"', source)
        self.assertIn("ParseFlushDraws(value)", source)
        self.assertIn('Diagnostic(fmt::format("D3D12 flush_draws={}"', source)
        mark = source.split("void Context::MarkWork", 1)[1].split("u64 Context::Submit", 1)[0]
        self.assertLess(
            mark.index("profile.Count(FrameCount::Draws)"), mark.index("flush_cadence.Drew()")
        )
        submit = source.split("u64 Context::Submit", 1)[1].split(
            "bool Context::FlushDrawsIfDue", 1
        )[0]
        # Before the early return for an empty list: whatever submits, the next list starts at 0.
        self.assertLess(submit.index("flush_cadence.Submitted()"), submit.index("if (!recording)"))
        flush = source.split("bool Context::FlushDrawsIfDue", 1)[1].split(
            "void Context::Finish", 1
        )[0]
        self.assertIn("flush_cadence.Due()", flush)
        self.assertIn("Submit(FrameCount::ForcedSubmitFlushDraws)", flush)

    def test_draw_path_cuts_between_draws(self):
        source = (NATIVE / "d3d12_rasterizer.cpp").read_text()
        draw = source.split("void RasterizerD3D12::PrepareDraw(bool is_indexed, Func&& draw) {", 1)[
            1
        ]
        head = draw.split('VideoCommon::SubmissionTrace::Log("host_draw_begin', 1)[0]
        # First thing a draw does, before any state is resolved. The fencing thread is woken like
        # FlushCommands does, so a deferred fence tick in this list is not left to the backstop.
        self.assertIn("if (context.FlushDrawsIfDue())", head)
        self.assertIn("fence_manager.NotifyDeferredSubmit();", head)
        self.assertEqual(source.count("FlushDrawsIfDue()"), 1)


if __name__ == "__main__":
    unittest.main()
