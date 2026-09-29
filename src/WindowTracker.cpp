#include "WindowTracker.h"

namespace wt
{
    bool IsCloaked(HWND hwnd)
    {
        DWORD cloaked = 0;
        return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
    }

    static bool IsShellClass(HWND hwnd)
    {
        wchar_t cls[64]{};
        GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
        static const wchar_t* blocked[] = {
            L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
            L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow", L"TopLevelWindowForOverflowXamlIsland",
        };
        for (auto b : blocked)
            if (wcscmp(cls, b) == 0)
                return true;
        return false;
    }

    bool IsManageable(HWND hwnd, HMONITOR monitor)
    {
        if (!hwnd || !IsWindowVisible(hwnd) || IsCloaked(hwnd))
            return false;
        if (GetWindow(hwnd, GW_OWNER))
            return false;
        LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        // Always-on-top windows (desktop pets, PiP players, overlays) stay where they are.
        if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST))
            return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId())
            return false;
        if (GetWindowTextLengthW(hwnd) == 0 || IsShellClass(hwnd))
            return false;
        if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != monitor)
            return false;
        return !IsFullscreen(hwnd);
    }

    bool IsFullscreen(HWND hwnd)
    {
        // A maximized borderless app (Discord, Spotify, VS Code...) also covers the whole monitor when the
        // taskbar auto-hides; it is maximized, not fullscreen.
        if (!hwnd || IsIconic(hwnd) || IsZoomed(hwnd) || (GetWindowLongW(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION)
            return false;
        RECT r;
        GetWindowRect(hwnd, &r);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
               r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
    }

    HWND TopWindow(HMONITOR monitor)
    {
        DWORD self = GetCurrentProcessId();
        for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
        {
            if (!IsWindowVisible(h) || IsIconic(h) || IsCloaked(h))
                continue;
            LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
            if (ex & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
                continue;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == self || MonitorFromWindow(h, MONITOR_DEFAULTTONULL) != monitor)
                continue;
            return h;
        }
        return nullptr;
    }

    std::vector<HWND> EnumManageable(HMONITOR monitor)
    {
        struct Ctx { HMONITOR mon; std::vector<HWND> out; } ctx{ monitor, {} };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto c = reinterpret_cast<Ctx*>(lp);
            if (IsManageable(h, c->mon))
                c->out.push_back(h);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.out;
    }

    RECT FrameRect(HWND hwnd)
    {
        RECT r{};
        if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
            GetWindowRect(hwnd, &r);
        return r;
    }

    std::wstring ProcessPath(HWND hwnd)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!proc)
            return {};
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        std::wstring out;
        if (QueryFullProcessImageNameW(proc, 0, path, &len))
            out.assign(path, len);
        CloseHandle(proc);
        return out;
    }

    RECT RestoreRect(HWND hwnd, bool* maximized)
    {
        WINDOWPLACEMENT wp{ sizeof(wp) };
        GetWindowPlacement(hwnd, &wp);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        bool max = (wp.flags & WPF_RESTORETOMAXIMIZED) != 0;
        if (maximized)
            *maximized = max;
        if (max)
            return mi.rcWork;
        // rcNormalPosition is in workspace coordinates.
        RECT r = wp.rcNormalPosition;
        OffsetRect(&r, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
        return r;
    }
}
