#include <windows.h>
#include <windowsx.h>

#include "code_hook.h"
#include "cursor_lock.h"
#include "d3d11_presenter.h"
#include "import_hook.h"
#include "registry_overlay.h"
#include "viewport.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace {

constexpr int kLogicalWidth = 1024;
constexpr int kLogicalHeight = 768;
constexpr char kWindowClass[] = "KOEI_SAN9WINDOW";
constexpr wchar_t kWindowTitle[] = L"三國志ⅨPK";
constexpr char kStatusProperty[] = "San9Toolkit.RuntimeStatus";
constexpr wchar_t kBootEventEnvironmentVariable[] = L"SAN9_TOOLKIT_BOOT_EVENT";
constexpr UINT kRedrawMessage = WM_APP + 0x319;
constexpr std::uintptr_t kNormalizeWindowMessageRva = 0x1CC0B0;
constexpr std::size_t kNormalizeWindowMessagePrologueSize = 8;
constexpr UINT kDefaultDpi = 96;

struct RuntimeOptions {
    bool scaleInitialWindowForSystemDpi;
    bool accelerateGameClock;
    DWORD gameClockRate;
};

constexpr RuntimeOptions kRuntimeOptions{
    .scaleInitialWindowForSystemDpi = true,
    .accelerateGameClock = true,
    .gameClockRate = 2,
};

static_assert(kRuntimeOptions.gameClockRate >= 1);

using BitBltFunction = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, DWORD);
using GetCursorPosFunction = BOOL(WINAPI*)(LPPOINT);
using ReleaseDcFunction = int(WINAPI*)(HWND, HDC);
using TimeGetTimeFunction = DWORD(WINAPI*)();
using NormalizeWindowMessageFunction = int(__cdecl*)(MSG*, MSG*);

HWND g_window = nullptr;
WNDPROC g_originalWindowProc = nullptr;
BitBltFunction g_originalBitBlt = nullptr;
GetCursorPosFunction g_originalGetCursorPos = nullptr;
ReleaseDcFunction g_originalReleaseDc = nullptr;
TimeGetTimeFunction g_originalTimeGetTime = nullptr;
NormalizeWindowMessageFunction g_originalNormalizeWindowMessage = nullptr;
HDC g_framebufferDc = nullptr;
volatile LONG g_presentSerial = 0;
volatile LONG g_windowBehaviorInstalled = 0;
SRWLOCK g_gameClockLock = SRWLOCK_INIT;
bool g_gameClockInitialized = false;
DWORD g_lastRealGameTick = 0;
DWORD g_scaledGameTick = 0;

using Viewport = san9::viewport::Bounds;

bool IsLogicalFramebuffer(HDC source) {
    if (!source || GetObjectType(source) != OBJ_MEMDC) {
        return false;
    }
    const HGDIOBJ selectedBitmap = GetCurrentObject(source, OBJ_BITMAP);
    BITMAP bitmap{};
    return selectedBitmap && GetObjectW(selectedBitmap, sizeof(bitmap), &bitmap) == sizeof(bitmap) &&
           bitmap.bmWidth == kLogicalWidth && std::abs(bitmap.bmHeight) == kLogicalHeight &&
           bitmap.bmBitsPixel == 16;
}

bool IsTargetWindow(HWND window) {
    if (!window) {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId()) {
        return false;
    }
    char className[64]{};
    return GetClassNameA(window, className, static_cast<int>(sizeof(className))) > 0 &&
           std::strcmp(className, kWindowClass) == 0;
}

bool RenderWindow() {
    return IsLogicalFramebuffer(g_framebufferDc) &&
           san9::d3d11_presenter::PresentFrame(g_framebufferDc);
}

bool CalculateInitialOuterRect(HWND window, LONG_PTR style, LONG_PTR exStyle,
                               RECT& outer) {
    const UINT dpi = GetDpiForWindow(window);
    if (dpi == 0) {
        return false;
    }
    const int clientWidth = kRuntimeOptions.scaleInitialWindowForSystemDpi
                                ? MulDiv(kLogicalWidth, static_cast<int>(dpi), kDefaultDpi)
                                : kLogicalWidth;
    const int clientHeight = kRuntimeOptions.scaleInitialWindowForSystemDpi
                                 ? MulDiv(kLogicalHeight, static_cast<int>(dpi), kDefaultDpi)
                                 : kLogicalHeight;
    outer = {0, 0, clientWidth, clientHeight};
    return AdjustWindowRectExForDpi(&outer, static_cast<DWORD>(style), FALSE,
                                    static_cast<DWORD>(exStyle), dpi) != FALSE;
}

bool MapPhysicalPoint(HWND window, POINT physical, POINT& logical) {
    const Viewport viewport = san9::viewport::Calculate(window);
    if (viewport.width <= 0 || viewport.height <= 0) {
        return false;
    }
    const bool inside = physical.x >= viewport.x && physical.y >= viewport.y &&
                        physical.x < viewport.x + viewport.width &&
                        physical.y < viewport.y + viewport.height;
    const int clampedX = std::clamp(static_cast<int>(physical.x), viewport.x, viewport.x + viewport.width - 1);
    const int clampedY = std::clamp(static_cast<int>(physical.y), viewport.y, viewport.y + viewport.height - 1);
    logical.x = std::clamp(MulDiv(clampedX - viewport.x, kLogicalWidth, viewport.width), 0, kLogicalWidth - 1);
    logical.y = std::clamp(MulDiv(clampedY - viewport.y, kLogicalHeight, viewport.height), 0, kLogicalHeight - 1);
    return inside;
}

bool IsClientMouseMessage(UINT message) {
    switch (message) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        return true;
    default:
        return false;
    }
}

bool IsWheelMouseMessage(UINT message) {
    return message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL;
}

bool IsHardwareMouseMessage(const MSG& message) {
    POINT messageScreen{};
    if (IsClientMouseMessage(message.message)) {
        messageScreen = {GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)};
        if (!ClientToScreen(message.hwnd, &messageScreen)) {
            return false;
        }
    } else if (IsWheelMouseMessage(message.message)) {
        messageScreen = {GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)};
    } else {
        return false;
    }
    return messageScreen.x == message.pt.x && messageScreen.y == message.pt.y;
}

BOOL WINAPI ScaledGetCursorPos(LPPOINT screenPoint) {
    if (!screenPoint || !g_originalGetCursorPos(screenPoint)) {
        return FALSE;
    }
    if (InterlockedCompareExchange(&g_windowBehaviorInstalled, 0, 0) == 0) {
        return TRUE;
    }

    const HWND window = g_window;
    POINT physical = *screenPoint;
    if (!window || !ScreenToClient(window, &physical)) {
        return TRUE;
    }
    POINT logical{};
    RECT outer{};
    if (!MapPhysicalPoint(window, physical, logical) || !GetWindowRect(window, &outer)) {
        return TRUE;
    }
    screenPoint->x = outer.left + logical.x;
    screenPoint->y = outer.top + logical.y;
    return TRUE;
}

int __cdecl ScaledNormalizeWindowMessage(MSG* normalizedMessage, MSG* sourceMessage) {
    const HWND window = g_window;
    if (sourceMessage && window && sourceMessage->hwnd == window &&
        InterlockedCompareExchange(&g_windowBehaviorInstalled, 0, 0) != 0) {
        if (san9::cursor_lock::HandleInputMessage(window, *sourceMessage)) {
            return g_originalNormalizeWindowMessage(normalizedMessage, sourceMessage);
        }
        if (!IsHardwareMouseMessage(*sourceMessage)) {
            return g_originalNormalizeWindowMessage(normalizedMessage, sourceMessage);
        }
        POINT physical{GET_X_LPARAM(sourceMessage->lParam), GET_Y_LPARAM(sourceMessage->lParam)};
        const bool wheelMessage = IsWheelMouseMessage(sourceMessage->message);
        if (wheelMessage && !ScreenToClient(window, &physical)) {
            return g_originalNormalizeWindowMessage(normalizedMessage, sourceMessage);
        }
        POINT logical{};
        const bool inside = MapPhysicalPoint(window, physical, logical);
        if (!inside && GetCapture() != window) {
            sourceMessage->message = WM_NULL;
        } else {
            if (wheelMessage && !ClientToScreen(window, &logical)) {
                return g_originalNormalizeWindowMessage(normalizedMessage, sourceMessage);
            }
            sourceMessage->lParam = MAKELPARAM(static_cast<short>(logical.x),
                                               static_cast<short>(logical.y));
        }
    }
    return g_originalNormalizeWindowMessage(normalizedMessage, sourceMessage);
}

void EnforceFourByThree(HWND window, WPARAM edge, RECT& outer) {
    const LONG_PTR style = GetWindowLongPtrA(window, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrA(window, GWL_EXSTYLE);
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectEx(&frame, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(exStyle));
    const int frameWidth = frame.right - frame.left;
    const int frameHeight = frame.bottom - frame.top;
    int clientWidth = std::max(1L, outer.right - outer.left - frameWidth);
    int clientHeight = std::max(1L, outer.bottom - outer.top - frameHeight);

    const bool verticalEdge = edge == WMSZ_TOP || edge == WMSZ_BOTTOM;
    if (verticalEdge) {
        clientWidth = MulDiv(clientHeight, kLogicalWidth, kLogicalHeight);
    } else {
        clientHeight = MulDiv(clientWidth, kLogicalHeight, kLogicalWidth);
    }
    const int outerWidth = clientWidth + frameWidth;
    const int outerHeight = clientHeight + frameHeight;

    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) {
        outer.left = outer.right - outerWidth;
    } else {
        outer.right = outer.left + outerWidth;
    }
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) {
        outer.top = outer.bottom - outerHeight;
    } else {
        outer.bottom = outer.top + outerHeight;
    }
}

LRESULT CALLBACK ScalerWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (san9::cursor_lock::HandleWindowMessageBefore(window, message, wParam, lParam)) {
        return 0;
    }
    if (message == WM_SIZING) {
        EnforceFourByThree(window, wParam, *reinterpret_cast<RECT*>(lParam));
        return TRUE;
    }
    if (message == WM_ERASEBKGND) {
        return TRUE;
    }
    if (message == kRedrawMessage) {
        RenderWindow();
        return 0;
    }
    const LONG presentBefore = message == WM_PAINT
                                   ? InterlockedCompareExchange(&g_presentSerial, 0, 0)
                                   : 0;
    const LRESULT result = CallWindowProcA(g_originalWindowProc, window, message, wParam, lParam);
    if (message == WM_PAINT &&
        InterlockedCompareExchange(&g_presentSerial, 0, 0) == presentBefore) {
        RenderWindow();
    } else if (message == WM_SIZE) {
        InvalidateRect(window, nullptr, FALSE);
    }
    san9::cursor_lock::HandleWindowMessageAfter(window, message);
    return result;
}

BOOL WINAPI ScaledBitBlt(HDC destination, int xDest, int yDest, int width, int height,
                         HDC source, int xSource, int ySource, DWORD rop) {
    const HWND destinationWindow = WindowFromDC(destination);
    const bool confirmedPresent = rop == SRCCOPY && IsTargetWindow(destinationWindow) &&
                                  IsLogicalFramebuffer(source) && width > 0 && height > 0 &&
                                  xSource >= 0 && ySource >= 0 &&
                                  xSource + width <= kLogicalWidth &&
                                  ySource + height <= kLogicalHeight;
    if (!confirmedPresent) {
        if (destinationWindow == g_window &&
            InterlockedCompareExchange(&g_windowBehaviorInstalled, 0, 0) != 0) {
            return TRUE;
        }
        return g_originalBitBlt(destination, xDest, yDest, width, height, source, xSource, ySource, rop);
    }

    InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&g_window),
                                      destinationWindow, nullptr);
    g_framebufferDc = source;
    if (destinationWindow != g_window ||
        InterlockedCompareExchange(&g_windowBehaviorInstalled, 0, 0) == 0) {
        return g_originalBitBlt(destination, xDest, yDest, width, height,
                                source, xSource, ySource, rop);
    }
    const BOOL result = san9::d3d11_presenter::QueueFrame(source) ? TRUE : FALSE;
    if (result) {
        InterlockedIncrement(&g_presentSerial);
    }
    return result;
}

int WINAPI ScaledReleaseDc(HWND window, HDC deviceContext) {
    const int result = g_originalReleaseDc(window, deviceContext);
    if (window == g_window &&
        InterlockedCompareExchange(&g_windowBehaviorInstalled, 0, 0) != 0 &&
        !san9::d3d11_presenter::PresentPendingFrame()) {
        OutputDebugStringW(L"San9Toolkit: D3D11 frame presentation failed after ReleaseDC.\n");
    }
    return result;
}

DWORD WINAPI ScaledTimeGetTime() {
    AcquireSRWLockExclusive(&g_gameClockLock);
    const DWORD realTick = g_originalTimeGetTime();
    if (!g_gameClockInitialized) {
        g_lastRealGameTick = realTick;
        g_scaledGameTick = realTick;
        g_gameClockInitialized = true;
    } else {
        const DWORD elapsed = realTick - g_lastRealGameTick;
        g_lastRealGameTick = realTick;
        g_scaledGameTick += elapsed * kRuntimeOptions.gameClockRate;
    }
    const DWORD result = g_scaledGameTick;
    ReleaseSRWLockExclusive(&g_gameClockLock);
    return result;
}

bool InstallGameClock() {
    return !kRuntimeOptions.accelerateGameClock ||
           san9::import_hook::Install("winmm.dll", "timeGetTime", &ScaledTimeGetTime,
                                      g_originalTimeGetTime);
}

bool PatchNormalizeWindowMessage() {
    auto* module = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!module) {
        return false;
    }
    auto* entry = module + kNormalizeWindowMessageRva;
    constexpr std::array<unsigned char, kNormalizeWindowMessagePrologueSize> expected{
        0x83, 0xEC, 0x0C, 0x53, 0x8B, 0x5C, 0x24, 0x18};
    void* trampoline = nullptr;
    if (!san9::code_hook::Install(entry, reinterpret_cast<void*>(&ScaledNormalizeWindowMessage),
                                  expected, &trampoline)) {
        return false;
    }
    g_originalNormalizeWindowMessage =
        reinterpret_cast<NormalizeWindowMessageFunction>(trampoline);
    return true;
}

bool PatchRequiredHooks() {
    return san9::import_hook::Install("gdi32.dll", "BitBlt", &ScaledBitBlt, g_originalBitBlt) &&
           san9::import_hook::Install("user32.dll", "GetCursorPos", &ScaledGetCursorPos,
                                      g_originalGetCursorPos) &&
           san9::import_hook::Install("user32.dll", "ReleaseDC", &ScaledReleaseDc,
                                      g_originalReleaseDc) &&
           san9::registry_overlay::Install() &&
           InstallGameClock() &&
           PatchNormalizeWindowMessage();
}

bool SignalBootReady() {
    std::array<wchar_t, 256> eventName{};
    const DWORD length = GetEnvironmentVariableW(
        kBootEventEnvironmentVariable, eventName.data(),
        static_cast<DWORD>(eventName.size()));
    if (length == 0 || length >= eventName.size()) {
        return false;
    }
    const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.data());
    if (!event) {
        return false;
    }
    const bool signaled = SetEvent(event) != FALSE;
    CloseHandle(event);
    return signaled;
}

bool InstallWindowBehavior(HWND window) {
    RECT outer{};
    if (!GetWindowRect(window, &outer)) {
        return false;
    }

    const LONG_PTR oldStyle = GetWindowLongPtrA(window, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrA(window, GWL_EXSTYLE);
    const LONG_PTR newStyle = (oldStyle & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_OVERLAPPEDWINDOW;
    SetLastError(ERROR_SUCCESS);
    if (SetWindowLongPtrA(window, GWL_STYLE, newStyle) == 0 && GetLastError() != ERROR_SUCCESS) {
        return false;
    }
    RECT desired{};
    if (!CalculateInitialOuterRect(window, newStyle, exStyle, desired)) {
        return false;
    }
    if (!SetWindowPos(window, nullptr, outer.left, outer.top,
                      desired.right - desired.left, desired.bottom - desired.top,
                      SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED)) {
        return false;
    }
    g_originalWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
        window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ScalerWindowProc)));
    if (!g_originalWindowProc) {
        return false;
    }
    if (!SetWindowTextW(window, kWindowTitle)) {
        return false;
    }
    if (!san9::d3d11_presenter::Initialize(window)) {
        return false;
    }
    san9::cursor_lock::Initialize(window);
    return true;
}

DWORD WINAPI InstallThread(void*) {
    if (!PatchRequiredHooks()) {
        OutputDebugStringW(L"San9Toolkit: required hooks failed.\n");
        return 2;
    }

    if (!SignalBootReady()) {
        OutputDebugStringW(L"San9Toolkit: launcher boot handshake failed.\n");
        return 4;
    }

    for (int attempt = 0; attempt < 600 && (!g_window || !g_framebufferDc); ++attempt) {
        Sleep(50);
    }
    const HWND window = g_window;
    if (!IsTargetWindow(window) || !IsLogicalFramebuffer(g_framebufferDc)) {
        OutputDebugStringW(L"San9Toolkit: confirmed framebuffer window not found.\n");
        return 1;
    }

    if (!InstallWindowBehavior(window)) {
        SetPropA(window, kStatusProperty, reinterpret_cast<HANDLE>(30));
        OutputDebugStringW(L"San9Toolkit: window subclass failed.\n");
        return 3;
    }
    InterlockedExchange(&g_windowBehaviorInstalled, 1);
    PostMessageA(window, kRedrawMessage, 0, 0);
    SetPropA(window, kStatusProperty, reinterpret_cast<HANDLE>(1));
    OutputDebugStringW(L"San9Toolkit: installed.\n");
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void* reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        const HANDLE thread = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        san9::registry_overlay::Shutdown();
        san9::cursor_lock::Shutdown();
        san9::d3d11_presenter::Shutdown();
    }
    return TRUE;
}
