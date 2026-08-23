#pragma once

#include <windows.h>

namespace san9::movie_player {

bool Install();
bool HandleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
void Shutdown();

} // namespace san9::movie_player
