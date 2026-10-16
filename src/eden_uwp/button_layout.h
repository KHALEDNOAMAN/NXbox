// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace EdenXbox {
enum class ButtonLayout { Name, Position };

// NativeButton's first four slots are A, B, X, Y. Position swaps only A/B; X/Y stay
// unchanged. This is its own inverse for physical -> guest and guest -> physical.
inline constexpr int FaceButtonIndex(int index, ButtonLayout layout) {
    return layout == ButtonLayout::Position && index >= 0 && index < 2 ? index ^ 1 : index;
}
} // namespace EdenXbox
