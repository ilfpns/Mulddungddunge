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

    static std::vector<std::wstring> g_excluded;

    void SetExcluded(std::vector<std::wstring> const& apps)
    {
        g_excluded = apps;
    }

    bool IsManageable(HWND hwnd, HMONITOR monitor, bool otherDesktops)
    {
        if (!IsAppWindow(hwnd, monitor, otherDesktops))
            return false;
        if (g_excluded.empty())
            return true;
        auto app = AppId(hwnd);
        return std::none_of(g_excluded.begin(), g_excluded.end(), [&](auto& e) { return _wcsicmp(e.c_str(), app.c_str()) == 0; });
    }

    bool IsAppWindow(HWND hwnd, HMONITOR monitor, bool otherDesktops)
    {
        if (!hwnd || !IsWindowVisible(hwnd))
            return false;
        // Cloaked by the shell = on another virtual desktop; any other cloaking means not shown at all.
        DWORD cloaked = 0;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked && !(otherDesktops && cloaked == DWM_CLOAKED_SHELL))
            return false;
        if (GetWindow(hwnd, GW_OWNER) || ModalOwner(hwnd))
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
        if (monitor && MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != monitor)
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
            // Shell surfaces (the emoji panel, Start, search, the desktop itself) cover the whole screen
            // without being fullscreen apps; taking them for one hid the sidebar until the next focus change.
            if (IsShellClass(h))
                continue;
            return h;
        }
        return nullptr;
    }

    std::vector<HWND> EnumManageable(HMONITOR monitor, bool otherDesktops)
    {
        struct Ctx { HMONITOR mon; bool other; std::vector<HWND> out; } ctx{ monitor, otherDesktops, {} };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto c = reinterpret_cast<Ctx*>(lp);
            if (IsManageable(h, c->mon, c->other))
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

    static bool IsDialog(HWND hwnd)
    {
        wchar_t cls[16]{};
        GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
        return wcscmp(cls, L"#32770") == 0;
    }

    HWND ModalOwner(HWND dialog)
    {
        if (!dialog || !IsDialog(dialog))
            return nullptr;
        HWND root = GetAncestor(dialog, GA_ROOTOWNER);
        if (root && root != dialog)
            return root;
        // Unowned (KakaoTalk's file picker): the window it blocks is a disabled window of the same
        // process, preferably on the same thread, since a modal loop runs on the blocked window's thread.
        struct Ctx { HWND self; DWORD pid, tid; HWND sameThread, sameProcess; } ctx{ dialog, 0, 0, nullptr, nullptr };
        ctx.tid = GetWindowThreadProcessId(dialog, &ctx.pid);
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto c = reinterpret_cast<Ctx*>(lp);
            if (h == c->self || IsWindowEnabled(h) || !IsWindowVisible(h) || GetWindow(h, GW_OWNER) ||
                GetWindowTextLengthW(h) == 0 || IsDialog(h))
                return TRUE;
            DWORD pid = 0;
            DWORD tid = GetWindowThreadProcessId(h, &pid);
            if (pid != c->pid)
                return TRUE;
            if (tid == c->tid)
            {
                c->sameThread = h;
                return FALSE;
            }
            if (!c->sameProcess)
                c->sameProcess = h;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.sameThread ? ctx.sameThread : ctx.sameProcess;
    }

    HWND ModalDialog(HWND owner)
    {
        if (!owner || !IsWindow(owner) || IsWindowEnabled(owner))
            return nullptr;
        HWND popup = GetLastActivePopup(owner);
        if (popup && popup != owner && IsWindowEnabled(popup))
            return popup;
        struct Ctx { HWND owner; DWORD pid; HWND found; } ctx{ owner, 0, nullptr };
        GetWindowThreadProcessId(owner, &ctx.pid);
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == c->pid && h != c->owner && IsWindowVisible(h) && IsWindowEnabled(h) && IsDialog(h) &&
                ModalOwner(h) == c->owner)
            {
                c->found = h;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.found;
    }

    // A browser web app window carries its own AppUserModelID, e.g. "Chrome._crx_<id>".
    static std::wstring WebAppModelId(HWND hwnd)
    {
        winrt::com_ptr<IPropertyStore> store;
        if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(store.put()))))
            return {};
        PROPVARIANT v;
        PropVariantInit(&v);
        std::wstring id;
        if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
            id = v.pwszVal;
        PropVariantClear(&v);
        return id.find(L"_crx_") != std::wstring::npos ? id : std::wstring{};
    }

    std::wstring AppId(HWND hwnd)
    {
        auto path = ProcessPath(hwnd);
        auto web = WebAppModelId(hwnd);
        return web.empty() || path.empty() ? path : path + L"#" + web;
    }

    std::wstring WebAppIcon(HWND hwnd)
    {
        auto id = WebAppModelId(hwnd);
        if (id.empty())
            return {};
        // "Chrome._crx_<app id>[.<profile>]"; a long app id is shortened to its head and tail.
        auto part = id.substr(id.find(L"_crx_") + 5);
        part = part.substr(0, part.find(L'.'));
        if (part.size() < 8)
            return {};
        auto head = part.substr(0, 6), tail = part.substr(part.size() - 6);

        wchar_t local[MAX_PATH];
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
            return {};
        auto exe = ProcessPath(hwnd);
        bool edge = exe.size() >= 10 && _wcsicmp(exe.c_str() + exe.size() - 10, L"msedge.exe") == 0;
        std::wstring userData = std::wstring(local) + (edge ? L"\\Microsoft\\Edge\\User Data\\" : L"\\Google\\Chrome\\User Data\\");

        // <profile>\Web Applications\Manifest Resources\<app id>\Icons\<size>.png, in any profile.
        WIN32_FIND_DATAW profile;
        HANDLE profiles = FindFirstFileW((userData + L"*").c_str(), &profile);
        if (profiles == INVALID_HANDLE_VALUE)
            return {};
        std::wstring found;
        do
        {
            if (!(profile.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || profile.cFileName[0] == L'.')
                continue;
            auto base = userData + profile.cFileName + L"\\Web Applications\\Manifest Resources\\";
            WIN32_FIND_DATAW app;
            HANDLE apps = FindFirstFileW((base + head + L"*").c_str(), &app);
            if (apps == INVALID_HANDLE_VALUE)
                continue;
            do
            {
                std::wstring name = app.cFileName;
                if (name.size() < tail.size() || _wcsicmp(name.c_str() + name.size() - tail.size(), tail.c_str()) != 0)
                    continue;
                for (auto size : { L"128", L"144", L"192", L"256", L"96", L"64" })   // 128 is plenty, and small
                {
                    auto file = base + name + L"\\Icons\\" + size + L".png";
                    if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES)
                    {
                        found = file;
                        break;
                    }
                }
            } while (found.empty() && FindNextFileW(apps, &app));
            FindClose(apps);
        } while (found.empty() && FindNextFileW(profiles, &profile));
        FindClose(profiles);
        return found;
    }

    std::wstring AppLabel(std::wstring const& app, HWND sample)
    {
        auto hash = app.find(L'#');
        std::wstring exe = app.substr(0, hash);
        if (hash != std::wstring::npos && sample)
        {
            // A web app's windows are titled "<app> - <page>" or just "<app>".
            wchar_t title[128]{};
            GetWindowTextW(sample, title, ARRAYSIZE(title));
            std::wstring t = title;
            auto dash = t.find(L" - ");
            if (dash != std::wstring::npos)
                t.resize(dash);
            if (!t.empty())
                return t;
        }
        DWORD handle = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
        if (size)
        {
            std::vector<BYTE> data(size);
            struct Lang { WORD lang, page; }* langs = nullptr;
            UINT len = 0;
            if (GetFileVersionInfoW(exe.c_str(), 0, size, data.data()) &&
                VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) && len >= sizeof(Lang))
            {
                wchar_t key[64];
                swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].page);
                wchar_t* text = nullptr;
                if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&text), &len) && text && *text)
                    return hash != std::wstring::npos ? std::wstring(text) + L" 웹앱" : std::wstring(text);
            }
        }
        auto slash = exe.find_last_of(L"\\");
        auto name = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
        if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0)
            name.resize(name.size() - 4);
        return name;
    }
}
