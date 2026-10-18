# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Guest draw state is reused only within an unchanged command-list generation."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"


class RedundantStateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.context = (BACKEND / "d3d12_context.cpp").read_text()
        cls.header = (BACKEND / "d3d12_context.h").read_text()
        cls.state = (BACKEND / "d3d12_recorded_state.h").read_text()
        cls.rasterizer = (BACKEND / "d3d12_rasterizer.cpp").read_text()
        cls.draw = cls.rasterizer.split("// ---- 4. Record state", 1)[1].split(
            "// drop upload-page references", 1
        )[0]

    def test_cache_is_owned_by_context_and_checks_generation(self):
        self.assertIn("struct RecordedDrawState", self.state)
        self.assertIn("RecordedDrawState recorded_state;", self.header)
        self.assertIn("recorded_state.Refresh(recording_generation);", self.header)
        self.assertIn("if (generation != current_generation)", self.state)
        self.assertIn("*this = {};", self.state)
        self.assertIn("generation = current_generation;", self.state)

    def test_command_list_start_and_submission_invalidate(self):
        start = self.context.split("Context::Commands()", 1)[1].split("Context::MarkWork", 1)[0]
        self.assertLess(
            start.index('"command list Reset"'), start.index("InvalidateRecordedState();")
        )
        submit = self.context.split("Context::Submit(", 1)[1].split("Context::Finish", 1)[0]
        self.assertLess(submit.index("InvalidateRecordedState();"), submit.index("if (!recording)"))

    def test_root_signature_change_clears_all_root_arguments(self):
        binding = self.draw.split("recorded.root_signature.Update(root_signature)", 1)[1]
        binding = binding.split("recorded.pipeline.Update", 1)[0]
        self.assertLess(
            binding.index("SetGraphicsRootSignature(root_signature)"),
            binding.index("recorded.ClearRootArguments();"),
        )
        self.assertIn("root_args = {};", self.state)
        self.assertIn("root_args.tables[parameter].Update(handle.ptr)", self.draw)
        self.assertIn("root_args.cbvs[kFirstRootCbv + i].Update(root_cbv_addresses[i])", self.draw)
        self.assertIn("root_args.constants[0].Update(words.data(), words.size())", self.draw)
        self.assertIn("std::memcmp(values.data(), next, next_count * sizeof(T))", self.state)

    def test_all_draw_state_calls_are_cached_and_draw_is_emitted(self):
        for name in (
            "pipeline",
            "topology",
            "targets",
            "viewports",
            "scissors",
            "vertices",
            "index",
            "stencil",
            "blend",
            "depth_bounds",
        ):
            with self.subTest(state=name):
                self.assertIn(f"context.RecordState(recorded.{name}.Update(", self.draw)
        self.assertIn("descriptors.SetHeaps(commands, true);", self.draw)
        self.assertIn("draw(commands);", self.draw)
        self.assertNotIn("if (context.RecordState(draw", self.draw)
        feedback = (BACKEND / "d3d12_transform_feedback.cpp").read_text()
        self.assertEqual(feedback.count("context.DrawState().stream_output.Update("), 2)

    def test_heap_changes_and_fixed_descriptor_rewrites_invalidate(self):
        heaps = (
            (BACKEND / "d3d12_descriptor.cpp")
            .read_text()
            .split("D3D12DescriptorAllocator::SetHeaps", 1)[1]
            .split("D3D12DescriptorAllocator::Allocate", 1)[0]
        )
        self.assertIn("state.heaps.Update(heaps)", heaps)
        self.assertIn("state.ClearRootArguments();", heaps)
        self.assertIn("context.InvalidateRecordedState();", heaps)
        targets = self.rasterizer.split("if (targets != bound_targets)", 1)[1]
        self.assertLess(targets.index("InvalidateRecordedState();"), targets.index("CreateRtv("))

    def test_state_changing_work_always_invalidates_even_for_repeated_labels(self):
        work = self.context.split("Context::MarkWork", 1)[1].split("Context::Submit", 1)[0]
        # Copies, uploads and queries leave graphics state alone and must not reset the cache.
        for label in ("guest draws", "upload/copy", "queries"):
            self.assertIn(f'"{label}"', work.split("InvalidateRecordedState();", 1)[0])
        neutral = work.split("state_neutral{", 1)[1].split("};", 1)[0]
        self.assertNotIn('"guest compute"', neutral)
        # Opt-in compute retains compute roots but still invalidates graphics PSO state.
        self.assertIn('BisectEnabled("NXBOX_D3D12_FAST_ROOT_BINDS")', work)
        self.assertIn('label == "guest compute"', work)
        self.assertLess(work.index("InvalidateGraphicsState();"), work.index("} else if"))
        self.assertLess(
            work.index("InvalidateRecordedState();"), work.index("std::find(work.begin()")
        )
        for filename in (
            "d3d12_blit_image.cpp",
            "d3d12_clear.cpp",
            "d3d12_dispatch.cpp",
            "d3d12_present/present.cpp",
            "d3d12_query_cache.cpp",
        ):
            with self.subTest(path=filename):
                self.assertIn(
                    "context.InvalidateRecordedState();", (BACKEND / filename).read_text()
                )

    def test_skips_have_a_frame_counter(self):
        self.assertIn("RedundantStateSkips,", self.header)
        self.assertIn("profile.Count(FrameCount::RedundantStateSkips);", self.header)
        self.assertIn('"redundant_state_skips"', self.context)


if __name__ == "__main__":
    unittest.main()
