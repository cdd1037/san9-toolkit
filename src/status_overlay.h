#pragma once

#include <windows.h>

namespace san9::status_overlay {

void ShowCursorLockState(HWND owner, bool enabled);
void ShowMessage(HWND owner, const wchar_t* message);
void UpdatePosition(HWND owner);
void Hide();
void Shutdown();

} // namespace san9::status_overlay
