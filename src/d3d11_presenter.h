#pragma once

#include <windows.h>

namespace san9::d3d11_presenter {

bool Initialize(HWND window);
bool QueueFrame(HDC framebufferDc);
bool PresentFrame(HDC framebufferDc);
bool PresentPendingFrame();
void Shutdown();

} // namespace san9::d3d11_presenter
