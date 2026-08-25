#pragma once

#include <windows.h>

#include <filesystem>

namespace san9::movie_player {

bool Install(const std::filesystem::path& configPath);
bool HandleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
void Shutdown();

} // namespace san9::movie_player
