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
        return std::none_of(g_excluded.begin(), g_excluded.end(), [&](auto& e) { return SameApp(e, app); });
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

    static std::wstring ImagePath(HANDLE proc)
    {
        std::wstring buf(32768, L'\0');             // long paths, not just MAX_PATH
        DWORD len = static_cast<DWORD>(buf.size());
        if (!QueryFullProcessImageNameW(proc, 0, buf.data(), &len))
            return {};
        buf.resize(len);
        return buf;
    }

    static std::wstring PathOf(DWORD pid)
    {
        HANDLE proc = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
        if (!proc)
            return {};
        auto out = ImagePath(proc);
        CloseHandle(proc);
        return out;
    }

    static DWORD HostedApp(HWND frame, DWORD host);

    static bool IsFrameHost(std::wstring const& path)
    {
        return path.size() >= 24 && _wcsicmp(path.c_str() + path.size() - 24, L"ApplicationFrameHost.exe") == 0;
    }

    std::wstring ProcessPath(HWND hwnd)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        return PathOf(pid);
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

    // The AppUserModelID a window carries, if any: browser web apps ("Chrome._crx_<id>") and UWP
    // frames (the app's own id, e.g. "Microsoft.WindowsCalculator_8wekyb3d8bbwe!App").
    static std::wstring WindowModelId(HWND hwnd)
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
        return id;
    }

    static std::wstring WebAppModelId(HWND hwnd)
    {
        auto id = WindowModelId(hwnd);
        return id.find(L"_crx_") != std::wstring::npos ? id : std::wstring{};
    }

    std::wstring AppId(HWND hwnd)
    {
        auto path = ProcessPath(hwnd);
        // Every UWP window belongs to ApplicationFrameHost. The frame carries the app's model id, also
        // while minimized, when the app's own window has left the frame.
        if (IsFrameHost(path))
        {
            auto id = WindowModelId(hwnd);
            return id.empty() ? path : L"uwp:" + id;
        }
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
        if (app.rfind(L"uwp:", 0) == 0)
        {
            // Its window title is its name ("Calculator"); without one, the package name.
            wchar_t title[128]{};
            if (sample)
                GetWindowTextW(sample, title, ARRAYSIZE(title));
            if (*title)
                return title;
            auto name = app.substr(4, app.find(L'_') == std::wstring::npos ? std::wstring::npos : app.find(L'_') - 4);
            auto dot = name.find_last_of(L'.');
            return dot == std::wstring::npos ? name : name.substr(dot + 1);
        }
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

    // A UWP frame (ApplicationFrameHost) hosts the app's own window as a child; its process is the app.
    static DWORD HostedApp(HWND frame, DWORD host)
    {
        struct Ctx { DWORD host, app; } ctx{ host, 0 };
        EnumChildWindows(frame, [](HWND h, LPARAM lp) -> BOOL {
            auto c = reinterpret_cast<Ctx*>(lp);
            DWORD p = 0;
            GetWindowThreadProcessId(h, &p);
            if (p != c->host)
                c->app = p;
            return c->app == 0;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.app;
    }

    // The running process of a packaged app, found by its model id.
    static DWORD ProcessOfModel(std::wstring const& model)
    {
        DWORD found = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return 0;
        PROCESSENTRY32W e{ sizeof(e) };
        for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
        {
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
            if (!proc)
                continue;
            wchar_t id[APPLICATION_USER_MODEL_ID_MAX_LENGTH]{};
            UINT32 len = ARRAYSIZE(id);
            if (GetApplicationUserModelId(proc, &len, id) == ERROR_SUCCESS && _wcsicmp(id, model.c_str()) == 0 &&
                !IsFrameHost(ImagePath(proc)))
                found = e.th32ProcessID;
            CloseHandle(proc);
        }
        CloseHandle(snap);
        return found;
    }

    DWORD AppProcess(HWND hwnd)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (IsFrameHost(PathOf(pid)))
        {
            // The app's window is inside the frame, except while minimized: then look it up by id.
            DWORD app = HostedApp(hwnd, pid);
            if (!app)
                if (auto model = WindowModelId(hwnd); !model.empty())
                    app = ProcessOfModel(model);
            pid = app;
        }
        return pid;
    }

    std::wstring ImageOf(DWORD pid)
    {
        return PathOf(pid);
    }

    std::wstring IconFile(HWND hwnd)
    {
        auto exe = ProcessPath(hwnd);
        auto name = exe.substr(exe.find_last_of(L'\\') + 1);
        if (_wcsicmp(name.c_str(), L"WindowsTerminal.exe") != 0 && _wcsicmp(name.c_str(), L"OpenConsole.exe") != 0 &&
            _wcsicmp(name.c_str(), L"conhost.exe") != 0)
            return exe;
        // One terminal process hosts every tab of every window, so which shell a window shows is told
        // by its title (the current tab's); the shells running under the terminal give their paths.
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        std::wstring pwsh, powershell, bash;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            PROCESSENTRY32W e{ sizeof(e) };
            for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
            {
                if (e.th32ParentProcessID != pid)
                    continue;
                if (_wcsicmp(e.szExeFile, L"pwsh.exe") == 0 && pwsh.empty())
                    pwsh = PathOf(e.th32ProcessID);
                else if (_wcsicmp(e.szExeFile, L"powershell.exe") == 0 && powershell.empty())
                    powershell = PathOf(e.th32ProcessID);
                else if (_wcsicmp(e.szExeFile, L"bash.exe") == 0 && bash.empty())
                    bash = PathOf(e.th32ProcessID);
            }
            CloseHandle(snap);
        }
        wchar_t raw[256]{};
        GetWindowTextW(hwnd, raw, ARRAYSIZE(raw));
        std::wstring title = raw;
        CharLowerBuffW(title.data(), static_cast<DWORD>(title.size()));
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        auto exists = [](std::wstring const& p) { return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };

        if (title.find(L"mingw") != std::wstring::npos || title.find(L"msys") != std::wstring::npos || title.find(L"bash") != std::wstring::npos)
        {
            // Git Bash: git-bash.exe at the root of the Git folder the shell runs from.
            std::wstring root = bash;
            CharLowerBuffW(root.data(), static_cast<DWORD>(root.size()));
            auto at = root.find(L"\\git\\");
            std::wstring gitBash = at != std::wstring::npos ? bash.substr(0, at + 5) + L"git-bash.exe" : L"C:\\Program Files\\Git\\git-bash.exe";
            if (exists(gitBash))
                return gitBash;
            return exists(bash) ? bash : exe;
        }
        if (title.find(L"명령 프롬프트") != std::wstring::npos || title.find(L"command prompt") != std::wstring::npos ||
            title.find(L"cmd.exe") != std::wstring::npos)
            return std::wstring(system) + L"\\cmd.exe";
        // Otherwise the terminal's usual shell: PowerShell (7 if it is the one running).
        if (exists(pwsh))
            return pwsh;
        if (exists(powershell))
            return powershell;
        auto fallback = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        return exists(fallback) ? fallback : exe;
    }

    std::wstring ModelOf(DWORD pid)
    {
        HANDLE proc = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
        if (!proc)
            return {};
        wchar_t id[APPLICATION_USER_MODEL_ID_MAX_LENGTH]{};
        UINT32 len = ARRAYSIZE(id);
        bool ok = GetApplicationUserModelId(proc, &len, id) == ERROR_SUCCESS;
        CloseHandle(proc);
        return ok ? std::wstring(id) : std::wstring();
    }

    bool IsCardApp(std::wstring const& cardApp, std::wstring const& image, std::wstring const& model)
    {
        auto id = cardApp.substr(0, cardApp.find(L'#'));     // a web app plays through its browser
        if (id.rfind(L"uwp:", 0) == 0)
            return !model.empty() && _wcsicmp(id.c_str() + 4, model.c_str()) == 0;
        return SameApp(image, id);
    }

    bool IsShellWindow(HWND hwnd)
    {
        return IsShellClass(hwnd);
    }

    static std::wstring DirOf(std::wstring const& path)
    {
        auto slash = path.find_last_of(L'\\');
        return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
    }

    static bool UnderDir(std::wstring const& path, std::wstring const& dir)
    {
        return !dir.empty() && path.size() > dir.size() && _wcsnicmp(path.c_str(), dir.c_str(), dir.size()) == 0;
    }

    static bool InWindowsDir(std::wstring const& path)
    {
        wchar_t win[MAX_PATH]{};
        UINT n = GetWindowsDirectoryW(win, MAX_PATH);
        return n && UnderDir(path, std::wstring(win, n) + L"\\");
    }

    // Whether a top-level window belongs to the process: its own, or a UWP frame hosting it.
    static bool WindowOf(HWND h, DWORD pid)
    {
        DWORD p = 0;
        GetWindowThreadProcessId(h, &p);
        if (p == pid)
            return true;
        wchar_t cls[32]{};
        GetClassNameW(h, cls, ARRAYSIZE(cls));
        return wcscmp(cls, L"ApplicationFrameWindow") == 0 && HostedApp(h, p) == pid;
    }

    static bool IsSystemImage(std::wstring const& path)
    {
        static constexpr wchar_t const* kShell[] = {
            L"explorer.exe", L"ApplicationFrameHost.exe", L"dwm.exe", L"sihost.exe", L"csrss.exe",
            L"winlogon.exe", L"ShellExperienceHost.exe", L"StartMenuExperienceHost.exe", L"SearchHost.exe",
            L"TextInputHost.exe", L"LockApp.exe", L"svchost.exe", L"services.exe", L"RuntimeBroker.exe",
        };
        auto name = path.substr(path.find_last_of(L'\\') + 1);
        for (auto n : kShell)
            if (_wcsicmp(name.c_str(), n) == 0)
                return true;
        return false;
    }

    static ULONGLONG CreatedAt(HANDLE proc);

    // Some apps draw their window in a helper process (Steam: steamwebhelper.exe under Steam's folder).
    // Quitting the helper alone leaves the app running (and restarting it): walk up to the process that
    // started it while that one runs from the same install (its folder contains the helper's).
    static DWORD MainProcess(DWORD pid)
    {
        if (!pid)
            return 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return pid;
        std::vector<PROCESSENTRY32W> all;
        PROCESSENTRY32W e{ sizeof(e) };
        for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
            all.push_back(e);
        CloseHandle(snap);
        for (int depth = 0; depth < 4; ++depth)
        {
            auto self = std::find_if(all.begin(), all.end(), [&](auto& p) { return p.th32ProcessID == pid; });
            if (self == all.end())
                break;
            HANDLE child = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            HANDLE parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, self->th32ParentProcessID);
            bool up = false;
            if (child && parent)
            {
                auto childPath = ImagePath(child), parentPath = ImagePath(parent);
                // Older than the child (else the id was reused), same install, not Windows itself.
                up = CreatedAt(parent) <= CreatedAt(child) && UnderDir(childPath, DirOf(parentPath)) &&
                     !InWindowsDir(parentPath) && !IsSystemImage(parentPath);
            }
            if (child)
                CloseHandle(child);
            if (parent)
                CloseHandle(parent);
            if (!up)
                break;
            pid = self->th32ParentProcessID;
        }
        return pid;
    }

    DWORD QuitTarget(HWND hwnd)
    {
        // A browser web app shares its process with every window of that browser: only closing the
        // window is right for it.
        if (AppId(hwnd).find(L'#') != std::wstring::npos)
            return 0;
        DWORD pid = MainProcess(AppProcess(hwnd));
        std::wstring path = PathOf(pid);
        if (!pid || pid == GetCurrentProcessId() || path.empty() || IsSystemImage(path))
            return 0;
        // Elevated apps can't be ended from here; then there is nothing to offer.
        HANDLE proc = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!proc)
            return 0;
        BOOL critical = FALSE;
        IsProcessCritical(proc, &critical);
        CloseHandle(proc);
        return critical ? 0 : pid;
    }

    bool IsInputPanel(HWND hwnd)
    {
        wchar_t cls[32]{};
        if (!GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) || wcscmp(cls, L"Windows.UI.Core.CoreWindow") != 0)
            return false;
        auto path = ProcessPath(hwnd);
        return path.size() >= 17 && _wcsicmp(path.c_str() + path.size() - 17, L"TextInputHost.exe") == 0;
    }

    void CloseWindowsOf(DWORD pid)
    {
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            if (IsWindowVisible(h) && !GetWindow(h, GW_OWNER) && WindowOf(h, static_cast<DWORD>(lp)))
                PostMessageW(h, WM_CLOSE, 0, 0);
            return TRUE;
        }, pid);
    }

    // A window still open after the close request is the app asking something ("save changes?") or
    // refusing to close: the user answers it, we don't kill underneath it. Windows on other virtual
    // desktops count too (cloaked by the shell), other cloaked windows don't.
    bool HasVisibleWindow(DWORD pid)
    {
        struct Ctx { DWORD pid; bool found; } ctx{ pid, false };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto c = reinterpret_cast<Ctx*>(lp);
            if (!IsWindowVisible(h) || (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) || !WindowOf(h, c->pid))
                return TRUE;
            DWORD cloaked = 0;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            RECT r{};
            GetWindowRect(h, &r);
            c->found = (!cloaked || cloaked == DWM_CLOAKED_SHELL) && r.right - r.left > 1 && r.bottom - r.top > 1;
            return !c->found;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.found;
    }

    static ULONGLONG CreatedAt(HANDLE proc)
    {
        FILETIME created{}, x1, x2, x3;
        GetProcessTimes(proc, &created, &x1, &x2, &x3);
        return (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }

    void KillProcess(HANDLE proc)
    {
        std::wstring image = ImagePath(proc);
        DWORD root = GetProcessId(proc);
        // Helpers first, so none of them restarts the main process meanwhile. A child must be younger
        // than its parent, or the parent id belongs to an older, unrelated process (ids are reused).
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE && !image.empty())
        {
            std::vector<PROCESSENTRY32W> all;
            PROCESSENTRY32W e{ sizeof(e) };
            for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
                all.push_back(e);
            std::vector<std::pair<DWORD, ULONGLONG>> parents{ { root, CreatedAt(proc) } };
            for (size_t i = 0; i < parents.size(); ++i)
                for (auto& c : all)
                {
                    if (c.th32ParentProcessID != parents[i].first || c.th32ProcessID == root)
                        continue;
                    HANDLE child = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, c.th32ProcessID);
                    if (!child)
                        continue;
                    ULONGLONG born = CreatedAt(child);
                    auto childImage = ImagePath(child);
                    // Its helpers: the same executable, or (outside the Windows folder) anything from its
                    // install folder, unless it has a window of its own (a game Steam started, say).
                    bool helper = _wcsicmp(childImage.c_str(), image.c_str()) == 0 ||
                                  (!InWindowsDir(image) && UnderDir(childImage, DirOf(image)) && !HasVisibleWindow(c.th32ProcessID));
                    if (born >= parents[i].second && helper)
                    {
                        parents.push_back({ c.th32ProcessID, born });
                        TerminateProcess(child, 1);
                    }
                    CloseHandle(child);
                }
        }
        if (snap != INVALID_HANDLE_VALUE)
            CloseHandle(snap);
        TerminateProcess(proc, 1);
    }

    // Updates move some apps to a new folder: Squirrel installs (Discord, Slack: "...\app-1.0.9\...")
    // and Store apps ("...\WindowsApps\Name_1.2.3.0_x64__publisher\..."). Their pins and exclusions
    // are compared without the version, so they survive the update.
    std::wstring AppKey(std::wstring const& id)
    {
        std::wstring k = id;
        CharLowerBuffW(k.data(), static_cast<DWORD>(k.size()));
        if (k.rfind(L"uwp:", 0) == 0)
            return k;
        if (IsFrameHost(k))
            return L"uwp:*";                        // saved by older versions for every UWP app
        // "\app-1.0.9205\" or "\app-1.2.3-beta\": digits and dots with at least one dot, then an
        // optional "-suffix". A folder merely named "app-dev" or "app-2" is left alone.
        for (size_t at = k.find(L"\\app-"); at != std::wstring::npos; at = k.find(L"\\app-", at + 1))
        {
            size_t begin = at + 5, end = k.find(L'\\', begin);
            if (end == std::wstring::npos)
                break;
            size_t digits = k.find_first_not_of(L"0123456789.", begin);
            bool version = digits > begin && (k[begin] >= L'0' && k[begin] <= L'9') && k.find(L'.', begin) < digits &&
                           (digits == end || k[digits] == L'-');
            if (version)
                k.replace(begin, end - begin, L"*");
        }
        size_t at = k.find(L"\\windowsapps\\");
        if (at != std::wstring::npos)
        {
            size_t begin = at + 13, end = k.find(L'\\', begin);
            size_t first = k.find(L'_', begin), pub = k.find(L"__", begin);
            if (end != std::wstring::npos && first < pub && pub < end)
                k.erase(first, pub - first);        // "name_1.2.3.0_x64__pub" -> "name__pub"
        }
        return k;
    }

    bool SameApp(std::wstring const& a, std::wstring const& b)
    {
        auto ka = AppKey(a), kb = AppKey(b);
        if (ka == L"uwp:*" || kb == L"uwp:*")
            return (ka == L"uwp:*" || ka.rfind(L"uwp:", 0) == 0) && (kb == L"uwp:*" || kb.rfind(L"uwp:", 0) == 0);
        return ka == kb;
    }
}
