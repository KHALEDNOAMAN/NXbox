// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace D3D12::ShaderProbe {
struct Config {
    std::filesystem::path directory;
    int timeout_s{45};

    bool Enabled() const {
        return !directory.empty();
    }

    static Config Parse(const char* directory, const char* timeout) {
        Config config;
        if (directory && *directory) {
            const std::string_view bytes{directory};
            config.directory = std::u8string{bytes.begin(), bytes.end()};
        }
        if (timeout && *timeout) {
            const std::string_view text{timeout};
            long long seconds{};
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), seconds);
            if (error == std::errc{} && end == text.data() + text.size())
                config.timeout_s = static_cast<int>(std::clamp(seconds, 1LL, 600LL));
        }
        return config;
    }
};

inline std::string Target(std::string_view source) {
    auto line = source.substr(0, source.find('\n'));
    constexpr std::string_view prefix = "// target:";
    if (!line.starts_with(prefix))
        return "cs_6_0";
    line.remove_prefix(prefix.size());
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string_view::npos)
        return "cs_6_0";
    line.remove_prefix(first);
    line = line.substr(0, line.find_last_not_of(" \t\r") + 1);
    return std::string{line};
}

inline std::string ErrorSnippet(std::string_view error) {
    std::string snippet;
    for (auto character : error.substr(0, 300)) {
        if (character == '\n' || character == '\r' || character == '\0')
            character = ' ';
        if (character == '"' || character == '\\')
            snippet += '\\';
        snippet += character;
    }
    return snippet;
}

inline std::vector<std::filesystem::path> Files(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".hlsl")
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

enum class Result { Ok, Timeout, Fail };
struct Counts {
    unsigned ok{}, timeout{}, fail{};
    void Record(Result result) {
        switch (result) {
        case Result::Ok:
            ++ok;
            break;
        case Result::Timeout:
            ++timeout;
            break;
        case Result::Fail:
            ++fail;
            break;
        }
    }
    bool Stopped() const {
        return timeout >= 4;
    }
};
} // namespace D3D12::ShaderProbe

#ifdef _WIN32
struct ID3D12Device;
namespace D3D12 {
// Detached workers own all their resources. Setting the flag never waits for DXC or the driver.
std::shared_ptr<std::atomic<bool>> StartShaderProbe(ID3D12Device* device, const char* directory,
                                                    const char* timeout) noexcept;
} // namespace D3D12
#endif
