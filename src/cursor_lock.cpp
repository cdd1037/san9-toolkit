#include "cursor_lock.h"

#include "status_overlay.h"
#include "viewport.h"

namespace san9::cursor_lock {
namespace {

constexpr WPARAM kToggleKey = VK_F12;

HWND g_window = nullptr;
volatile LONG g_enabled = 0;
volatile LONG g_clipApplied = 0;
volatile LONG g_windowActive = 0;
volatile LONG g_applicationActive = 0;
volatile LONG g_windowMovingOrSizing = 0;

bool IsFlagSet(volatile LONG& flag) {
    return InterlockedCompareExchange(&flag, 0, 0) != 0;
}

void ReleaseClip() {
    if (InterlockedExchange(&g_clipApplied, 0) != 0) {
        ClipCursor(nullptr);
    }
}

void Refresh(HWND window) {
    const bool shouldApply = window == g_window &&
                             IsFlagSet(g_enabled) &&
                             IsFlagSet(g_windowActive) &&
                             IsFlagSet(g_applicationActive) &&
                             !IsFlagSet(g_windowMovingOrSizing) &&
                             !IsIconic(window);
    if (!shouldApply) {
        ReleaseClip();
        return;
    }

    const viewport::Bounds bounds = viewport::Calculate(window);
    if (bounds.width <= 0 || bounds.height <= 0) {
        ReleaseClip();
        return;
    }

    POINT corners[2]{{bounds.x, bounds.y},
                     {bounds.x + bounds.width, bounds.y + bounds.height}};
    SetLastError(ERROR_SUCCESS);
    if (MapWindowPoints(window, nullptr, corners, 2) == 0 && GetLastError() != ERROR_SUCCESS) {
        ReleaseClip();
        return;
    }
    const RECT clip{corners[0].x, corners[0].y, corners[1].x, corners[1].y};
    if (ClipCursor(&clip)) {
        InterlockedExchange(&g_clipApplied, 1);
    } else {
        ReleaseClip();
    }
}

bool Toggle(HWND window) {
    const LONG enabled = InterlockedCompareExchange(&g_enabled, 0, 0);
    const bool nowEnabled = enabled == 0;
    InterlockedExchange(&g_enabled, nowEnabled ? 1 : 0);
    Refresh(window);
    return nowEnabled;
}

void HandleLifecycleBefore(UINT message, WPARAM wParam) {
    if (message == WM_ACTIVATE) {
        InterlockedExchange(&g_windowActive, LOWORD(wParam) == WA_INACTIVE ? 0 : 1);
        if (!IsFlagSet(g_windowActive)) {
            ReleaseClip();
            status_overlay::Hide();
        }
    } else if (message == WM_ACTIVATEAPP) {
        InterlockedExchange(&g_applicationActive, wParam == FALSE ? 0 : 1);
        if (!IsFlagSet(g_applicationActive)) {
            ReleaseClip();
            status_overlay::Hide();
        }
    } else if (message == WM_ENTERSIZEMOVE) {
        InterlockedExchange(&g_windowMovingOrSizing, 1);
        ReleaseClip();
    } else if (message == WM_DESTROY || message == WM_NCDESTROY) {
        Shutdown();
    }
}

void HandleLifecycleAfter(HWND window, UINT message) {
    if (message == WM_EXITSIZEMOVE) {
        InterlockedExchange(&g_windowMovingOrSizing, 0);
    }
    switch (message) {
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
    case WM_MOVE:
    case WM_SIZE:
    case WM_WINDOWPOSCHANGED:
    case WM_DPICHANGED:
    case WM_EXITSIZEMOVE:
        Refresh(window);
        status_overlay::UpdatePosition(window);
        break;
    default:
        break;
    }
}

} // namespace

void Initialize(HWND window) {
    g_window = window;
    const bool active = GetForegroundWindow() == window;
    InterlockedExchange(&g_windowActive, active ? 1 : 0);
    InterlockedExchange(&g_applicationActive, active ? 1 : 0);
}

bool HandleInputMessage(HWND window, MSG& message) {
    HandleLifecycleBefore(message.message, message.wParam);
    HandleLifecycleAfter(window, message.message);

    if ((message.message == WM_KEYDOWN || message.message == WM_KEYUP) &&
        message.wParam == kToggleKey) {
        if (message.message == WM_KEYDOWN &&
            (message.lParam & (static_cast<LPARAM>(1) << 30)) == 0) {
            const bool enabled = Toggle(window);
            status_overlay::ShowCursorLockState(window, enabled);
        }
        message.message = WM_NULL;
        return true;
    }
    return false;
}

bool HandleWindowMessageBefore(HWND, UINT message, WPARAM wParam, LPARAM) {
    HandleLifecycleBefore(message, wParam);
    return false;
}

void HandleWindowMessageAfter(HWND window, UINT message) {
    HandleLifecycleAfter(window, message);
}

void Shutdown() {
    InterlockedExchange(&g_enabled, 0);
    ReleaseClip();
    status_overlay::Shutdown();
    g_window = nullptr;
}

} // namespace san9::cursor_lock
