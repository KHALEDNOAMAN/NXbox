# SPDX-License-Identifier: GPL-3.0-or-later
"""Check font isolation, deterministic generation, settings persistence and wiring.

Requires fontTools (uv run --with fonttools --with pytest python -m pytest ...).
"""

import importlib.util
import io
import re
import subprocess
from pathlib import Path

from fontTools.pens.recordingPen import RecordingPen
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "make_xbox_ext_font", ROOT / "tools/nxbox/make_xbox_ext_font.py"
)
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)
DATA = GENERATOR.DATA


def load(name):
    return TTFont(io.BytesIO(GENERATOR.read_array(DATA / f"{name}.cpp")))


def raw_glyph(font, name):
    index = font.getGlyphID(name)
    offsets = font["loca"].locations
    # Read the serialized bytes, never a fontTools recompilation of the table.
    return font.reader["glyf"][offsets[index] : offsets[index + 1]]


def test_generated_font_is_deterministic_and_preserves_other_glyphs():
    original = load("font_nintendo_extended")
    generated = load("font_nintendo_extended_xbox")
    assert GENERATOR.build_font() == GENERATOR.read_array(DATA / "font_nintendo_extended_xbox.cpp")
    assert (
        generated.getBestCmap().keys() == original.getBestCmap().keys() | GENERATOR.ADDITIONS.keys()
    )
    assert set(GENERATOR.REPLACEMENTS) <= original.getBestCmap().keys()
    assert set(GENERATOR.ADDITIONS).isdisjoint(original.getBestCmap())
    replaced_names = {original.getBestCmap()[cp] for cp in GENERATOR.REPLACEMENTS}
    for name in original.getGlyphOrder():
        assert original["hmtx"][name] == generated["hmtx"][name]
        if name in replaced_names:
            assert raw_glyph(original, name) != raw_glyph(generated, name)
            old_pen, new_pen = RecordingPen(), RecordingPen()
            original.getGlyphSet()[name].draw(old_pen)
            generated.getGlyphSet()[name].draw(new_pen)
            assert old_pen.value != new_pen.value
            old, new = original["glyf"][name], generated["glyf"][name]
            assert new.yMin >= old.yMin and new.yMax <= old.yMax
            assert new.xMin >= old.xMin and new.xMax <= old.xMax
        else:
            assert raw_glyph(original, name) == raw_glyph(generated, name), name
    for cp in GENERATOR.ADDITIONS:
        name = generated.getBestCmap()[cp]
        assert generated["glyf"][name].numberOfContours > 0
        assert generated["hmtx"][name] == original["hmtx"][original.getBestCmap()[0xE0A0]]
    # All existing global vertical metrics remain unchanged, including OS/2 metrics.
    assert original.getTableData("hhea") == generated.getTableData("hhea")
    assert generated["OS/2"].usLastCharIndex == max(generated.getBestCmap())
    for field in ("sTypoAscender", "sTypoDescender", "sTypoLineGap", "usWinAscent", "usWinDescent"):
        assert getattr(original["OS/2"], field) == getattr(generated["OS/2"], field)
    assert original["head"].unitsPerEm == generated["head"].unitsPerEm
    for field in ("yMin", "yMax", "created", "modified"):
        assert getattr(original["head"], field) == getattr(generated["head"], field)
    assert len(GENERATOR.read_array(DATA / "font_nintendo_extended_xbox.cpp")) % 4 == 0


def test_setting_round_trip_and_preservation(tmp_path):
    source = tmp_path / "driver.cpp"
    binary = tmp_path / "driver"
    source.write_text(r"""
#include <cassert>
#include <fstream>
#include <iterator>
#include "eden_uwp/renderer_settings.h"
using namespace EdenXbox::RendererSettings;
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path local(argv[1]);
    assert(ReadXboxButtonIcons(local));
    const auto file = local / "eden_settings.txt";
    {
        std::ofstream out(file);
        out << "# keep\nshader_preload=0\nrenderer_backend=5\nother=a=b\n"
               " xbox_button_icons = 1\r\nxbox_button_icons=0\n";
    }
    assert(!ReadXboxButtonIcons(local));
    for (const bool enabled : {true, false, true}) {
        assert(SaveXboxButtonIcons(local, enabled));
        assert(ReadXboxButtonIcons(local) == enabled);
        assert(!ReadShaderPreload(local));
        assert(Read(local) == kDirectX12);
        std::ifstream in(file);
        const std::string content{std::istreambuf_iterator<char>(in), {}};
        assert(content.find("# keep\n") != std::string::npos);
        assert(content.find("other=a=b\n") != std::string::npos);
        const auto key = content.find("xbox_button_icons=");
        assert(key != std::string::npos);
        assert(content.find("xbox_button_icons=", key + 1) == std::string::npos);
        assert(SaveShaderPreload(local, true));
        assert(Save(local, kOpenGL));
        assert(ReadXboxButtonIcons(local) == enabled);
        assert(SaveShaderPreload(local, false));
        assert(Save(local, kDirectX12));
    }
    assert(!SaveXboxButtonIcons(file, false));
    assert(!SaveXboxButtonIcons(local / "missing", false));
}
""")
    subprocess.run(
        [
            "clang++",
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "src"),
            str(source),
            "-o",
            str(binary),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run([str(binary), str(tmp_path)], check=True, timeout=10)


def test_xbox_archive_overrides_both_slots_before_nand():
    service = (ROOT / "src/core/hle/service/ns/platform_service_manager.cpp").read_text()
    loop = service.split("for (auto& font : SHARED_FONTS)", 1)[1]
    assert "Settings::values.xbox_button_icons.GetValue()" in loop
    assert "static_cast<u64>(font.first) == 0x0100000000000810" in loop
    assert re.search(
        r"if \(use_xbox\)\s*\{\s*romfs = FileSys::SystemArchive::SynthesizeXboxFontArchive\(\)",
        loop,
    )
    assert "(force_substitute || use_xbox)" in loop
    assert loop.index("SynthesizeXboxFontArchive") < loop.index("nand->GetEntry")
    assert "NXBOX_SUBSTITUTE_FONTS" in service
    archive = (ROOT / "src/core/file_sys/system_archive/system_archive.cpp").read_text()
    assert "return CreateRomFS(FontNintendoExtensionXbox());" in archive
    shared = (ROOT / "src/core/file_sys/system_archive/shared_font.cpp").read_text()
    xbox = shared.split("VirtualDir FontNintendoExtensionXbox()", 1)[1].split(
        "VirtualDir FontStandard()", 1
    )[0]
    for name in ("nintendo_ext_003.bfttf", "nintendo_ext2_003.bfttf"):
        assert f'PackBFTTF(SharedFontData::FONT_NINTENDO_EXTENDED_XBOX, "{name}")' in xbox
    settings = (ROOT / "src/common/settings.h").read_text()
    assert 'xbox_button_icons{linkage, true, "xbox_button_icons"' in settings
    session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
    assert re.search(
        r"Settings::values.xbox_button_icons.SetValue\(\s*RendererSettings::ReadXboxButtonIcons\(",
        session[: session.index("system.Initialize()")],
    )
