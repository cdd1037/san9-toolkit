#pragma once

#include <windows.h>

namespace san9::viewport {

struct Bounds {
    int x{};
    int y{};
    int width{};
    int height{};
};

Bounds Calculate(HWND window);

} // namespace san9::viewport
