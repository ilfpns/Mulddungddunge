#include "AppBar.h"

namespace appbar
{
    static APPBARDATA Data(HWND hwnd)
    {
        APPBARDATA abd{ sizeof(abd) };
        abd.hWnd = hwnd;
        return abd;
    }

    void Register(HWND hwnd, UINT callbackMessage)
    {
        auto abd = Data(hwnd);
        abd.uCallbackMessage = callbackMessage;
        SHAppBarMessage(ABM_NEW, &abd);
    }

    RECT Dock(HWND hwnd, RECT const& monitor, int width)
    {
        auto abd = Data(hwnd);
        abd.uEdge = ABE_LEFT;
        abd.rc = monitor;
        abd.rc.right = abd.rc.left + width;
        SHAppBarMessage(ABM_QUERYPOS, &abd);
        abd.rc.right = abd.rc.left + width;
        SHAppBarMessage(ABM_SETPOS, &abd);
        return abd.rc;
    }

    void Remove(HWND hwnd)
    {
        auto abd = Data(hwnd);
        SHAppBarMessage(ABM_REMOVE, &abd);
    }
}
