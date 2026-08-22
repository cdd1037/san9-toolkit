#pragma once

#include <windows.h>

namespace san9::status_overlay {

void ShowCursorLockState(HWND owner, bool enabled);
void UpdatePosition(HWND owner);
void Hide();
void Shutdown();

} // namespace san9::status_overlay
