# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""A draw that records a query keeps the recorded draw state, except the stream-output targets."""

import re
import unittest
from pathlib import Path

from test_d3d12_copy_write_hazards import function_body

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"


class QueryEndStateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.queries = (BACKEND / "d3d12_query_cache.cpp").read_text()
        cls.end_draw = function_body(cls.queries, "void QueryCacheRuntime::EndDraw")
        cls.feedback = (BACKEND / "d3d12_transform_feedback.cpp").read_text()
        cls.rasterizer = (BACKEND / "d3d12_rasterizer.cpp").read_text()

    def test_end_draw_keeps_the_cache(self):
        # Nearly every draw records a query; a blanket invalidation here made
        # redundant_state_skips 0 in production and re-recorded every call after every draw.
        self.assertNotIn("InvalidateRecordedState", self.end_draw)

    def test_end_draw_forgets_only_the_stream_output_targets(self):
        forgotten = re.findall(r"DrawState\(\)\.(\w+) = \{\};", self.end_draw)
        self.assertEqual(forgotten, ["stream_output"])

    def test_forgetting_stream_output_makes_the_next_draw_issue_sosettargets(self):
        # The forced SOSetTargets is what ends vkd3d-proton's render pass after a query draw.
        for name in (
            "void TransformFeedback::Bind(bool cache_draw_state)",
            "void TransformFeedback::Unbind(bool reset_cursor, bool cache_draw_state)",
        ):
            with self.subTest(function=name):
                body = function_body(self.feedback, name)
                self.assertRegex(
                    body,
                    r"if \(context\.RecordState\(\s*context\.DrawState\(\)\.stream_output\.Update"
                    r"\([^;]*\)\)\)\s*commands->SOSetTargets\(0, 4, ",
                )
        draw = self.rasterizer.split("// ---- 4. Record state", 1)[1]
        self.assertIn("transform_feedback.Bind(true);", draw)
        self.assertIn(
            "transform_feedback.Unbind(regs.transform_feedback_enabled == 0, true);", draw
        )

    def test_work_that_changes_graphics_state_still_invalidates_everything(self):
        # The compute resolve rebinds root signature, pipeline and heaps behind the cache.
        resolve = self.queries.split("void DispatchResolve(", 1)[1].split(
            "context.Transition(results", 1
        )[0]
        self.assertIn("context.InvalidateRecordedState();", resolve)


if __name__ == "__main__":
    unittest.main()
