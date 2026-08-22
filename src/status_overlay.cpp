#include "status_overlay.h"

#include <algorithm>

namespace san9::status_overlay {
namespace {

constexpr DWORD kDisplayDurationMilliseconds = 1800;
constexpr LONG kHidden = 0;
constexpr LONG kCursorLocked = 1;
constexpr LONG kCursorReleased = 2;

volatile LONG g_message = kHidden;
volatile LONG g_deadline = 0;
HFONT g_font = nullptr;
int g_fontHeight = 0;

HFONT GetFont(int height) {
    if (g_font && g_fontHeight == height) {
        return g_font;
    }
    if (g_font) {
        DeleteObject(g_font);
    }
    g_fontHeight = height;
    g_font = CreateFontW(-height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Microsoft JhengHei UI");
    return g_font;
}

} // namespace

void ShowCursorLockState(bool enabled) {
    InterlockedExchange(&g_message, enabled ? kCursorLocked : kCursorReleased);
    InterlockedExchange(&g_deadline,
                        static_cast<LONG>(GetTickCount() + kDisplayDurationMilliseconds));
}

void Draw(HDC destination, const RECT& viewport) {
    const LONG message = InterlockedCompareExchange(&g_message, kHidden, kHidden);
    if (message == kHidden) {
        return;
    }
    const DWORD deadline = static_cast<DWORD>(
        InterlockedCompareExchange(&g_deadline, 0, 0));
    if (static_cast<LONG>(GetTickCount() - deadline) >= 0) {
        InterlockedExchange(&g_message, kHidden);
        return;
    }

    const int viewportWidth = viewport.right - viewport.left;
    const int viewportHeight = viewport.bottom - viewport.top;
    const int panelWidth = std::min(std::max(1, viewportWidth - 16),
                                    std::max(260, MulDiv(viewportWidth, 500, 1024)));
    const int panelHeight = std::min(std::max(1, viewportHeight - 16),
                                     std::max(64, MulDiv(viewportHeight, 100, 768)));
    const int left = viewport.left + (viewportWidth - panelWidth) / 2;
    const int top = viewport.top + (viewportHeight - panelHeight) / 2;
    RECT panel{left, top, left + panelWidth, top + panelHeight};

    const int savedDc = SaveDC(destination);
    const HBRUSH shadow = CreateSolidBrush(RGB(45, 36, 24));
    RECT shadowRect = panel;
    OffsetRect(&shadowRect, 5, 5);
    FillRect(destination, &shadowRect, shadow);
    DeleteObject(shadow);

    const HBRUSH background = CreateSolidBrush(RGB(225, 207, 159));
    const HPEN outerPen = CreatePen(PS_SOLID, 3, RGB(55, 45, 30));
    const HGDIOBJ oldBrush = SelectObject(destination, background);
    const HGDIOBJ oldPen = SelectObject(destination, outerPen);
    RoundRect(destination, panel.left, panel.top, panel.right, panel.bottom, 12, 12);

    RECT inner = panel;
    InflateRect(&inner, -6, -6);
    const HPEN innerPen = CreatePen(PS_SOLID, 1, RGB(132, 101, 57));
    SelectObject(destination, GetStockObject(NULL_BRUSH));
    SelectObject(destination, innerPen);
    RoundRect(destination, inner.left, inner.top, inner.right, inner.bottom, 8, 8);

    const int fontHeight = std::max(24, MulDiv(viewportHeight, 30, 768));
    const HFONT font = GetFont(fontHeight);
    if (font) {
        SelectObject(destination, font);
    }
    SetBkMode(destination, TRANSPARENT);
    SetTextColor(destination, RGB(28, 24, 18));
    RECT textRect = panel;
    InflateRect(&textRect, -16, -10);
    const wchar_t* text = message == kCursorLocked
                              ? L"鼠标已锁定在游戏画面内"
                              : L"鼠标锁定已解除";
    DrawTextW(destination, text, -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(destination, oldPen);
    SelectObject(destination, oldBrush);
    DeleteObject(innerPen);
    DeleteObject(outerPen);
    DeleteObject(background);
    RestoreDC(destination, savedDc);
}

void Shutdown() {
    InterlockedExchange(&g_message, kHidden);
    if (g_font) {
        DeleteObject(g_font);
        g_font = nullptr;
        g_fontHeight = 0;
    }
}

} // namespace san9::status_overlay
