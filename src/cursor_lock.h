#pragma once

#include <windows.h>

namespace san9::cursor_lock {

void Initialize(HWND window, UINT toggleKey);
void Suspend();
void Resume();
bool HandleInputMessage(HWND window, MSG& message);
bool HandleWindowMessageBefore(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
void HandleWindowMessageAfter(HWND window, UINT message);
void Shutdown();

} // namespace san9::cursor_lock
