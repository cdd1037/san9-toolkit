#include "viewport.h"

#include <algorithm>
#include <cstdint>

namespace san9::viewport {
namespace {

constexpr int kLogicalWidth = 1024;
constexpr int kLogicalHeight = 768;

} // namespace

Bounds Calculate(HWND window) {
    RECT client{};
    if (!GetClientRect(window, &client)) {
        return {};
    }
    const int clientWidth = std::max(0L, client.right - client.left);
    const int clientHeight = std::max(0L, client.bottom - client.top);
    if (clientWidth == 0 || clientHeight == 0) {
        return {};
    }

    Bounds viewport{};
    if (static_cast<std::int64_t>(clientWidth) * kLogicalHeight <=
        static_cast<std::int64_t>(clientHeight) * kLogicalWidth) {
        viewport.width = clientWidth;
        viewport.height = MulDiv(clientWidth, kLogicalHeight, kLogicalWidth);
    } else {
        viewport.height = clientHeight;
        viewport.width = MulDiv(clientHeight, kLogicalWidth, kLogicalHeight);
    }
    viewport.x = (clientWidth - viewport.width) / 2;
    viewport.y = (clientHeight - viewport.height) / 2;
    return viewport;
}

} // namespace san9::viewport
