# SPDX-License-Identifier: GPL-3.0-or-later
"""Guest face-button layouts, persisted choices, onboarding and font variants."""

import re
import subprocess
from pathlib import Path

from test_xbox_button_icons import DATA, GENERATOR, load, raw_glyph

ROOT = Path(__file__).resolve().parents[2]


def test_layout_default_round_trip_and_mapping(tmp_path):
    source = tmp_path / "driver.cpp"
    binary = tmp_path / "driver"
    source.write_text(r"""
#include <cassert>
#include <fstream>
#include <iterator>
#include "eden_uwp/renderer_settings.h"
using namespace EdenXbox;
using namespace EdenXbox::RendererSettings;
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path local(argv[1]);
    assert(ReadButtonLayout(local) == ButtonLayout::Name);
    assert(!HasButtonLayout(local));
    const auto file = local / "eden_settings.txt";
    { std::ofstream out(file); out << "# keep\nshader_preload=0\nxbox_button_icons=0\n"
        "renderer_backend=5\nother=a=b\n button_layout = name\r\nbutton_layout=position\n"; }
    assert(HasButtonLayout(local));
    assert(ReadButtonLayout(local) == ButtonLayout::Position);
    for (auto layout : {ButtonLayout::Name, ButtonLayout::Position, ButtonLayout::Name}) {
        assert(SaveButtonLayout(local, layout));
        assert(HasButtonLayout(local));
        assert(ReadButtonLayout(local) == layout);
        assert(!ReadShaderPreload(local));
        assert(!ReadXboxButtonIcons(local));
        assert(Read(local) == kDirectX12);
        std::ifstream in(file);
        const std::string text{std::istreambuf_iterator<char>(in), {}};
        assert(text.find("# keep\n") != std::string::npos);
        assert(text.find("other=a=b\n") != std::string::npos);
        auto key = text.find("button_layout=");
        assert(key != std::string::npos);
        assert(text.find("button_layout=", key + 1) == std::string::npos);
        assert(SaveShaderPreload(local, true));
        assert(SaveXboxButtonIcons(local, true));
        assert(Save(local, kOpenGL));
        assert(ReadButtonLayout(local) == layout);
        assert(SaveShaderPreload(local, false));
        assert(SaveXboxButtonIcons(local, false));
        assert(Save(local, kDirectX12));
    }
    { std::ofstream out(file); out << "button_layout=unknown\n"; }
    assert(ReadButtonLayout(local) == ButtonLayout::Name);
    assert(!SaveButtonLayout(file, ButtonLayout::Position));
    assert(!SaveButtonLayout(local / "missing", ButtonLayout::Name));
    constexpr int name[] = {0, 1, 2, 3};
    constexpr int position[] = {1, 0, 2, 3};
    assert(FaceButtonIndex(-1, ButtonLayout::Position) == -1);
    for (int i = 0; i < 20; ++i) {
        assert(FaceButtonIndex(i, ButtonLayout::Name) == (i < 4 ? name[i] : i));
        assert(FaceButtonIndex(i, ButtonLayout::Position) == (i < 4 ? position[i] : i));
        assert(FaceButtonIndex(FaceButtonIndex(i, ButtonLayout::Position), ButtonLayout::Position) == i);
    }
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


def test_physical_and_scripted_input_use_separate_conventions():
    pad = (ROOT / "src/eden_uwp/gamepad.h").read_text()
    assert re.findall(r"GamepadButtons::([ABXY]),", pad)[:4] == ["A", "B", "X", "Y"]
    assert "FaceButtonIndex(i, button_layout)" in pad
    assert "reading.Buttons & mappings[physical]" in pad
    assert "physical_keys & (1u << physical)" in pad
    assert "scripted_keys & (1u << i)" in pad
    ui = (ROOT / "src/eden_uwp/ui/input.cpp").read_text()
    for letter in "ABXY":
        assert f"merge(Button::{letter}, pressed(GamepadButtons::{letter}))" in ui
    assert "FaceButtonIndex" not in ui
    session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
    assert (
        session.index("ReadButtonLayout")
        < session.index("gamepad->SetButtonLayout")
        < session.index("system.Initialize()")
    )
    assert "Settings::values.button_layout.SetValue" in session


def test_setup_migration_cards_and_settings_strings():
    setup = (ROOT / "src/eden_uwp/ui/savesync_ui.cpp").read_text()
    flow = setup.split("bool RunFirstRunSetup", 1)[1]
    assert "if (IsSetupDone(local_state))" in flow
    assert flow.index("RunButtonLayoutSetup") < flow.index("bool wants_sync")
    assert flow.rindex("RunButtonLayoutSetup") < flow.index('local_state / "setup_done.txt"')
    cards = setup.split("bool RunButtonLayoutSetup", 1)[1].split("bool RunFirstRunSetup", 1)[0]
    for action in ("Left", "Right", "A"):
        assert f"input.Pressed(Button::{action})" in cards
    assert "SaveButtonLayout" in cards and "save_failed = true" in cards
    assert "DrawXboxButtonDiamond" in cards and "DrawRing" in cards
    assert "Text::ButtonLayoutNameActions" in cards
    assert "Text::ButtonLayoutPositionActions" in cards
    widgets = (ROOT / "src/eden_uwp/ui/widgets.cpp").read_text()
    diamond = widgets.split("void DrawXboxButtonDiamond", 1)[1].split("float PillWidth", 1)[0]
    assert 'letters[] = {L"Y", L"X", L"B", L"A"}' in diamond
    assert re.findall(r"Theme::kPhysicalButton([ABXY])", diamond) == ["Y", "X", "B", "A"]
    for point in (
        "Point2F(center.x, center.y - spacing)",
        "Point2F(center.x - spacing, center.y)",
        "Point2F(center.x + spacing, center.y)",
        "Point2F(center.x, center.y + spacing)",
    ):
        assert point in diamond
    assert "layout == ButtonLayout::Position ? 2 : 3" in diamond
    assert "spot == confirm" in diamond and "renderer.StrokeRounded" in diamond
    assert "renderer.FillDisc" in diamond
    library = (ROOT / "src/eden_uwp/ui/library_screen.cpp").read_text()
    assert "!IsSetupDone(local_state) || !RendererSettings::HasButtonLayout(local_state)" in library
    assert "Setting::ButtonLayout" in library
    assert "DrawXboxButtonDiamond(renderer_" in library
    assert "SaveButtonLayout(local_state_, next)" in library
    model = (ROOT / "src/eden_uwp/ui/settings_model.h").read_text()
    assert "Setting::XboxButtonIcons, Setting::ButtonLayout" in model
    for lang in ("en", "pt_br"):
        strings = (ROOT / f"src/eden_uwp/ui/lang/{lang}.inc").read_text()
        for key in (
            "Title",
            "Name",
            "Position",
            "NameActions",
            "PositionActions",
            "NameCaption",
            "PositionCaption",
            "Hint",
            "SaveFailed",
            "NameShort",
            "PositionShort",
        ):
            assert f"Text::ButtonLayout{key}," in strings


def test_position_font_swaps_only_ab_glyphs_and_is_deterministic():
    name = load("font_nintendo_extended_xbox")
    position = load("font_nintendo_extended_xbox_position")
    assert GENERATOR.build_font("position") == GENERATOR.read_array(
        DATA / "font_nintendo_extended_xbox_position.cpp"
    )
    assert name.getBestCmap() == position.getBestCmap()
    swaps = {base + i: base + (i ^ 1) for base in (0xE0A0, 0xE0E0) for i in range(2)}
    by_name = {name.getBestCmap()[cp]: name.getBestCmap()[other] for cp, other in swaps.items()}
    for glyph in name.getGlyphOrder():
        assert raw_glyph(position, glyph) == raw_glyph(name, by_name.get(glyph, glyph)), glyph
    for table in name.keys():
        if table not in ("GlyphOrder", "glyf", "loca", "head"):
            assert name.getTableData(table) == position.getTableData(table), table
    for field in ("created", "modified", "xMin", "yMin", "xMax", "yMax", "unitsPerEm"):
        assert getattr(name["head"], field) == getattr(position["head"], field)
    service = (ROOT / "src/core/hle/service/ns/platform_service_manager.cpp").read_text()
    assert 'Settings::values.button_layout.GetValue() == "position"' in service
    assert "SynthesizeXboxPositionFontArchive()" in service
    shared = (ROOT / "src/core/file_sys/system_archive/shared_font.cpp").read_text()
    assert shared.count("PackBFTTF(SharedFontData::FONT_NINTENDO_EXTENDED_XBOX_POSITION,") == 2
    cmake = (ROOT / "src/core/CMakeLists.txt").read_text()
    for extension in ("cpp", "h"):
        assert f"font_nintendo_extended_xbox_position.{extension}" in cmake


def test_confirm_button_translations():
    expected = {
        "en": {
            "Title": "Confirm button",
            "Name": "Confirm with A",
            "Position": "Confirm with B",
            "NameCaption": "Xbox standard.",
            "PositionCaption": "Like on a Switch: the right button confirms.",
            "NameActions": "A = Confirm · B = Back",
            "PositionActions": "B = Confirm · A = Back",
        },
        "pt_br": {
            "Title": "Botão de confirmar",
            "Name": "Confirmar com A",
            "Position": "Confirmar com B",
            "NameCaption": "Padrão do Xbox.",
            "PositionCaption": "Como no Switch: o botão da direita confirma.",
            "NameActions": "A = Confirmar · B = Voltar",
            "PositionActions": "B = Confirmar · A = Voltar",
        },
    }
    for lang, translations in expected.items():
        strings = (ROOT / f"src/eden_uwp/ui/lang/{lang}.inc").read_text()
        for key, value in (translations | {"NameShort": "A", "PositionShort": "B"}).items():
            assert f'Text::ButtonLayout{key}, L"{value}"' in strings
