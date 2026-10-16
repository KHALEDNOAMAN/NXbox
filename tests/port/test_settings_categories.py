# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the category map, disabled renderer and real settings persistence."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <fstream>
#include <iterator>
#include "eden_uwp/ui/settings_model.h"
#include "eden_uwp/renderer_settings.h"
using namespace EdenXbox;
using namespace EdenXbox::Ui;
int main(int argc, char** argv) {
    assert(argc == 2);
    std::array<int, static_cast<std::size_t>(Setting::Count)> visits{};
    for (const auto& category : kSettingsCategories) {
        assert(category.count > 0 && category.count <= static_cast<int>(category.rows.size()));
        for (int row = 0; row < category.count; ++row)
            ++visits[static_cast<std::size_t>(category.rows[row])];
    }
    for (const auto count : visits)
        assert(count == 1);
    assert(kSettingsCategories[0].rows[0] == Setting::Renderer);
    assert(kSettingsCategories[3].rows[3] == Setting::SaveSync);
    assert(kSettingsCategories[5].rows[1] == Setting::Credits);
    assert(RendererChoiceEnabled(0) && RendererChoiceEnabled(1));
    assert(!RendererChoiceEnabled(2) && !RendererChoiceEnabled(-1));
    const std::filesystem::path local(argv[1]);
    const auto file = local / "eden_settings.txt";
    assert(RendererSettings::Read(local) == RendererSettings::kDirectX12);
    { std::ofstream out(file); out << "# keep\nresolution_setup=6\nbackend=4\nrenderer_backend=5\nbackend=0\nother=a=b\n"; }
    assert(RendererSettings::Read(local) == RendererSettings::kOpenGL);
    for (const int backend : {RendererSettings::kDirectX12, RendererSettings::kOpenGL,
                              RendererSettings::kDirectX12}) {
        assert(RendererSettings::Save(local, backend));
        assert(RendererSettings::Read(local) == backend);
        std::ifstream in(file);
        const std::string content{std::istreambuf_iterator<char>(in), {}};
        assert(content == "# keep\nresolution_setup=6\nother=a=b\nrenderer_backend=" +
                          std::to_string(backend) + "\n");
        std::istringstream resolution(content);
        assert(TitleSettings::ReadResolution(resolution) == 6);
    }
    assert(!RendererSettings::Save(local, 1)); // Vulkan cannot be persisted.
    assert(RendererSettings::Read(local) == RendererSettings::kDirectX12);
    const auto blocked = local / "blocked";
    { std::ofstream out(blocked); out << "file"; }
    assert(!RendererSettings::Save(blocked, RendererSettings::kOpenGL));
}
"""


class SettingsCategoriesTests(unittest.TestCase):
    def test_categories_renderer_and_persistence(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-settings-") as directory:
            source = Path(directory) / "driver.cpp"
            binary = Path(directory) / "driver"
            source.write_text(DRIVER)
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
            subprocess.run([str(binary), directory], check=True, timeout=10)

    def test_backend_values_match_eden(self):
        enums = (ROOT / "src/common/settings_enums.h").read_text()
        values = re.search(r"ENUM\(RendererBackend,\s*([^;]+)\);", enums).group(1).split(",")
        names = [value.strip() for value in values]
        self.assertEqual(names.index("OpenGL_GLSL"), 0)
        self.assertEqual(names.index("D3D12"), 5)

    def test_disabled_entry_and_pre_window_reload_are_wired(self):
        ui = (ROOT / "src/eden_uwp/ui/library_screen.cpp").read_text()
        action = ui.split("void ActivateSetting", 1)[1].split("void Toast", 1)[0]
        self.assertLess(
            action.index("!RendererChoiceEnabled(renderer_choice_)"),
            action.index("RendererSettings::Save"),
        )
        self.assertIn("Text::RendererVulkanHint", ui)
        self.assertIn("!RendererChoiceEnabled(i)", ui)
        session = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        creation = session.index("graphics = std::make_shared<MesaWindow>")
        reload = session.rindex("ApplyEdenSettingsFile();", 0, creation)
        self.assertLess(creation - reload, 250)
