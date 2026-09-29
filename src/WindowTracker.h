#pragma once
#include "pch.h"

// Decides which top-level windows the stage manages and where they are.
namespace wt
{
    bool IsManageable(HWND hwnd, HMONITOR monitor, bool otherDesktops = false);
    bool IsFullscreen(HWND hwnd);
    bool IsCloaked(HWND hwnd);                              // hidden by DWM, e.g. on another virtual desktop
    // The top-most ordinary window on the monitor: skips always-on-top windows (pets, PiP players),
    // tool windows and our own, which may hold focus without being what the user is looking at.
    HWND TopWindow(HMONITOR monitor);
    std::vector<HWND> EnumManageable(HMONITOR monitor, bool otherDesktops = false);   // top of z-order first
    RECT FrameRect(HWND hwnd);                              // visible frame, without invisible resize borders
    RECT RestoreRect(HWND hwnd, bool* maximized = nullptr); // window rect a minimized window comes back to
    std::wstring ProcessPath(HWND hwnd);                    // full path of the window's executable
}
