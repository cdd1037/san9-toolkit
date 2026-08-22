#include <windows.h>
#include <windowsx.h>

#include "cursor_lock.h"
#include "viewport.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr int kLogicalWidth = 1024;
constexpr int kLogicalHeight = 768;
constexpr char kWindowClass[] = "KOEI_SAN9WINDOW";
constexpr wchar_t kWindowTitle[] = L"三國志ⅨPK";
constexpr char kStatusProperty[] = "San9Toolkit.RuntimeStatus";
constexpr UINT kRedrawMessage = WM_APP + 0x319;
constexpr std::uintptr_t kNormalizeWindowMessageRva = 0x1CC0B0;
constexpr std::size_t kNormalizeWindowMessagePrologueSize = 8;

using BitBltFunction = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, DWORD);
using GetCursorPosFunction = BOOL(WINAPI*)(LPPOINT);
using NormalizeWindowMessageFunction = int(__cdecl*)(MSG*, MSG*);

HWND g_window = nullptr;
WNDPROC g_originalWindowProc = nullptr;
BitBltFunction g_originalBitBlt = nullptr;
GetCursorPosFunction g_originalGetCursorPos = nullptr;
NormalizeWindowMessageFunction g_originalNormalizeWindowMessage = nullptr;
HDC g_framebufferDc = nullptr;
volatile LONG g_clearClient = 1;
volatile LONG g_presentSerial = 0;
volatile LONG g_windowBehaviorInstalled = 0;

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

void ClearLetterbox(HDC destination, const RECT& client, const Viewport& viewport) {
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    if (viewport.y > 0) {
        PatBlt(destination, 0, 0, clientWidth, viewport.y, BLACKNESS);
    }
    const int bottom = viewport.y + viewport.height;
    if (bottom < clientHeight) {
        PatBlt(destination, 0, bottom, clientWidth, clientHeight - bottom, BLACKNESS);
    }
    if (viewport.x > 0) {
        PatBlt(destination, 0, viewport.y, viewport.x, viewport.height, BLACKNESS);
    }
    const int right = viewport.x + viewport.width;
    if (right < clientWidth) {
        PatBlt(destination, right, viewport.y, clientWidth - right, viewport.height, BLACKNESS);
    }
}

bool RenderFullFrame(HWND window, HDC destination) {
    const HDC source = g_framebufferDc;
    if (!destination || !IsLogicalFramebuffer(source)) {
        return false;
    }
    const Viewport viewport = san9::viewport::Calculate(window);
    if (viewport.width <= 0 || viewport.height <= 0) {
        return false;
    }

    RECT client{};
    if (!GetClientRect(window, &client)) {
        return false;
    }
    ClearLetterbox(destination, client, viewport);
    const int previousMode = SetStretchBltMode(destination, COLORONCOLOR);
    const BOOL result = StretchBlt(destination, viewport.x, viewport.y, viewport.width, viewport.height,
                                   source, 0, 0, kLogicalWidth, kLogicalHeight, SRCCOPY);
    if (previousMode != 0) {
        SetStretchBltMode(destination, previousMode);
    }
    return result != FALSE;
}

bool RenderWindow(HWND window) {
    const HDC destination = GetDC(window);
    if (!destination) {
        return false;
    }
    const bool result = RenderFullFrame(window, destination);
    ReleaseDC(window, destination);
    return result;
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
        InterlockedCompareExchange(&g_windowBehaviorInstalled, 0, 0) != 0 &&
        IsHardwareMouseMessage(*sourceMessage)) {
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
        RenderWindow(window);
        return 0;
    }
    if (message == WM_SIZE) {
        InterlockedExchange(&g_clearClient, 1);
    }

    const LONG presentBefore = message == WM_PAINT
                                   ? InterlockedCompareExchange(&g_presentSerial, 0, 0)
                                   : 0;
    const LRESULT result = CallWindowProcA(g_originalWindowProc, window, message, wParam, lParam);
    if (message == WM_PAINT &&
        InterlockedCompareExchange(&g_presentSerial, 0, 0) == presentBefore) {
        RenderWindow(window);
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
    const Viewport viewport = san9::viewport::Calculate(g_window);
    if (viewport.width <= 0 || viewport.height <= 0) {
        return FALSE;
    }
    if (InterlockedExchange(&g_clearClient, 0) != 0) {
        RECT client{};
        GetClientRect(g_window, &client);
        ClearLetterbox(destination, client, viewport);
    }

    const int previousMode = SetStretchBltMode(destination, COLORONCOLOR);
    const BOOL result = StretchBlt(destination, viewport.x, viewport.y,
                                   viewport.width, viewport.height, source, 0, 0,
                                   kLogicalWidth, kLogicalHeight, SRCCOPY);
    if (previousMode != 0) {
        SetStretchBltMode(destination, previousMode);
    }
    if (result) {
        InterlockedIncrement(&g_presentSerial);
    }
    return result;
}

template <typename Function>
bool PatchImport(const char* importedModule, const char* importedFunction,
                 Function replacement, Function& original) {
    auto* module = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!module) {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return false;
    }
    const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) {
        return false;
    }

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(module + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* moduleName = reinterpret_cast<const char*>(module + descriptor->Name);
        if (_stricmp(moduleName, importedModule) != 0) {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(module + descriptor->OriginalFirstThunk);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA32*>(module + descriptor->FirstThunk);
        if (!descriptor->OriginalFirstThunk) {
            return false;
        }
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) {
                continue;
            }
            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(module + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), importedFunction) != 0) {
                continue;
            }
            DWORD oldProtection = 0;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function), PAGE_READWRITE, &oldProtection)) {
                return false;
            }
            original = reinterpret_cast<Function>(addresses->u1.Function);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&addresses->u1.Function),
                                reinterpret_cast<LONG>(replacement));
            DWORD ignored = 0;
            VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function), oldProtection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), &addresses->u1.Function, sizeof(addresses->u1.Function));
            return true;
        }
    }
    return false;
}

bool WriteRelativeJump(unsigned char* source, const void* target) {
    const auto delta = reinterpret_cast<std::intptr_t>(target) -
                       reinterpret_cast<std::intptr_t>(source + 5);
    if (delta < std::numeric_limits<std::int32_t>::min() ||
        delta > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    source[0] = 0xE9;
    const auto displacement = static_cast<std::int32_t>(delta);
    std::memcpy(source + 1, &displacement, sizeof(displacement));
    return true;
}

bool PatchNormalizeWindowMessage() {
    auto* module = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!module) {
        return false;
    }
    auto* entry = module + kNormalizeWindowMessageRva;
    constexpr std::array<unsigned char, kNormalizeWindowMessagePrologueSize> expected{
        0x83, 0xEC, 0x0C, 0x53, 0x8B, 0x5C, 0x24, 0x18};
    if (std::memcmp(entry, expected.data(), expected.size()) != 0) {
        return false;
    }

    constexpr std::size_t trampolineSize = kNormalizeWindowMessagePrologueSize + 5;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, trampolineSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return false;
    }
    std::memcpy(trampoline, entry, kNormalizeWindowMessagePrologueSize);
    if (!WriteRelativeJump(trampoline + kNormalizeWindowMessagePrologueSize,
                           entry + kNormalizeWindowMessagePrologueSize)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(entry, kNormalizeWindowMessagePrologueSize,
                        PAGE_EXECUTE_READWRITE, &oldProtection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    g_originalNormalizeWindowMessage =
        reinterpret_cast<NormalizeWindowMessageFunction>(trampoline);
    const bool jumpWritten = WriteRelativeJump(entry, &ScaledNormalizeWindowMessage);
    if (jumpWritten) {
        std::memset(entry + 5, 0x90, kNormalizeWindowMessagePrologueSize - 5);
    }
    DWORD ignored = 0;
    const BOOL restored = VirtualProtect(entry, kNormalizeWindowMessagePrologueSize,
                                         oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), entry, kNormalizeWindowMessagePrologueSize);
    if (!jumpWritten || !restored) {
        return false;
    }
    return true;
}

bool PatchRequiredHooks() {
    return PatchImport("gdi32.dll", "BitBlt", &ScaledBitBlt, g_originalBitBlt) &&
           PatchImport("user32.dll", "GetCursorPos", &ScaledGetCursorPos, g_originalGetCursorPos) &&
           PatchNormalizeWindowMessage();
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
    RECT desired{0, 0, kLogicalWidth, kLogicalHeight};
    if (!AdjustWindowRectEx(&desired, static_cast<DWORD>(newStyle), FALSE, static_cast<DWORD>(exStyle))) {
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
    san9::cursor_lock::Initialize(window);
    return true;
}

DWORD WINAPI InstallThread(void*) {
    if (!PatchRequiredHooks()) {
        OutputDebugStringW(L"San9Toolkit: required hooks failed.\n");
        return 2;
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
        san9::cursor_lock::Shutdown();
    }
    return TRUE;
}
