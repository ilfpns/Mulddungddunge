#pragma once
#include "pch.h"

// Moving another app's window to a different virtual desktop. The documented IVirtualDesktopManager
// only moves our own windows; this goes through Explorer's internal interfaces, whose ids change
// between Windows releases. Only the layout of Windows 11 24H2/25H2 is known here: elsewhere
// Available() is false and callers offer nothing.
namespace vd
{
    bool Available();
    std::vector<GUID> Desktops();                   // in Task View order
    // The desktop being shown, asked of Explorer (the registry copy of it can lag behind). False where
    // the interface isn't known; the connection is kept, and made again after Explorer restarts.
    bool Current(GUID* id);
    bool MoveWindow(HWND hwnd, GUID const& desktop);
}
