# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the firmware selection/recovery policy and HOME input transitions."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r"""
#include <cassert>
#include <memory>
#include "core/hle/service/am/applet_policy.h"
#include "eden_uwp/applet_boot.h"
int main() {
    using namespace Service::AM;
    int preferred = 0;
    for (unsigned id = 0; id < 256; ++id) {
        const bool expected = id == 0x0a || id == 0x0b ||
            id == 0x0e || id == 0x0f || id == 0x10 || id == 0x11 ||
            id == 0x12 || id == 0x13 || id == 0x15 || id == 0x17 || id == 0x19;
        assert(PreferLocalFirmwareApplet(static_cast<AppletId>(id)) == expected);
        preferred += expected;
    }
    assert(preferred == 11);
    // NXBOX_SWKBD=hle moves only the keyboard applet to the frontend; other values change nothing.
    for (unsigned id = 0; id < 256; ++id) {
        const auto applet = static_cast<AppletId>(id);
        assert(ForceHleApplet(applet, "hle") == (applet == AppletId::SoftwareKeyboard));
        assert(!ForceHleApplet(applet, ""));
        assert(!ForceHleApplet(applet, "lle"));
        assert(!ForceHleApplet(applet, "HLE"));
    }
    // Missing firmware and loader failure have the same null guest result.
    for (int prefer = 0; prefer < 2; ++prefer) {
        for (int guest_ok = 0; guest_ok < 2; ++guest_ok) {
            for (int frontend_ok = 0; frontend_ok < 2; ++frontend_ok) {
                int guest_calls = 0, frontend_calls = 0;
                auto result = CreateAppletWithFallback(prefer,
                    [&] { ++guest_calls; return guest_ok ? std::make_shared<int>(1) : nullptr; },
                    [&] { ++frontend_calls; return frontend_ok ? std::make_shared<int>(2) : nullptr; });
                assert(guest_calls == prefer);
                assert(frontend_calls == !(prefer && guest_ok));
                const int expected = prefer && guest_ok ? 1 : frontend_ok ? 2 : 0;
                assert((result ? *result : 0) == expected);
            }
        }
    }
    // Every combination: normal returns, output, cancellation and HLE must never replay.
    for (unsigned flags = 0; flags < 64; ++flags) {
        assert(ShouldRecoverGuestApplet(flags & 1, flags & 2, flags & 4,
            flags & 8, flags & 16, flags & 32) == (flags == 32));
    }
    EdenXbox::HomeButtonEdge home;
    assert(!home.Update(false, false));
    assert(!home.Update(true, false));
    assert(home.Update(true, true));
    assert(!home.Update(true, true));
    assert(!home.Update(false, true));
    assert(home.Update(true, true));
    home.Reset();
    assert(!home.Update(true, true));
    assert(!home.Update(false, false));
    assert(home.Update(true, true));
    assert(EdenXbox::SwitchHomeBootTarget != "");
}
"""


class AppletPolicyTests(unittest.TestCase):
    def test_selection_and_fallback_coverage(self):
        with tempfile.TemporaryDirectory(prefix="nxbox-t17-", dir="/tmp") as directory:
            driver = Path(directory) / "driver.cpp"
            driver.write_text(DRIVER)
            binary = Path(directory) / "driver"
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(driver),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_standalone_mii_edit_boot(self):
        target = (ROOT / "src/eden_uwp/applet_boot.h").read_text()
        self.assertIn('SwitchMiiEditBootTarget = "nxbox-system-mii-edit"', target)
        source = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        self.assertIn("protocol_path == SwitchMiiEditBootTarget", source)
        self.assertIn("Service::AM::AppletProgramId::MiiEdit", source)
        self.assertIn("Service::AM::AppletId::MiiEdit", source)
        self.assertIn("firmware->GetEntryRaw(static_cast<u64>(firmware_program_id)", source)
        self.assertIn(".applet_type = Service::AM::AppletType::LibraryApplet", source)
        self.assertIn(".applet_id = firmware_applet_id", source)
        self.assertLess(source.index("SetCurrentAppletId("), source.index("system.Load("))
        selection = source.split("else if (chosen == SwitchMiiEditBootTarget)", 1)[1]
        self.assertIn("protocol_path = chosen;", selection.split("} else", 1)[0])
        firmware = (ROOT / "src/eden_uwp/firmware.cpp").read_text()
        self.assertIn("next.mii_edit_available =", firmware)
        self.assertIn("Service::AM::AppletProgramId::MiiEdit", firmware)
        moru = (ROOT / "src/eden_uwp/ui/moru_screen.cpp").read_text()
        self.assertIn("!selected && GetFirmwareStatus().mii_edit_available", moru)
        self.assertIn("*launch_target_ = std::string(SwitchMiiEditBootTarget)", moru)
        for lang in ("en", "pt_br"):
            table = (ROOT / f"src/eden_uwp/ui/lang/{lang}.inc").read_text()
            self.assertIn("Text::MoruSystemCreator,", table)
            self.assertIn("Text::MoruSystemCreatorHint,", table)

    def test_guest_creation_falls_back_in_both_ipc_paths(self):
        source = (ROOT / "src/core/hle/service/am/service/library_applet_creator.cpp").read_text()
        for method in ("CreateLibraryApplet", "CreateLibraryAppletEx"):
            body = source.split(f"Result ILibraryAppletCreator::{method}(")[1].split("\n}", 1)[0]
            self.assertIn("CreateAppletWithFallback(", body)
            self.assertLess(body.index("CreateGuestApplet("), body.index("CreateFrontendApplet("))
            self.assertIn("CreateFrontendApplet(", body)
        self.assertIn("return PreferLocalFirmwareApplet(applet_id);", source)
        uwp = source.split("#ifdef NXBOX_UWP", 1)[1].split("#else", 1)[0]
        self.assertIn('std::getenv("NXBOX_SWKBD")', uwp)
        self.assertLess(uwp.index("ForceHleApplet("), uwp.index("return PreferLocalFirmwareApplet("))
        self.assertNotIn("UWP has no firmware-applet UI", source)

    def test_recovery_keeps_broker_and_waits_for_hle_completion(self):
        source = (ROOT / "src/core/hle/service/am/window_system.cpp").read_text()
        self.assertIn("ShouldRecoverGuestApplet(", source)
        self.assertIn("input.Push(m_system.Kernel(), storage);", source)
        self.assertIn("applet->window_visible = false;", source)
        self.assertIn("applet->frontend && !applet->is_completed", source)
        self.assertIn("!m_system.IsShuttingDown()", source)
        self.assertIn("applet->display_layer_manager.SetWindowVisibility(false);", source)
        caller = (ROOT / "src/core/hle/service/am/service/library_applet_accessor.cpp").read_text()
        self.assertIn("fallback_input.push_back(", caller)
        self.assertIn("m_applet->guest_returned = true;", caller)
        guest = (
            ROOT / "src/core/hle/service/am/service/library_applet_self_accessor.cpp"
        ).read_text()
        self.assertEqual(guest.count("m_applet->guest_output_sent = true;"), 2)
        self.assertIn("m_applet->guest_returned = true;", guest)

    def test_net_connect_hle_completes_with_failure(self):
        source = (ROOT / "src/core/hle/service/am/frontend/applet_net_connect.cpp").read_text()
        execute = source.split("void NetConnect::Execute()", 1)[1].split(
            "Result NetConnect::RequestExit", 1
        )[0]
        self.assertIn("AM::ResultLibraryAppletTerminated", execute)
        self.assertIn("complete = true;", execute)
        self.assertIn("Exit();", execute)

    def test_home_boot_does_not_remember_game_or_sync_save(self):
        source = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        selection = source.split("if (chosen == SwitchHomeBootTarget)", 1)[1].split(
            "std::atomic<bool> done", 1
        )[0]
        self.assertIn("protocol_path = chosen;", selection)
        self.assertIn("} else if (!chosen.empty())", selection)
        self.assertIn("GetSystemNANDContents()", source)
        self.assertIn("Service::AM::AppletProgramId::QLaunch", source)
        self.assertIn("parameters.applet_type = Service::AM::AppletType::SystemApplet;", source)
        self.assertIn("gamepad->ResetHomeButton();", source)
        self.assertIn("system.GetAppletManager().HomeButtonPressed();", source)
        for lang in ("en", "pt_br"):
            table = (ROOT / f"src/eden_uwp/ui/lang/{lang}.inc").read_text()
            self.assertIn("Text::SystemMenuBoot", table)


if __name__ == "__main__":
    unittest.main()
