# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""accelerate_astc=2 decodes D3D12 ASTC off the GPU thread, and a pending decode never outlives its image."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / "src/video_core/texture_cache"
D3D12 = ROOT / "src/video_core/renderer_d3d12"


def function(text, signature):
    start = text.index(signature)
    return text[start : text.index("\n}\n", start)]


class AsyncAstcTests(unittest.TestCase):
    def setUp(self):
        self.cache = (CACHE / "texture_cache.h").read_text()
        self.base = (CACHE / "texture_cache_base.h").read_text()
        d3d12 = (D3D12 / "d3d12_texture_cache.cpp").read_text()
        start = d3d12.index("Image::Image(TextureCacheRuntime& runtime")
        self.ctor = d3d12[start : d3d12.index("void Image::UploadMemory", start)]

    def test_d3d12_queues_converted_astc_only_in_cpu_asynchronous_mode(self):
        rule = self.ctor[self.ctor.index("ImageFlagBits::AsynchronousDecode") - 400 :]
        for needle in (
            "True(flags & VideoCommon::ImageFlagBits::Converted)",
            "False(flags & VideoCommon::ImageFlagBits::AcceleratedUpload)",
            "Settings::AstcDecodeMode::CpuAsynchronous",
        ):
            self.assertIn(needle, rule)
        # The GPU decoder flag is decided first, so the two paths stay exclusive.
        self.assertLess(self.ctor.index("NXBOX_D3D12_GPU_ASTC"),
                        self.ctor.index("ImageFlagBits::AsynchronousDecode"))

    def test_deleting_a_decoding_image_cancels_its_upload(self):
        delete = function(self.cache, "void TextureCache<P>::DeleteImage(")
        guard = delete[delete.index("ImageFlagBits::IsDecoding") :]
        self.assertIn("async_decode->image_id == image_id", guard)
        self.assertIn("async_decode->cancelled = true", guard)
        self.assertLess(delete.index("cancelled = true"), delete.index("slot_images.erase(image_id)"))

    def test_tick_drops_cancelled_decodes_and_never_erases_a_held_mutex(self):
        tick = function(self.cache, "void TextureCache<P>::TickAsyncDecode()")
        cancelled = tick.index("if (async_decode->cancelled)")
        self.assertLess(tick.index("if (!async_decode->complete)"), cancelled)
        self.assertLess(cancelled, tick.index("slot_images[async_decode->image_id]"))
        erases = [m.start() for m in re.finditer(r"async_decodes\.erase\(i\)", tick)]
        self.assertEqual(len(erases), 2)
        for erase in erases:
            self.assertIn("lock.unlock();", tick[erase - 120 : erase])

    def test_decode_worker_is_joined_before_the_contexts_are_freed(self):
        self.assertIn("bool cancelled{};", self.base)
        self.assertLess(self.base.index("std::vector<std::unique_ptr<AsyncDecodeContext>> async_decodes;"),
                        self.base.index("Common::ThreadWorker texture_decode_worker"))


if __name__ == "__main__":
    unittest.main()
