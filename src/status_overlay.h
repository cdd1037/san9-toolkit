#pragma once

#include <windows.h>

namespace san9::status_overlay {

void ShowCursorLockState(bool enabled);
void Draw(HDC destination, const RECT& viewport);
void Shutdown();

} // namespace san9::status_overlay
