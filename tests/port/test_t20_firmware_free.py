# SPDX-License-Identifier: GPL-3.0-or-later
"""Run Eden's archive readers and Mii implementation without firmware inputs."""

import importlib.util
import math
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "src/core/file_sys/system_archive/data"
spec = importlib.util.spec_from_file_location("t20_resources", DATA / "generate_mii_resources.py")
resources = importlib.util.module_from_spec(spec)
spec.loader.exec_module(resources)

# Only infrastructure is stubbed: logging, numeric filename formatting, UUID entropy,
# host path utilities and unrelated font/timezone suppliers. All VFS/RomFS, database,
# StoreData, CharInfo, CRC and MiiManager implementations below are production sources.
STUBS = {
    "fmt/format.h": "#pragma once\n",
    "common/logging.h": """
#pragma once
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_CRITICAL(...) ((void)0)
""",
    "fmt/ranges.h": """
#pragma once
#include <string>
namespace fmt {
inline std::string format(std::string pattern, std::size_t value) {
    pattern.replace(pattern.find("{}"), 2, std::to_string(value));
    return pattern;
}
}
""",
}

SUPPORT = r"""
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>
#include "common/fs/path_util.h"
#include "common/uuid.h"
#include "core/file_sys/ips_layer.h"
#include "core/file_sys/vfs/vfs_vector.h"
void AssertFailSoftImpl() { std::abort(); }
[[noreturn]] void AssertFatalImpl() { std::abort(); }
namespace Common {
UUID UUID::MakeRandomRFC4122V4() {
    static unsigned counter = 0;
    UUID id{};
    id.uuid[0] = static_cast<u8>(++counter);
    id.uuid[6] = 0x40;
    id.uuid[8] = 0x80;
    return id;
}
}
namespace Common::FS {
std::filesystem::path test_root;
const std::filesystem::path& GetEdenPath(EdenPath) { return test_root; }
bool ValidatePath(const std::filesystem::path& p) { return !p.empty(); }
std::string SanitizePath(std::string_view p, DirectorySeparator) { return std::string(p); }
std::string_view GetPathWithoutTop(std::string_view p) {
    const auto split = p.find_first_of("/\\");
    return split == std::string_view::npos ? std::string_view{} : p.substr(split + 1);
}
std::string GetParentPath(std::string_view p) {
    return std::filesystem::path(p).parent_path().string();
}
std::vector<std::string_view> SplitPathComponents(std::string_view p) {
    std::vector<std::string_view> out;
    while (!p.empty()) {
        const auto split = p.find_first_of("/\\");
        if (split == std::string_view::npos) { out.push_back(p); break; }
        if (split) out.push_back(p.substr(0, split));
        p.remove_prefix(split + 1);
    }
    return out;
}
}
namespace FileSys {
VirtualFile PatchIPS(const VirtualFile&, const VirtualFile&) { std::abort(); }
}
namespace FileSys::SystemArchive {
VirtualDir FontNintendoExtension() { return nullptr; }
VirtualDir FontNintendoExtensionXbox() { return nullptr; }
VirtualDir FontStandard() { return nullptr; }
VirtualDir FontKorean() { return nullptr; }
VirtualDir FontChineseTraditional() { return nullptr; }
VirtualDir FontChineseSimple() { return nullptr; }
VirtualDir TimeZoneBinary() { return nullptr; }
}
"""

DRIVER = r"""
#include <set>
#include "core/file_sys/romfs.h"
#include "core/file_sys/system_archive/system_archive.h"
#include "core/hle/service/ssl/ssl_types.h"
#include "core/hle/service/mii/mii_manager.h"
#include "core/hle/service/mii/mii_database_manager.h"
#include "core/hle/service/mii/types/char_info.h"
#include "core/hle/service/mii/types/store_data.h"
using namespace FileSys;
using namespace Service::Mii;
int main(int argc, char** argv) {
    assert(argc == 2);
    Common::FS::test_root = argv[1];
    for (u64 suffix : {0x800, 0x802, 0x806, 0x809, 0x80b, 0x823}) {
        const auto romfs = SystemArchive::SynthesizeSystemArchive(0x0100000000000000ULL + suffix);
        assert(romfs);
        const auto dir = ExtractRomFS(romfs);
        assert(dir);
        if (suffix == 0x800) {
            Service::SSL::CertStoreHeader header{};
            const auto file = dir->GetFile("ssl_TrustedCerts.bdf");
            assert(file && file->GetSize() == sizeof(header));
            assert(file->ReadObject(&header) == sizeof(header));
            assert(header.magic == 0x546c7373 && header.num_entries == 0);
        } else if (suffix == 0x802) {
            assert(dir->GetFiles().size() == 6);
            for (const auto& file : dir->GetFiles()) {
                const auto bytes = file->ReadAllBytes();
                assert(bytes.size() > 0x1000);
                u32 size{};
                assert(file->ReadObject(&size, 8) == 4 && size == bytes.size());
                const std::filesystem::path path = Common::FS::test_root / file->GetName();
                std::FILE* output = std::fopen(path.string().c_str(), "wb");
                assert(output && std::fwrite(bytes.data(), 1, bytes.size(), output) == bytes.size());
                std::fclose(output);
            }
        } else if (suffix == 0x806) {
            assert(dir->GetFiles().size() == 18);
            for (int i = 0; i < 16; ++i) assert(dir->GetFile(std::to_string(i) + ".txt"));
        } else if (suffix == 0x809) {
            assert(dir->GetFile("file")->GetSize() == 0x100);
        } else if (suffix == 0x80b) {
            assert(dir->GetFiles().empty() && dir->GetSubdirectories().empty());
        } else {
            assert(dir->GetFiles().size() == 52);
        }
    }
    assert(!SystemArchive::SynthesizeSystemArchive(0x01000000000008ffULL));
    DatabaseSessionMetadata metadata{};
    MiiManager manager;
    assert(manager.Initialize(metadata).IsSuccess());
    assert(manager.GetCount(metadata, SourceFlag::Default) == 6);
    assert(manager.GetCount(metadata, SourceFlag::Database) == 1);
    assert(manager.GetCount(metadata, SourceFlag::None) == 0);
    std::array<StoreData, 7> stores{};
    u32 count{};
    assert(manager.Get(metadata, std::span{stores}, count, (SourceFlag::Database | SourceFlag::Default)).IsSuccess());
    assert(count == 7);
    std::set<std::array<u8, 16>> ids;
    for (const auto& store : stores) {
        assert(store.IsValid() == ValidationResult::NoErrors);
        assert(!store.IsSpecial());
        assert(store.GetCreateId().IsValid());
        ids.insert(store.GetCreateId().uuid);
        CharInfo info{};
        info.SetFromStoreData(store);
        assert(info.Verify() == ValidationResult::NoErrors);
    }
    assert(ids.size() == 7);
    // Add five ordinary profiles to exercise filtering, index mapping and persistence.
    for (u32 index = 1; index < 6; ++index) {
        assert(manager.AddOrReplace(metadata, stores[index]).IsSuccess());
    }
    assert(manager.GetCount(metadata, SourceFlag::Database) == 6);
    NintendoFigurineDatabase counted;
    counted.CleanDatabase();
    counted.Add(stores[0]);
    auto special = stores[1];
    special.SetType(1);
    special.SetChecksum();
    counted.Add(special);
    assert(counted.GetCount(metadata) == 1);
    auto privileged = metadata;
    privileged.magic = MiiMagic;
    assert(counted.GetCount(privileged) == 2);
    assert(counted.CheckIntegrity().IsSuccess());
    DatabaseManager indexed;
    bool broken{};
    assert(indexed.MountSaveData().IsSuccess());
    assert(indexed.Initialize(metadata, broken).IsSuccess() && !broken);
    for (u32 index = 0; index < 6; ++index) {
        u32 found = 99;
        assert(indexed.FindIndex(metadata, found, stores[index].GetCreateId()).IsSuccess());
        assert(found == index);
    }

    std::array<CharInfoElement, 12> infos{};
    count = 0;
    assert(manager.Get(metadata, std::span{infos}, count, (SourceFlag::Database | SourceFlag::Default)).IsSuccess());
    assert(count == 12);
    for (int i = 0; i < 12; ++i) {
        assert(infos[i].char_info.Verify() == ValidationResult::NoErrors);
        assert(infos[i].source == (i < 6 ? Source::Database : Source::Default));
    }
    std::array<StoreData, 5> short_buffer{};
    count = 0;
    assert(manager.Get(metadata, std::span{short_buffer}, count, SourceFlag::Default).IsError());
    assert(count == 5);
    // Reload does not duplicate defaults; deletion persists in a nonempty database.
    assert(manager.Delete(metadata, stores[0].GetCreateId()).IsSuccess());
    MiiManager reloaded;
    assert(reloaded.Initialize(metadata).IsSuccess());
    assert(reloaded.GetCount(metadata, SourceFlag::Database) == 5);
    count = 0;
    std::array<StoreData, 5> saved{};
    assert(reloaded.Get(metadata, std::span{saved}, count, SourceFlag::Database).IsSuccess());
    assert(saved[0].GetCreateId() == stores[1].GetCreateId());
    assert(reloaded.Format(metadata).IsSuccess());
    assert(reloaded.GetCount(metadata, SourceFlag::Database) == 0);
    // Corruption must not be concealed by automatic seeding.
    const auto db_path = Common::FS::test_root / "system/save/8000000000000030/MiiDatabase.dat";
    { std::FILE* file = std::fopen(db_path.string().c_str(), "wb");
      assert(file); std::fputc(0, file); std::fclose(file); }
    MiiManager corrupt;
    assert(corrupt.Initialize(metadata).IsError());
}
"""


class FirmwareFreeTests(unittest.TestCase):
    def test_generated_resources_are_reproducible(self):
        self.assertEqual((DATA / "mii_resources.h").read_text(), resources.generate_header())

    def test_all_resource_indices_and_payloads(self):
        for blob, magic, counts, size in (
            (resources.make_shapes(), b"NFSR", resources.SHAPE_COUNTS, 88),
            (resources.make_textures(), b"NFTR", resources.TEXTURE_COUNTS, 28),
        ):
            self.assertEqual(blob[:4], magic)
            version, file_size = struct.unpack_from("<II", blob, 4)
            self.assertEqual(version, 1)
            self.assertEqual(file_size, len(blob))
            start = 12 + len(counts) * 8
            for index in range(sum(counts)):
                entry = start + index * size
                offset, raw_size, packed_size, level, memory, pad = struct.unpack_from(
                    "<IIIBBH", blob, entry
                )
                self.assertEqual(offset % 16, 0)
                self.assertGreaterEqual(offset, start + sum(counts) * size)
                self.assertLessEqual(offset + packed_size, len(blob))
                self.assertEqual((level, memory, pad), (6, 8, 0))
                raw = zlib.decompress(blob[offset : offset + packed_size])
                self.assertEqual(len(raw), raw_size)
                if magic == b"NFSR":
                    fields = struct.unpack_from("<12I6f", blob, entry + 16)
                    offsets, sizes = fields[:5], fields[5:10]
                    for off, length in zip(offsets, sizes):
                        self.assertLessEqual(off + length, len(raw))
                    vertices = sizes[0] // 8
                    idx_offset, idx_size = fields[10:12]
                    self.assertLessEqual(idx_offset + idx_size, len(raw))
                    for vertex in struct.iter_unpack(
                        "<4e", raw[offsets[0] : offsets[0] + sizes[0]]
                    ):
                        self.assertTrue(all(math.isfinite(v) for v in vertex))
                    indices = struct.unpack_from(f"<{idx_size // 2}H", raw, idx_offset)
                    self.assertTrue(all(i < vertices for i in indices))
                    self.assertEqual(len(indices) % 3, 0)
                else:
                    alignment, width, height, fmt, mip, tile, pad = struct.unpack_from(
                        "<IHH4B", blob, entry + 16
                    )
                    self.assertEqual((alignment, fmt, mip, tile, pad), (16, 2, 1, 1, 0))
                    self.assertEqual(raw_size, width * height * 4)

    def test_eden_romfs_readers_and_mii_service(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "A C++20 compiler is required")
        with tempfile.TemporaryDirectory(prefix="nxbox-t20-", dir="/tmp") as directory:
            temp = Path(directory)
            for name, content in STUBS.items():
                path = temp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            driver = temp / "driver.cpp"
            driver.write_text(SUPPORT + DRIVER)
            sources = [
                "common/random.cpp",
                "common/fs/file.cpp",
                "common/fs/fs.cpp",
                "core/file_sys/romfs.cpp",
                "core/file_sys/fsmitm_romfsbuild.cpp",
            ]
            sources += [
                f"core/file_sys/vfs/{name}.cpp"
                for name in ("vfs", "vfs_vector", "vfs_offset", "vfs_concat", "vfs_cached")
            ]
            sources += [
                f"core/file_sys/system_archive/{name}.cpp"
                for name in ("system_archive", "mii_model", "ng_word", "system_version")
            ]
            sources += [
                f"core/hle/service/mii/{name}.cpp"
                for name in ("mii_manager", "mii_database_manager", "mii_database")
            ]
            sources += [
                f"core/hle/service/mii/types/{name}.cpp"
                for name in ("store_data", "core_data", "char_info", "raw_data", "ver3_store_data")
            ]
            binary = temp / "test"
            result = subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-O1",
                    "-ffunction-sections",
                    "-fdata-sections",
                    "-I",
                    str(temp),
                    "-I",
                    str(ROOT / "src"),
                    str(driver),
                    *[str(ROOT / "src" / source) for source in sources],
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            data = temp / "nand"
            data.mkdir()
            subprocess.run([str(binary), str(data)], check=True, timeout=15)
            for name in ("ShapeHigh.dat", "ShapeMid.dat"):
                self.assertEqual((data / name).read_bytes(), resources.make_shapes())
            for name in (
                "NXTextureLowLinear.dat",
                "NXTextureLowSRGB.dat",
                "NXTextureMidLinear.dat",
                "NXTextureMidSRGB.dat",
            ):
                self.assertEqual((data / name).read_bytes(), resources.make_textures())

    def test_inventory_covers_registered_archives(self):
        source = (ROOT / "src/core/file_sys/system_archive/system_archive.cpp").read_text()
        documented = (ROOT / "docs/firmware-free.md").read_text()
        for title_id in re.findall(r"case (0x01000000000008[0-9A-F]{2}):", source):
            self.assertIn(title_id, documented)


if __name__ == "__main__":
    unittest.main()
