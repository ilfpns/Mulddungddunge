#pragma once
#include "pch.h"

// Reserves a strip on the left edge of a monitor so maximized windows don't cover the sidebar.
namespace appbar
{
    void Register(HWND hwnd, UINT callbackMessage);
    // Returns the rect the system granted for a bar of `width` px on the left of `monitor`.
    RECT Dock(HWND hwnd, RECT const& monitor, int width);
    void Remove(HWND hwnd);
}
