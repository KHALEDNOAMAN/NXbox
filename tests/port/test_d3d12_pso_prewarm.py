# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep boot prewarming connected to the guest-keyed async cache."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


class PsoPrewarmTests(unittest.TestCase):
    def test_request_schema_async_boot_and_documented_toggle(self):
        header = (D3D12 / "d3d12_disk_pipeline_cache.h").read_text()
        wire = (D3D12 / "d3d12_disk_pipeline_cache.cpp").read_text()
        source = (D3D12 / "d3d12_pipeline_cache.cpp").read_text()
        loader = source.split("void PipelineCache::LoadDiskResources", 1)[1].split(
            "void PipelineCache::BindToChannel", 1
        )[0]
        self.assertIn("DiskBytes request_key", header)
        self.assertIn("DiskBytes runtime_data", header)
        self.assertIn("FormatVersion = 3", wire)
        self.assertIn("record.request_key.begin()", wire)
        self.assertIn("record.request_key.assign", wire)
        self.assertNotIn("if (AsyncPsoEnabled())", loader)
        self.assertIn("const bool async_warmup = AsyncPsoEnabled()", loader)
        self.assertIn("disk->StartWarmup(", loader)
        self.assertIn("async->Submit(", loader)
        self.assertIn("BuildPrewarmedPipeline(", loader)
        self.assertIn("stalled_requests.contains(hash)", loader)
        self.assertIn('BisectEnabled("NXBOX_D3D12_NO_PREWARM")', loader)
        self.assertIn(
            "NXBOX_D3D12_NO_PREWARM", (ROOT / "docs/d3d12-backend.md").read_text()
        )
        self.assertIn("FrameCount::PrewarmedPsoHits", source)
        # A pending prewarm still gets the bounded first-use wait; a failed one is rebuilt.
        self.assertNotIn(".prewarmed && pso_waited", source)
        self.assertIn("ForgetFailedPrewarm(request)", source)
        self.assertIn("ForgetFailedPrewarm(async_key)", source)
        # Compute records use the same key as the runtime lookup.
        self.assertEqual(source.count("key.state.sample_count = 0; // Same key as CurrentComputePipeline."), 2)


if __name__ == "__main__":
    unittest.main()
