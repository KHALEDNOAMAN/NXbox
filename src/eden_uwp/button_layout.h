// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace EdenXbox {
enum class ButtonLayout { Name, Position };

// NativeButton's first four slots are A, B, X, Y. The positional permutation is
// its own inverse: use it for physical -> guest and guest -> physical alike.
inline constexpr int FaceButtonIndex(int index, ButtonLayout layout) {
    return layout == ButtonLayout::Position && index >= 0 && index < 4 ? index ^ 1 : index;
}
} // namespace EdenXbox
