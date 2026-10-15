"""Overlapping buffer copy writes must be ordered on the current D3D12 command list."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "src/video_core/renderer_d3d12"


def function_body(source, signature):
    start = source.index("{", source.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start + 1 : end - 1]


class CopyWriteHazards(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = (D3D12 / "d3d12_context.h").read_text()
        cls.context = (D3D12 / "d3d12_context.cpp").read_text()

    def test_overlap_records_one_global_aliasing_barrier(self):
        self.assertIn(
            "void BeforeCopyWrite(const ResourcePtr& resource, u64 offset, u64 size);", self.header
        )
        self.assertIn(
            "std::unordered_map<ID3D12Resource*, std::vector<CopyWriteRange>> copy_writes;",
            self.header,
        )
        body = function_body(self.context, "void Context::BeforeCopyWrite(")
        self.assertIn("if (!size)", body)
        self.assertLess(
            body.index("Retain(resource);"), body.index("copy_writes.find(destination)")
        )
        self.assertIn("offset < range.offset ? range.offset - offset < size", body)
        self.assertIn("offset - range.offset < range.size", body)
        self.assertLess(body.index("if (!overlaps)"), body.index("ResourceBarrier(1, &barrier)"))
        self.assertEqual(body.count("ResourceBarrier(1, &barrier)"), 1)
        self.assertIn("barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;", body)
        self.assertIn("barrier.Aliasing.pResourceBefore = nullptr;", body)
        self.assertIn("barrier.Aliasing.pResourceAfter = nullptr;", body)
        self.assertIn("profile.Count(FrameCount::CopyHazardBarriers);", body)
        self.assertIn("profile.Count(FrameCount::Barriers);", body)
        self.assertRegex(body, r"copy_writes\.clear\(\);\s*break;")
        self.assertLess(
            body.index("copy_writes.clear();"),
            body.index("copy_writes[destination].push_back({offset, size});"),
        )

    def test_counter_is_printed_with_frame_counts(self):
        self.assertIn("CopyHazardBarriers,", self.header)
        self.assertIn('"copy_hazard_barriers"', self.context)

    def test_tracking_is_scoped_to_recording(self):
        submit = function_body(self.context, "u64 Context::Submit(")
        self.assertRegex(submit, r"work\.clear\(\);\s*copy_writes\.clear\(\);")
        commands = function_body(self.context, "ID3D12GraphicsCommandList* Context::Commands(")
        self.assertLess(commands.index("commands->Reset("), commands.index("copy_writes.clear();"))
        self.assertLess(commands.index("copy_writes.clear();"), commands.index("recording = true;"))

    def test_only_emitted_transitions_erase_resource_ranges(self):
        transition = function_body(self.context, "void Context::Transition(")
        self.assertLess(transition.index("return;"), transition.index("copy_writes.erase("))
        self.assertLess(
            transition.index("ResourceBarrier(1, &barrier)"),
            transition.index("copy_writes.erase(resource->handle.Get());"),
        )
        batch = function_body(self.context, "void Context::TransitionBatch(")
        self.assertLess(batch.index("continue;"), batch.index("copy_writes.erase("))
        self.assertLess(
            batch.index("list->ResourceBarrier("),
            batch.index("copy_writes.erase(barrier.Transition.pResource);"),
        )

    def test_every_buffer_copy_has_an_immediate_destination_guard(self):
        expected = {
            "d3d12_buffer_cache.cpp": ["probe, 0, size", "dst, copy.dst_offset, copy.size"],
            "d3d12_rasterizer.cpp": [
                "source_readback, 0, probe",
                "padded, 0, copied",
                "padded, 0, copied",
                "padded, copied, required - copied",
                "readback, 0, 4",
            ],
            "d3d12_query_cache.cpp": [
                "result.buffer, result.offset, 8",
                "resource, destination, size",
                "impl->predicate, 0, 8",
                "impl->predicate, 0, 8",
            ],
            "d3d12_transform_feedback.cpp": [
                "counters, 0, 32",
                "counters, u64{slot} * 8, 8",
                "result.buffer, result.offset, 8",
            ],
            "d3d12_texture_cache.cpp": [
                "gpu_destination, destination, row_bytes",
                "gpu_destination, destination, size",
                "aligned.buffer, aligned.offset, padded_size",
            ],
            "d3d12_compute_pass.cpp": ["input, 0, size"],
        }
        for filename, arguments in expected.items():
            with self.subTest(filename=filename):
                source = re.sub(r"\s+", " ", (D3D12 / filename).read_text())
                copies = list(
                    re.finditer(r"(?:impl->)?context\.Commands\(\)->CopyBufferRegion\(", source)
                )
                self.assertEqual(len(copies), len(arguments))
                for copy, argument in zip(copies, arguments):
                    self.assertTrue(
                        source[: copy.start()].rstrip().endswith(f"BeforeCopyWrite({argument});"),
                        f"{filename}: missing immediate guard for {argument}",
                    )
                # The optional byte trace has its own disjoint readback slots.
                trace_copies = source.count("commands->CopyBufferRegion(")
                self.assertEqual(trace_copies, 1 if filename == "d3d12_rasterizer.cpp" else 0)

    def test_texture_downloads_guard_buffer_footprints(self):
        source = re.sub(r"\s+", " ", (D3D12 / "d3d12_texture_cache.cpp").read_text())
        for argument, destination in (
            ("padded_gpu ? padded_gpu : aligned.buffer, footprint.Offset, padded_size", "linear"),
            ("aligned.buffer, footprint.Offset, padded_slice * extent.depth", "linear"),
            (
                (
                    "bounce, step.read.Offset, u64{step.read.Footprint.RowPitch} * "
                    "step.read.Footprint.Height * step.read.Footprint.Depth"
                ),
                "out",
            ),
        ):
            with self.subTest(argument=argument):
                self.assertIn(
                    f"context.BeforeCopyWrite({argument}); "
                    f"context.Commands()->CopyTextureRegion(&{destination},",
                    source,
                )
        screenshot = re.sub(r"\s+", " ", (D3D12 / "renderer_d3d12.cpp").read_text())
        self.assertIn(
            "context.BeforeCopyWrite(readback, footprint.Offset, total_bytes); "
            "context.Commands()->CopyTextureRegion(&destination,",
            screenshot,
        )

    def test_uniform_data_precedes_disjoint_zero_tail(self):
        source = (D3D12 / "d3d12_buffer_cache.cpp").read_text()
        body = function_body(source, "BoundBuffer BufferCacheRuntime::CopyUniform(")
        self.assertIn("const VideoCommon::BufferCopy data{offset, 0, size};", body)
        tail = function_body(body, "if (padded > size)")
        self.assertIn("upload.Request(padded - size)", tail)
        self.assertIn(
            "const VideoCommon::BufferCopy zero{staging.offset, size, padded - size};", tail
        )
        self.assertIn("CopyBuffer(target, staging.buffer, std::span{&zero, 1}, true);", tail)
        self.assertLess(
            body.index("CopyBuffer(target, resource, std::span{&data, 1}, false);"),
            body.index("if (padded > size)"),
        )
        self.assertNotIn("staging.offset, 0, padded", body)


if __name__ == "__main__":
    unittest.main()
