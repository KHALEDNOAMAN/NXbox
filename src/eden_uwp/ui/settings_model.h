// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include "eden_uwp/ui/strings.h"

namespace EdenXbox::Ui {
enum class Setting {
    SaveSync,
    Sources,
    Usb,
    Credits,
    Nextendo,
    Multiplayer,
    InternetArt,
    SystemMenu,
    OfficialInterface,
    ProfilePicture,
    Accounts,
    Moru,
    BackToNXbox,
    Renderer,
    ShaderPreload,
    XboxButtonIcons,
    ButtonLayout,
    Resolution,
    Version,
    Count
};
struct SettingsCategory {
    Text title;
    std::array<Setting, 6> rows;
    int count;
};
inline constexpr std::array<SettingsCategory, 6> kSettingsCategories{{
    {Text::SettingsGraphics,
     {Setting::Renderer, Setting::ShaderPreload, Setting::XboxButtonIcons, Setting::ButtonLayout,
      Setting::Resolution, Setting::InternetArt},
     6},
    {Text::NavShop, {Setting::Sources, Setting::Usb}, 2},
    {Text::SettingsOnline, {Setting::Nextendo, Setting::Multiplayer}, 2},
    {Text::AccountSection,
     {Setting::ProfilePicture, Setting::Accounts, Setting::Moru, Setting::SaveSync},
     4},
    {Text::SettingsSystem,
     {Setting::SystemMenu, Setting::OfficialInterface, Setting::BackToNXbox},
     3},
    {Text::SettingsAbout, {Setting::Version, Setting::Credits}, 2},
}};
inline constexpr bool RendererChoiceEnabled(int choice) {
    return choice >= 0 && choice < 2;
}
} // namespace EdenXbox::Ui
