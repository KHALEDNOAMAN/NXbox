// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

#include "core/hle/service/am/am_types.h"

namespace Service::AM {

// Firmware-first policy for local applets. Account authentication and the shop are excluded,
// and so is Controller: the HLE applet accepts the connected Xbox pad at once, while the
// firmware one waits on a Joy-Con UI that the pad cannot drive.
constexpr bool PreferLocalFirmwareApplet(AppletId id) {
    switch (id) {
    case AppletId::Auth: // Parental controls, not Nintendo account authentication.
    case AppletId::Cabinet:
    case AppletId::Error:
    case AppletId::NetConnect:
    case AppletId::ProfileSelect:
    case AppletId::SoftwareKeyboard:
    case AppletId::MiiEdit:
    case AppletId::Web:
    case AppletId::PhotoViewer:
    case AppletId::OfflineWeb:
    case AppletId::WebAuth: // Captive portal, not console account authentication.
        return true;
    default:
        return false;
    }
}

// NXBOX_SWKBD=hle (LocalState\nxbox_env.txt) keeps the keyboard applet inside the emulator. The
// frontend answers a name prompt at once, so a scripted run does not have to drive the firmware
// keyboard with the pad. Unset or any other value leaves the firmware-first policy untouched.
constexpr bool ForceHleApplet(AppletId id, std::string_view swkbd_override) {
    return id == AppletId::SoftwareKeyboard && swkbd_override == "hle";
}

// Keep creation and fallback ordering identical for both library-applet IPC variants.
template <typename GuestFactory, typename FrontendFactory>
auto CreateAppletWithFallback(bool prefer_guest, GuestFactory create_guest,
                              FrontendFactory create_frontend) {
    auto applet = prefer_guest ? create_guest() : decltype(create_guest()){};
    if (!applet) {
        return create_frontend();
    }
    return applet;
}

constexpr bool ShouldRecoverGuestApplet(bool frontend, bool completed, bool returned,
                                        bool output_sent, bool exit_requested, bool has_input) {
    return !frontend && !completed && !returned && !output_sent && !exit_requested && has_input;
}

} // namespace Service::AM
