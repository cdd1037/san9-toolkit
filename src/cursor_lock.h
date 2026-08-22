#pragma once

#include <windows.h>

namespace san9::cursor_lock {

void Initialize(HWND window);
bool HandleWindowMessageBefore(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
void HandleWindowMessageAfter(HWND window, UINT message);
void Shutdown();

} // namespace san9::cursor_lock
