#pragma once
#include "pch.h"

// Decides which top-level windows the stage manages and where they are.
namespace wt
{
    bool IsManageable(HWND hwnd, HMONITOR monitor);
    bool IsFullscreen(HWND hwnd);
    std::vector<HWND> EnumManageable(HMONITOR monitor);   // top of z-order first
    RECT FrameRect(HWND hwnd);                              // visible frame, without invisible resize borders
    RECT RestoreRect(HWND hwnd, bool* maximized = nullptr); // window rect a minimized window comes back to
}
