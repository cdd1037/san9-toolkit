#include "window_placement.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cwchar>
#include <vector>

namespace san9::window_placement {
namespace {

constexpr wchar_t kSection[] = L"WindowState";
constexpr wchar_t kPlacementKey[] = L"Placement";
constexpr wchar_t kMissing[] = L"{missing}";
constexpr int kVersion = 1;
constexpr int kDefaultDpi = 96;
constexpr int kMinimumClientWidthDip = 1;
constexpr int kMaximumClientWidthDip = 16384;
constexpr int kMaximumCoordinateDip = 100000;

std::filesystem::path g_configPath;
HWND g_window = nullptr;
State g_state;
bool g_hasState = false;
bool g_tracking = false;
bool g_persistPlacement = false;

bool ParseInt(std::wstring_view text, int minimum, int maximum, int& result) {
    if (text.empty()) {
        return false;
    }
    const std::wstring owned(text);
    wchar_t* end = nullptr;
    errno = 0;
    const long value = std::wcstol(owned.c_str(), &end, 10);
    if (errno == ERANGE || end == owned.c_str() || *end != L'\0' ||
        value < minimum || value > maximum) {
        return false;
    }
    result = static_cast<int>(value);
    return true;
}

std::vector<std::wstring_view> Split(std::wstring_view text) {
    std::vector<std::wstring_view> fields;
    std::size_t start = 0;
    for (;;) {
        const std::size_t separator = text.find(L'|', start);
        fields.push_back(text.substr(start, separator - start));
        if (separator == std::wstring_view::npos) {
            return fields;
        }
        start = separator + 1;
    }
}

UINT WindowDpi(HWND window) {
    const UINT dpi = GetDpiForWindow(window);
    return dpi == 0 ? kDefaultDpi : dpi;
}

int ToDip(int pixels, UINT dpi) {
    return MulDiv(pixels, kDefaultDpi, static_cast<int>(dpi));
}

int ToPixels(int dips, UINT dpi) {
    return MulDiv(dips, static_cast<int>(dpi), kDefaultDpi);
}

bool GetMonitorDetails(HMONITOR monitor, MONITORINFOEXW& details) {
    details = {};
    details.cbSize = sizeof(details);
    return monitor && GetMonitorInfoW(monitor, &details) != FALSE;
}

struct MonitorSearch {
    std::wstring_view name;
    HMONITOR result{};
};

BOOL CALLBACK FindMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto& search = *reinterpret_cast<MonitorSearch*>(parameter);
    MONITORINFOEXW details{};
    if (GetMonitorDetails(monitor, details) && search.name == details.szDevice) {
        search.result = monitor;
        return FALSE;
    }
    return TRUE;
}

HMONITOR ResolveMonitor(HWND window, std::wstring_view name) {
    MonitorSearch search{name};
    if (!name.empty()) {
        EnumDisplayMonitors(nullptr, nullptr, FindMonitor,
                            reinterpret_cast<LPARAM>(&search));
    }
    return search.result ? search.result
                         : MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
}

bool ReadState(const std::filesystem::path& path, State& state) {
    std::array<wchar_t, 1024> buffer{};
    GetPrivateProfileStringW(kSection, kPlacementKey, kMissing, buffer.data(),
                             static_cast<DWORD>(buffer.size()), path.c_str());
    return std::wcscmp(buffer.data(), kMissing) != 0 && TryParse(buffer.data(), state);
}

bool WriteState() {
    if (!g_persistPlacement || !g_hasState || g_configPath.empty()) {
        return false;
    }
    const std::wstring serialized = Serialize(g_state);
    return !serialized.empty() &&
           WritePrivateProfileStringW(kSection, kPlacementKey, serialized.c_str(),
                                      g_configPath.c_str()) != FALSE;
}

bool GetFrameSize(LONG_PTR style, LONG_PTR exStyle, UINT dpi,
                  int& frameWidth, int& frameHeight) {
    RECT frame{0, 0, 0, 0};
    if (!AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(style), FALSE,
                                  static_cast<DWORD>(exStyle), dpi)) {
        return false;
    }
    frameWidth = frame.right - frame.left;
    frameHeight = frame.bottom - frame.top;
    return frameWidth >= 0 && frameHeight >= 0;
}

bool CaptureNormal(HWND window) {
    if (!window || IsIconic(window) || IsZoomed(window)) {
        return false;
    }
    RECT outer{};
    RECT client{};
    if (!GetWindowRect(window, &outer) || !GetClientRect(window, &client)) {
        return false;
    }
    const int clientWidth = client.right - client.left;
    if (clientWidth <= 0) {
        return false;
    }
    MONITORINFOEXW monitor{};
    if (!GetMonitorDetails(MonitorFromRect(&outer, MONITOR_DEFAULTTONEAREST), monitor)) {
        return false;
    }
    const UINT dpi = WindowDpi(window);
    g_state.monitor = monitor.szDevice;
    g_state.xDip = ToDip(outer.left - monitor.rcWork.left, dpi);
    g_state.yDip = ToDip(outer.top - monitor.rcWork.top, dpi);
    g_state.clientWidthDip = std::clamp(ToDip(clientWidth, dpi),
                                        kMinimumClientWidthDip,
                                        kMaximumClientWidthDip);
    g_state.maximized = false;
    g_hasState = true;
    return true;
}

bool CalculateOuterRect(HWND window, LONG_PTR style, LONG_PTR exStyle,
                        HMONITOR target, const State* saved,
                        int defaultClientWidth, int defaultClientHeight,
                        RECT& result) {
    MONITORINFOEXW monitor{};
    if (!GetMonitorDetails(target, monitor)) {
        return false;
    }
    const UINT dpi = WindowDpi(window);
    int clientWidth = saved ? ToPixels(saved->clientWidthDip, dpi) : defaultClientWidth;
    int clientHeight = saved ? MulDiv(clientWidth, 3, 4) : defaultClientHeight;

    int frameWidth = 0;
    int frameHeight = 0;
    if (!GetFrameSize(style, exStyle, dpi, frameWidth, frameHeight)) {
        return false;
    }
    const int workWidth = monitor.rcWork.right - monitor.rcWork.left;
    const int workHeight = monitor.rcWork.bottom - monitor.rcWork.top;
    const int maximumClientWidth = std::max(1, workWidth - frameWidth);
    const int maximumClientHeight = std::max(1, workHeight - frameHeight);
    if (clientWidth > maximumClientWidth || clientHeight > maximumClientHeight) {
        clientWidth = std::min(maximumClientWidth,
                               MulDiv(maximumClientHeight, 4, 3));
        clientHeight = MulDiv(clientWidth, 3, 4);
    }

    RECT adjusted{0, 0, clientWidth, clientHeight};
    if (!AdjustWindowRectExForDpi(&adjusted, static_cast<DWORD>(style), FALSE,
                                  static_cast<DWORD>(exStyle), dpi)) {
        return false;
    }
    const int outerWidth = adjusted.right - adjusted.left;
    const int outerHeight = adjusted.bottom - adjusted.top;

    RECT current{};
    if (!GetWindowRect(window, &current)) {
        return false;
    }
    int left = saved ? monitor.rcWork.left + ToPixels(saved->xDip, dpi) : current.left;
    int top = saved ? monitor.rcWork.top + ToPixels(saved->yDip, dpi) : current.top;
    const int workLeft = monitor.rcWork.left;
    const int workTop = monitor.rcWork.top;
    const int maximumLeft = std::max(workLeft,
                                     static_cast<int>(monitor.rcWork.right) - outerWidth);
    const int maximumTop = std::max(workTop,
                                    static_cast<int>(monitor.rcWork.bottom) - outerHeight);
    left = std::clamp(left, workLeft, maximumLeft);
    top = std::clamp(top, workTop, maximumTop);
    result = {left, top, left + outerWidth, top + outerHeight};
    return true;
}

bool CalculateMaximumForMonitor(HWND window, HMONITOR target,
                                MaximumGeometry& geometry,
                                MONITORINFOEXW* monitorDetails = nullptr) {
    MONITORINFOEXW monitor{};
    if (!GetMonitorDetails(target, monitor)) {
        return false;
    }
    int frameWidth = 0;
    int frameHeight = 0;
    if (!GetFrameSize(GetWindowLongPtrW(window, GWL_STYLE),
                      GetWindowLongPtrW(window, GWL_EXSTYLE), WindowDpi(window),
                      frameWidth, frameHeight) ||
        !CalculateMaximumFourByThree(monitor.rcWork, frameWidth, frameHeight,
                                     geometry)) {
        return false;
    }
    if (monitorDetails) {
        *monitorDetails = monitor;
    }
    return true;
}

} // namespace

std::wstring Serialize(const State& state) {
    if (state.monitor.empty() || state.monitor.find(L'|') != std::wstring::npos ||
        state.xDip < -kMaximumCoordinateDip || state.xDip > kMaximumCoordinateDip ||
        state.yDip < -kMaximumCoordinateDip || state.yDip > kMaximumCoordinateDip ||
        state.clientWidthDip < kMinimumClientWidthDip ||
        state.clientWidthDip > kMaximumClientWidthDip) {
        return {};
    }
    return std::to_wstring(kVersion) + L"|" + state.monitor + L"|" +
           std::to_wstring(state.xDip) + L"|" + std::to_wstring(state.yDip) + L"|" +
           std::to_wstring(state.clientWidthDip) + L"|" +
           std::to_wstring(state.maximized ? 1 : 0);
}

bool TryParse(std::wstring_view text, State& state) {
    const std::vector<std::wstring_view> fields = Split(text);
    int version = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int maximized = 0;
    if (fields.size() != 6 || fields[1].empty() ||
        fields[1].find(L'|') != std::wstring_view::npos ||
        !ParseInt(fields[0], kVersion, kVersion, version) ||
        !ParseInt(fields[2], -kMaximumCoordinateDip, kMaximumCoordinateDip, x) ||
        !ParseInt(fields[3], -kMaximumCoordinateDip, kMaximumCoordinateDip, y) ||
        !ParseInt(fields[4], kMinimumClientWidthDip, kMaximumClientWidthDip, width) ||
        !ParseInt(fields[5], 0, 1, maximized)) {
        return false;
    }
    state = {std::wstring(fields[1]), x, y, width, maximized != 0};
    return true;
}

bool CalculateMaximumFourByThree(const RECT& workArea, int frameWidth,
                                 int frameHeight, MaximumGeometry& result) {
    const int workWidth = workArea.right - workArea.left;
    const int workHeight = workArea.bottom - workArea.top;
    const int availableClientWidth = workWidth - frameWidth;
    const int availableClientHeight = workHeight - frameHeight;
    if (workWidth <= 0 || workHeight <= 0 || frameWidth < 0 || frameHeight < 0 ||
        availableClientWidth < 4 || availableClientHeight < 3) {
        return false;
    }
    const int widthLimited = availableClientWidth / 4 * 4;
    const int heightLimited = availableClientHeight / 3 * 4;
    const int clientWidth = std::min(widthLimited, heightLimited);
    const int clientHeight = clientWidth / 4 * 3;
    const int outerWidth = clientWidth + frameWidth;
    const int outerHeight = clientHeight + frameHeight;
    const int left = workArea.left + (workWidth - outerWidth) / 2;
    const int top = workArea.bottom - outerHeight;
    result = {{left, top, left + outerWidth, top + outerHeight},
              clientWidth, clientHeight};
    return true;
}

bool Restore(HWND window, const std::filesystem::path& configPath,
             int defaultClientWidth, int defaultClientHeight,
             bool borderlessFullscreen, bool& shouldMaximize) {
    if (!window || configPath.empty() || defaultClientWidth <= 0 ||
        defaultClientHeight <= 0) {
        return false;
    }
    State saved{};
    const bool hasSaved = ReadState(configPath, saved);
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const HMONITOR monitor = ResolveMonitor(window, hasSaved ? saved.monitor : L"");
    RECT target{};
    if (borderlessFullscreen) {
        MaximumGeometry geometry{};
        if (!CalculateMaximumForMonitor(window, monitor, geometry)) {
            return false;
        }
        target = geometry.outer;
    } else if (!CalculateOuterRect(window, style, exStyle, monitor,
                                   hasSaved ? &saved : nullptr,
                                   defaultClientWidth, defaultClientHeight, target)) {
        return false;
    }
    if (!SetWindowPos(window, nullptr, target.left, target.top,
                      target.right - target.left, target.bottom - target.top,
                      SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED)) {
        return false;
    }

    g_configPath = configPath;
    g_window = window;
    g_hasState = false;
    g_persistPlacement = !borderlessFullscreen;
    if (g_persistPlacement && !CaptureNormal(window)) {
        return false;
    }
    shouldMaximize = g_persistPlacement && hasSaved && saved.maximized;
    if (g_persistPlacement) {
        g_state.maximized = shouldMaximize;
    }
    return true;
}

void StartTracking(HWND window) {
    g_tracking = window && window == g_window;
}

bool HandleGetMinMaxInfo(HWND window, MINMAXINFO& limits) {
    if (!g_tracking || !g_persistPlacement || window != g_window) {
        return false;
    }
    MONITORINFOEXW monitor{};
    MaximumGeometry geometry{};
    if (!CalculateMaximumForMonitor(
            window, MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), geometry,
            &monitor)) {
        return false;
    }
    limits.ptMaxPosition.x = geometry.outer.left - monitor.rcMonitor.left;
    limits.ptMaxPosition.y = geometry.outer.top - monitor.rcMonitor.top;
    limits.ptMaxSize.x = geometry.outer.right - geometry.outer.left;
    limits.ptMaxSize.y = geometry.outer.bottom - geometry.outer.top;
    return true;
}

void HandleWindowMessageBefore(HWND window, UINT message) {
    if (!g_tracking || window != g_window || message != WM_CLOSE) {
        return;
    }
    CaptureNormal(window);
    WriteState();
}

void HandleWindowMessageAfter(HWND window, UINT message, WPARAM wParam) {
    if (!g_tracking || window != g_window) {
        return;
    }
    if (message == WM_SIZE) {
        if (wParam == SIZE_MAXIMIZED) {
            g_state.maximized = true;
        } else if (wParam == SIZE_RESTORED && CaptureNormal(window)) {
            g_state.maximized = false;
        }
    } else if (message == WM_MOVE || message == WM_WINDOWPOSCHANGED ||
               message == WM_DPICHANGED) {
        CaptureNormal(window);
    } else if (message == WM_EXITSIZEMOVE) {
        CaptureNormal(window);
        WriteState();
    } else if (message == WM_NCDESTROY) {
        WriteState();
        g_tracking = false;
        g_window = nullptr;
    }
}

void Shutdown() {
    g_tracking = false;
    g_window = nullptr;
    g_hasState = false;
    g_persistPlacement = false;
    g_configPath.clear();
}

} // namespace san9::window_placement
