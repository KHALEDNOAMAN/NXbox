"""Guest uniforms use root CBVs without per-draw descriptor writes or copies."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/video_core/renderer_d3d12"


def source(name):
    return (BACKEND / name).read_text()


class D3D12RootCbv(unittest.TestCase):
    def test_guest_layout_matches_hlsl_registers(self):
        root = source("d3d12_root_signature.cpp")
        self.assertIn("D3D12_ROOT_PARAMETER_TYPE_CBV", root)
        self.assertIn("p.Descriptor = {i, 0};", root)
        self.assertIn("params[kFirstRootCbv + i]", root)
        self.assertIn("Constants = {0, 1, kRootConstantDwords}", root)
        hlsl = (ROOT / "src/shader_recompiler/backend/hlsl/hlsl_emit_context.cpp").read_text()
        self.assertIn("register(b{}, space0)", hlsl)
        self.assertIn("bindings.uniform_buffer++", hlsl)
        pipelines = source("d3d12_pipeline_cache.cpp")
        self.assertEqual(pipelines.count("->SetRootCbvs("), 6)  # Runtime and prewarm builds.
        self.assertIn("CreateRootSignature(device, total, UseRootCbvs(total.cbv))", pipelines)
        self.assertIn("CreateRootSignature(context.Device(), counts, key.second)", pipelines)

    def test_64_dword_fallback(self):
        header = source("d3d12_root_signature.h")
        constants = dict(re.findall(r"constexpr u32 (k\w+) = (\d+);", header))
        limit = int(constants["kRootSignatureDwordLimit"])
        words = int(constants["kRootConstantDwords"])
        self.assertEqual(limit, 64)
        self.assertEqual(words + 3 + 2 * 22, 63)
        self.assertGreater(words + 3 + 2 * 23, limit)
        self.assertIn("count <= (kRootSignatureDwordLimit - kRootConstantDwords - 3) / 2", header)
        self.assertIn(
            "root_cbvs = root_cbvs && UseRootCbvs(counts.cbv)", source("d3d12_root_signature.cpp")
        )

    def test_null_slots_alignment_and_state(self):
        rasterizer = source("d3d12_rasterizer.cpp")
        self.assertIn(
            "zero_uniform_buffer = context.CreateBuffer(65536, D3D12_HEAP_TYPE_UPLOAD)", rasterizer
        )
        self.assertIn("zero_uniform_buffer->mapped.end(), u8{0}", rasterizer)
        self.assertEqual(rasterizer.count("buffer = {zero_uniform_buffer, 0, 65536}"), 2)
        buffers = source("d3d12_buffer_cache.cpp")
        self.assertIn("(offset & 255)", buffers)
        self.assertIn("CopyUniform({std::move(resource), offset, size})", buffers)
        for body in (
            rasterizer.split("// Constant buffers, in the order")[1].split("// Textures")[0],
            rasterizer.split("for (u32 i = 0; i < (root_cbvs ? counts.cbv")[1].split(
                "u32 srv_index"
            )[0],
        ):
            self.assertIn("!buffer.resource || !buffer.size", body)
            self.assertIn("buffer_runtime.CopyUniform(buffer)", body)
            self.assertIn("D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER", body)
            self.assertRegex(body, r"else\s*\{\s*descriptors.WriteCbv\(")

    def test_no_cbv_descriptors_in_root_mode(self):
        rasterizer = source("d3d12_rasterizer.cpp")
        self.assertIn("root_cbvs ? 0u : TableSize(totals.cbv)", rasterizer)
        self.assertIn("root_cbvs ? 0u : TableSize(counts.cbv)", rasterizer)
        self.assertIn("SetGraphicsRootConstantBufferView(kFirstRootCbv + i", rasterizer)
        dispatch = source("d3d12_dispatch.cpp")
        self.assertIn("SetComputeRootConstantBufferView(kFirstRootCbv + i", dispatch)
        self.assertIn("i = bindings.root_cbvs ? 1u : 0u", dispatch)
        self.assertIn("FrameCount::RootCbvBinds", rasterizer)
        self.assertIn("FrameCount::RootCbvBinds", dispatch)
        self.assertIn('"root_cbv_binds"', source("d3d12_context.cpp"))
        helper = source("d3d12_compute_pass.cpp")
        self.assertIn("CreateComputeRootSignature", helper)
        self.assertNotIn("root_cbvs = true", helper)

    def test_cache_layout_identity_and_version(self):
        pipelines = source("d3d12_pipeline_cache.cpp")
        self.assertIn("std::pair{counts, UseRootCbvs(counts.cbv)}", pipelines)
        self.assertIn("Append32(bytes, 4)", pipelines)
        self.assertIn("Append32(bytes, UseRootCbvs(cbv_count) ? 1u : 0u)", pipelines)
        self.assertIn("root_cbvs != UseRootCbvs(cbv_count)", pipelines)
        self.assertIn("StageBindingAbi = 10", pipelines)
        self.assertIn("QueryIdentity(context.Device(), {4, 2, 6})", pipelines)
        self.assertIn("FormatVersion = 3", source("d3d12_disk_pipeline_cache.cpp"))
        self.assertIn("Pso::HashBytes(recipe.data(), recipe.size(), request)", pipelines)


if __name__ == "__main__":
    unittest.main()
