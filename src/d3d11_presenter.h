#pragma once

#include <windows.h>

namespace san9::d3d11_presenter {

bool Initialize(HWND window);
bool RequestPresent(HDC framebufferDc);
bool HandleWindowMessage(MSG& message);
void Shutdown();

} // namespace san9::d3d11_presenter
