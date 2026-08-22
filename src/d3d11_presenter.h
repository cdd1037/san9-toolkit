#pragma once

#include <windows.h>

namespace san9::d3d11_presenter {

bool Initialize(HWND window);
bool Present(HDC framebufferDc);
void Shutdown();

} // namespace san9::d3d11_presenter
