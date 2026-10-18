// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <limits>

namespace D3D12 {
// NXBOX_D3D12_FLUSH_DRAWS=N submits the open command list every N recorded draws, so the GPU starts
// on a frame while the recording thread is still producing it. Without it nothing reaches the
// queue before the guest's fence at the end of the frame, and the guest releases its next frame
// only when that fence completes: recording and execution run strictly one after the other.
//
// Anything but a positive decimal number leaves it off: 0, a sign, text, a value over 32 bits.
// Blanks around the number are ignored; `set X=128 && app` in cmd.exe leaves one behind.
constexpr std::uint32_t ParseFlushDraws(const char* text) noexcept {
    if (!text)
        return 0;
    const auto blank = [](char c) { return c == ' ' || c == '\t'; };
    while (blank(*text))
        ++text;
    std::uint64_t value = 0;
    bool digits = false;
    for (; *text >= '0' && *text <= '9'; ++text) {
        value = value * 10 + static_cast<std::uint64_t>(*text - '0');
        if (value > std::numeric_limits<std::uint32_t>::max())
            return 0;
        digits = true;
    }
    while (blank(*text))
        ++text;
    return digits && !*text ? static_cast<std::uint32_t>(value) : 0;
}

// Counts the draws recorded into the open command list on the recording thread. Any submission,
// whatever asked for it, closes that list, so the next one starts at zero.
class DrawFlushCadence {
public:
    explicit constexpr DrawFlushCadence(std::uint32_t interval) noexcept : every{interval} {}
    constexpr void Drew() noexcept {
        ++draws;
    }
    constexpr void Submitted() noexcept {
        draws = 0;
    }
    // Asked before a draw is touched: a list is cut between draws, never inside one.
    constexpr bool Due() const noexcept {
        return every != 0 && draws >= every;
    }

private:
    std::uint32_t every;
    std::uint32_t draws{};
};
} // namespace D3D12
