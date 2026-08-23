#include "status_overlay.h"

#include "viewport.h"

#include <algorithm>
#include <string>

namespace san9::status_overlay {
namespace {

constexpr wchar_t kWindowClass[] = L"San9Toolkit.StatusOverlay";
constexpr UINT_PTR kHideTimer = 1;
constexpr UINT kDisplayDurationMilliseconds = 1800;
constexpr COLORREF kTransparentColor = RGB(255, 0, 255);

HWND g_overlay = nullptr;
HWND g_owner = nullptr;
HINSTANCE g_instance = nullptr;
std::wstring g_message;

void DrawPanel(HWND window, HDC destination) {
    RECT client{};
    if (!GetClientRect(window, &client)) {
        return;
    }
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    const HDC buffer = CreateCompatibleDC(destination);
    const HBITMAP bitmap = CreateCompatibleBitmap(destination, width, height);
    if (!buffer || !bitmap) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        if (buffer) {
            DeleteDC(buffer);
        }
        return;
    }
    const HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);

    const HBRUSH transparent = CreateSolidBrush(kTransparentColor);
    FillRect(buffer, &client, transparent);
    DeleteObject(transparent);

    RECT shadowRect = client;
    shadowRect.right -= 6;
    shadowRect.bottom -= 6;
    OffsetRect(&shadowRect, 5, 5);
    const HBRUSH shadow = CreateSolidBrush(RGB(45, 36, 24));
    const HGDIOBJ oldBrush = SelectObject(buffer, shadow);
    const HGDIOBJ oldPen = SelectObject(buffer, GetStockObject(NULL_PEN));
    RoundRect(buffer, shadowRect.left, shadowRect.top, shadowRect.right, shadowRect.bottom, 12, 12);

    RECT panel = client;
    panel.right -= 6;
    panel.bottom -= 6;
    const HBRUSH background = CreateSolidBrush(RGB(225, 207, 159));
    const HPEN outerPen = CreatePen(PS_SOLID, 3, RGB(55, 45, 30));
    SelectObject(buffer, background);
    SelectObject(buffer, outerPen);
    RoundRect(buffer, panel.left, panel.top, panel.right, panel.bottom, 12, 12);

    RECT inner = panel;
    InflateRect(&inner, -6, -6);
    const HPEN innerPen = CreatePen(PS_SOLID, 1, RGB(132, 101, 57));
    SelectObject(buffer, GetStockObject(NULL_BRUSH));
    SelectObject(buffer, innerPen);
    RoundRect(buffer, inner.left, inner.top, inner.right, inner.bottom, 8, 8);

    const int fontHeight = std::max(24, MulDiv(height, 30, 100));
    const HFONT font = CreateFontW(-fontHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                   L"Microsoft JhengHei UI");
    const HGDIOBJ oldFont = font ? SelectObject(buffer, font) : nullptr;
    SetBkMode(buffer, TRANSPARENT);
    SetTextColor(buffer, RGB(28, 24, 18));
    RECT textRect = panel;
    InflateRect(&textRect, -16, -10);
    DrawTextW(buffer, g_message.c_str(), -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    BitBlt(destination, 0, 0, width, height, buffer, 0, 0, SRCCOPY);

    if (oldFont) {
        SelectObject(buffer, oldFont);
    }
    SelectObject(buffer, oldPen);
    SelectObject(buffer, oldBrush);
    if (font) {
        DeleteObject(font);
    }
    DeleteObject(innerPen);
    DeleteObject(outerPen);
    DeleteObject(background);
    DeleteObject(shadow);
    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}

LRESULT CALLBACK OverlayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC destination = BeginPaint(window, &paint);
        DrawPanel(window, destination);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kHideTimer) {
            Hide();
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        KillTimer(window, kHideTimer);
        g_overlay = nullptr;
        g_owner = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool EnsureWindow(HWND owner) {
    if (g_overlay) {
        return true;
    }
    if (!g_instance) {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&OverlayWindowProc), &module)) {
            return false;
        }
        g_instance = module;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &OverlayWindowProc;
    windowClass.hInstance = g_instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                    WS_EX_TOOLWINDOW,
                                kWindowClass, L"", WS_POPUP, 0, 0, 1, 1,
                                owner, nullptr, g_instance, nullptr);
    if (!g_overlay) {
        return false;
    }
    g_owner = owner;
    if (!SetLayeredWindowAttributes(g_overlay, kTransparentColor, 245,
                                    LWA_COLORKEY | LWA_ALPHA)) {
        DestroyWindow(g_overlay);
        return false;
    }
    return true;
}

} // namespace

void ShowCursorLockState(HWND owner, bool enabled) {
    ShowMessage(owner, enabled ? L"鼠标已锁定在游戏画面内" : L"鼠标锁定已解除");
}

void ShowMessage(HWND owner, const wchar_t* message) {
    if (!EnsureWindow(owner)) {
        return;
    }
    g_message = message ? message : L"";
    UpdatePosition(owner);
    InvalidateRect(g_overlay, nullptr, FALSE);
    ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
    SetTimer(g_overlay, kHideTimer, kDisplayDurationMilliseconds, nullptr);
}

void UpdatePosition(HWND owner) {
    if (!g_overlay || owner != g_owner || IsIconic(owner)) {
        Hide();
        return;
    }
    const viewport::Bounds bounds = viewport::Calculate(owner);
    if (bounds.width <= 0 || bounds.height <= 0) {
        Hide();
        return;
    }
    const int panelWidth = std::min(std::max(1, bounds.width - 16),
                                    std::max(260, MulDiv(bounds.width, 500, 1024)));
    const int panelHeight = std::min(std::max(1, bounds.height - 16),
                                     std::max(64, MulDiv(bounds.height, 100, 768)));
    POINT origin{bounds.x + (bounds.width - panelWidth) / 2,
                 bounds.y + (bounds.height - panelHeight) / 2};
    if (!ClientToScreen(owner, &origin)) {
        Hide();
        return;
    }
    SetWindowPos(g_overlay, HWND_TOP, origin.x, origin.y, panelWidth, panelHeight,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void Hide() {
    if (g_overlay) {
        KillTimer(g_overlay, kHideTimer);
        ShowWindow(g_overlay, SW_HIDE);
    }
}

void Shutdown() {
    if (g_overlay) {
        PostMessageW(g_overlay, WM_CLOSE, 0, 0);
    }
}

} // namespace san9::status_overlay
