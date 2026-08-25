#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace san9::window_placement {

struct State {
    std::wstring monitor;
    int xDip{};
    int yDip{};
    int clientWidthDip{};
    bool maximized{};
};

struct MaximumGeometry {
    RECT outer{};
    int clientWidth{};
    int clientHeight{};
};

std::wstring Serialize(const State& state);
bool TryParse(std::wstring_view text, State& state);
bool CalculateMaximumFourByThree(const RECT& workArea, int frameWidth,
                                 int frameHeight, MaximumGeometry& result);

bool Restore(HWND window, const std::filesystem::path& configPath,
             int defaultClientWidth, int defaultClientHeight,
             bool borderlessFullscreen, bool& shouldMaximize);
void StartTracking(HWND window);
void HandleWindowMessageBefore(HWND window, UINT message);
void HandleWindowMessageAfter(HWND window, UINT message, WPARAM wParam);
bool HandleGetMinMaxInfo(HWND window, MINMAXINFO& limits);
void Shutdown();

} // namespace san9::window_placement
