// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace D3D12 {
// Values match the Windows thread priority constants.
constexpr int ParsePsoWorkerPriority(std::string_view value) {
    return value == "lowest" ? -2 : value == "normal" ? 0 : -1;
}
inline int PsoWorkerPriority() {
    const char* value = std::getenv("NXBOX_D3D12_PSO_WORKER_PRIORITY");
    return ParsePsoWorkerPriority(value ? value : "");
}
inline size_t ParsePrewarmThreads(std::string_view value) {
    if (value.empty())
        return 2;
    unsigned parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed >= 1 &&
                   parsed <= 6
               ? parsed
               : 2;
}
inline size_t PrewarmThreads() {
    const char* value = std::getenv("NXBOX_D3D12_PREWARM_THREADS");
    return ParsePrewarmThreads(value ? value : "");
}
#ifdef NXBOX_UWP
inline constexpr bool XboxCompilePolicy = true;
#else
inline constexpr bool XboxCompilePolicy = false;
#endif
constexpr bool SkipGlobalFallbackCompute(bool enabled, bool compute, bool uses_global_memory) {
    return enabled && compute && uses_global_memory;
}
inline bool SkipGlobalFallbackComputeEnabled() {
    const char* value = std::getenv("NXBOX_D3D12_SKIP_GLOBAL_FALLBACK_COMPUTE");
    return value ? std::string_view{value} != "0" : XboxCompilePolicy;
}
inline std::uint64_t CompileHeadroomBytes() {
    const char* value = std::getenv("NXBOX_D3D12_COMPILE_HEADROOM_MB");
    std::uint64_t mb = XboxCompilePolicy ? 448 : 0;
    if (value) {
        const std::string_view text{value};
        std::uint64_t parsed{};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
            parsed <= UINT64_MAX / (1024 * 1024))
            mb = parsed;
    }
    return mb * 1024 * 1024;
}
// The caller serializes access; passing time and a reader makes caching deterministic in tests.
class CompileHeadroomGuard {
public:
    using Clock = std::chrono::steady_clock;
    template <typename Reader>
    bool Allows(std::uint64_t threshold, Clock::time_point now, Reader read) {
        if (!threshold)
            return true;
        if (!sampled || now - *sampled >= std::chrono::milliseconds{100}) {
            const auto [limit, usage] = read();
            headroom = limit > usage ? limit - usage : 0;
            sampled = now;
        }
        return headroom >= threshold;
    }
    bool ShouldLog(Clock::time_point now) {
        if (logged && now - *logged < std::chrono::seconds{5})
            return false;
        logged = now;
        return true;
    }
    std::uint64_t Headroom() const {
        return headroom;
    }

private:
    std::optional<Clock::time_point> sampled, logged;
    std::uint64_t headroom{};
};
} // namespace D3D12
