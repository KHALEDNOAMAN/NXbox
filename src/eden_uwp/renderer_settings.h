// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eden_uwp/title_settings.h"

namespace EdenXbox::RendererSettings {
// Values of Eden's RendererBackend: OpenGL_GLSL = 0, D3D12 = 5.
inline constexpr int kOpenGL = 0;
inline constexpr int kDirectX12 = 5;
inline int Read(const std::filesystem::path& local) {
    std::ifstream in(local / "eden_settings.txt");
    int backend = kDirectX12;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (entry && (entry->label == "renderer_backend" || entry->label == "backend")) {
            // Mesa also supports the legacy GLASM and SPIR-V settings.
            backend = entry->value == "0" || entry->value == "3" || entry->value == "4"
                          ? kOpenGL
                          : kDirectX12;
        }
    }
    return backend;
}
inline bool ReadShaderPreload(const std::filesystem::path& local) {
    std::ifstream in(local / "eden_settings.txt");
    bool enabled = true;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (entry && entry->label == "shader_preload")
            enabled = entry->value != "0";
    }
    return enabled;
}
inline bool SaveShaderPreload(const std::filesystem::path& local, bool enabled) {
    const auto file = local / "eden_settings.txt";
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error)
        return false;
    std::ifstream in(file);
    if (exists && !in)
        return false;
    std::string content;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (!entry || entry->label != "shader_preload")
            content += line + '\n';
    }
    if (in.bad())
        return false;
    in.close();
    std::ofstream out(file, std::ios::trunc);
    out << content << "shader_preload=" << (enabled ? 1 : 0) << '\n';
    out.close();
    return !out.fail();
}
inline bool ReadXboxButtonIcons(const std::filesystem::path& local) {
    std::ifstream in(local / "eden_settings.txt");
    bool enabled = true;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (entry && entry->label == "xbox_button_icons")
            enabled = entry->value != "0";
    }
    return enabled;
}
inline bool SaveXboxButtonIcons(const std::filesystem::path& local, bool enabled) {
    const auto file = local / "eden_settings.txt";
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error)
        return false;
    std::ifstream in(file);
    if (exists && !in)
        return false;
    std::string content;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (!entry || entry->label != "xbox_button_icons")
            content += line + '\n';
    }
    if (in.bad())
        return false;
    in.close();
    std::ofstream out(file, std::ios::trunc);
    out << content << "xbox_button_icons=" << (enabled ? 1 : 0) << '\n';
    out.close();
    return !out.fail();
}
inline bool Save(const std::filesystem::path& local, int backend) {
    if (backend != kDirectX12 && backend != kOpenGL)
        return false;
    const auto file = local / "eden_settings.txt";
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error)
        return false;
    std::ifstream in(file);
    if (exists && !in)
        return false;
    std::string content;
    for (std::string line; std::getline(in, line);) {
        const auto entry = TitleSettings::ParseLine(line);
        if (!entry || (entry->label != "renderer_backend" && entry->label != "backend"))
            content += line + '\n';
    }
    if (in.bad())
        return false;
    in.close();
    std::ofstream out(file, std::ios::trunc);
    out << content << "renderer_backend=" << backend << '\n';
    out.close();
    return !out.fail();
}
} // namespace EdenXbox::RendererSettings
