# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""NXBOX_D3D12_SWAPCHAIN_BUFFERS picks two or three back buffers; every buffer use follows it."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RENDERER = ROOT / "src/video_core/renderer_d3d12/renderer_d3d12.cpp"


class SwapchainBuffersTests(unittest.TestCase):
    def setUp(self):
        self.source = RENDERER.read_text()

    def test_knob_only_accepts_three(self):
        knob = self.source.split("static UINT SwapchainBuffers() {", 1)[1].split("\n}", 1)[0]
        self.assertIn('"NXBOX_D3D12_SWAPCHAIN_BUFFERS"', knob)
        self.assertIn("return length == 1 && value[0] == '3' ? 3 : 2;", knob)

    def test_buffer_uses_follow_the_knob(self):
        self.assertNotIn("BufferCount = 2", self.source)
        self.assertIn("const UINT buffer_count = SwapchainBuffers();", self.source)
        for use in ("desc.BufferCount = buffer_count;", "ResizeBuffers(buffer_count,",
                    "for (UINT i = 0; i < buffer_count; ++i)", "if (index < buffer_count)"):
            self.assertIn(use, self.source)
        # Storage and descriptor slices cover the largest count; the screenshot target uses the
        # slot after them, so it never shares a slice with a back buffer.
        self.assertIn("std::array<ResourcePtr, MaxBuffers> buffers;", self.source)
        self.assertIn("std::array<u64, MaxBuffers> present_ticks{};", self.source)
        self.assertEqual(len(re.findall(r"NumDescriptors = (?:MaxLayers \* \()?MaxBuffers \+ 1", self.source)), 2)
        self.assertEqual(self.source.count("Rtv(MaxBuffers)"), 2)


if __name__ == "__main__":
    unittest.main()
