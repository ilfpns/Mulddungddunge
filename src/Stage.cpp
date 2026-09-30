#include "Stage.h"
#include "WindowTracker.h"
#include "VirtualDesktop.h"

namespace
{
    constexpr UINT WM_TRAY = WM_APP + 2;
    constexpr UINT WM_AUDIO = WM_APP + 3;     // Audio: which apps play sound changed
    constexpr wchar_t kTouchpadKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad";
    constexpr UINT ID_EXIT = 1;
    constexpr UINT ID_PIN = 2;
    constexpr UINT ID_UNPIN = 3;
    constexpr UINT ID_CLOSE = 4;
    constexpr UINT ID_SETTINGS = 6;
    constexpr UINT ID_AUTOSTART = 5;
    constexpr UINT ID_QUIT = 7;
    constexpr UINT ID_FIT = 8;                // the card's window takes the saved place and size
    // Where "크기 맞추기" puts a window: the visible frame of the Orca window as it was on 2026-09-30,
    // in screen pixels (left, top, right, bottom). Fixed on purpose.
    constexpr RECT kFitFrame = { 252, 52, 2228, 1377 };
    constexpr UINT ID_MOVE = 20;              // 20..28: send to desktop 1..9
    constexpr size_t kMaxMoveTargets = 9;

    // "<name>로 보내기" / "<name>으로 보내기": 으로 after a final consonant other than ㄹ (and after the
    // digits read that way: 3 삼, 6 육, 0 영).
    std::wstring SendLabel(std::wstring const& name)
    {
        wchar_t last = name.empty() ? L'1' : name.back();
        bool euro = false;
        if (last >= 0xAC00 && last <= 0xD7A3)
        {
            int final = (last - 0xAC00) % 28;
            euro = final != 0 && final != 8;
        }
        else if (last == L'3' || last == L'6' || last == L'0')
            euro = true;
        return name + (euro ? L"으로 보내기" : L"로 보내기");
    }
    constexpr float kAlertDot = 9.f;
    constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kRunValue[] = L"StageManager";
    constexpr wchar_t kStateKey[] = L"Software\\StageManager";

    // Card menu metrics (at 96 dpi).
    constexpr float kMenuW = 200.f;
    constexpr float kMenuItemH = 34.f;
    constexpr float kMenuSepH = 9.f;        // a separator line's row
    constexpr float kMenuIcon = 24.f;
    constexpr float kMenuPad = 5.f;
    constexpr float kMenuRadius = 10.f;
    constexpr int kHotkeyBase = 100;     // hotkey ids 100..103 = Alt+1..4
    constexpr UINT_PTR kTimerPopulate = 1;
    constexpr UINT_PTR kTimerActiveSnap = 2;
    constexpr UINT kActiveSnapDelayMs = 500;
    constexpr UINT_PTR kTimerMinimizeOut = 3;
    constexpr UINT_PTR kTimerFade = 4;
    constexpr UINT_PTR kTimerShrink = 5;
    constexpr UINT_PTR kTimerTrim = 6;
    constexpr UINT_PTR kTimerTuck = 7;
    constexpr UINT_PTR kTimerRecheck = 8;
    constexpr UINT kRecheckMs = 80;           // a new window not ready yet: looked at again this often
    constexpr int kMaxRechecks = 25;          // ...for about two seconds
    constexpr UINT_PTR kTimerDock = 9;
    constexpr UINT_PTR kTimerWatchdog = 10;
    constexpr UINT_PTR kTimerHidden = 12;     // a hidden stage window: closed, or only put away?
    constexpr UINT kHiddenMs = 300;
    constexpr UINT_PTR kTimerQuit = 14;       // an app asked to quit: gone, or still running in the background?
    constexpr UINT kQuitPollMs = 500;
    // Apps save and clean up after their windows close (Chrome, VS Code, Office); only one still
    // running this long after, with nothing on screen, is ended.
    constexpr ULONGLONG kQuitGraceMs = 5000;
    constexpr UINT_PTR kTimerNewWindow = 15;  // a new window not ready yet when it took focus
    constexpr UINT_PTR kTimerTrace = 16;      // the hover trace has run its course: remove it
    constexpr UINT_PTR kTimerSweep = 11;      // after a window left the taskbar: was it closed or only hidden?    // a transition still running after this long is forced to end
    constexpr UINT kWatchdogMs = 3000;         // re-evaluate the dock once a closing/minimizing window is gone
    constexpr float kEdgeStrip = 2.f;    // px of the tucked sidebar left at the screen edge to call it back

    constexpr float kSidebarW = 210.f;
    constexpr float kThumbW = 210.f;     // card size before the tilt foreshortens it; every card has this shape
    constexpr float kThumbH = 140.f;
    constexpr float kPitch = 172.f;
    constexpr float kTilt = 42.f;        // degrees
    constexpr float kDepthRatio = 2.4f;  // camera distance relative to card width
    constexpr float kRadius = 8.f;
    constexpr float kBadge = 34.f;
    constexpr float kPinBadge = 20.f;    // pin marker: dark disc with a white pin, readable on light and dark windows
    constexpr float kHoverGrow = 1.14f;     // hovered card turns to face the viewer and grows
    // Placeholders are drawn at twice their on-screen size: the tilt magnifies the near edge, and the
    // far edge gets a clean 2:1 average instead of skipped pixels.
    constexpr float kPlaceholderScale = 2.f;
    constexpr int kMaxHotkeys = 6;       // as many as the most cards the sidebar can show
    constexpr UINT_PTR kTimerFit = 13;   // a stage window moved: snapped next to the sidebar?
    constexpr int kSlideMs = 260;
    constexpr int kFlyMs = 420;
    constexpr int kFadeMs = 150;
    constexpr int kPaintWaitMs = 110;    // restored windows need a moment to repaint before the copy fades
    constexpr ULONGLONG kPrefetchMaxAge = 4000;
    // A picture of the stage window this recent (and with the window unmoved) is reused instead of
    // starting another capture session, which is most of what a switch costs.
    constexpr ULONGLONG kReuseMaxAge = 3000;
    constexpr float kDragStart = 8.f;    // px of movement before a press turns into a drag
    constexpr ULONGLONG kQuietMs = 700;

    Stage* g_stage = nullptr;


    // Opt-in event trace for diagnosing window-manager interactions: create %TEMP%\stage-manager.trace
    // before starting and events are appended to it. Off (one check at startup) otherwise.
    // Opened once at startup when enabled and kept open: opening a file per event (with antivirus
    // scanning each open) cost tens of milliseconds per event.
    FILE* g_trace = nullptr;

    void Trace(wchar_t const* what, HWND hwnd = nullptr)
    {
        if (!g_trace)
            return;
        wchar_t title[64]{};
        if (hwnd)
            GetWindowTextW(hwnd, title, ARRAYSIZE(title));
        fwprintf(g_trace, L"%llu %s %p '%s'\n", GetTickCount64(), what, hwnd, title);
        fflush(g_trace);
    }

    // Appends to %TEMP%\stage-manager.log; only written when something goes wrong.
    void LogError(UINT msg, HRESULT hr, wchar_t const* text)
    {
        wchar_t path[MAX_PATH];
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"stage-manager.log");
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"a, ccs=UTF-8") == 0 && f)
        {
            fwprintf(f, L"msg=0x%04X hr=0x%08X %s\n", msg, static_cast<unsigned>(hr), text);
            fclose(f);
        }
    }

    // A Direct2D path handed to composition (CompositionPath takes an IGeometrySource2D). Composition
    // asks for the path built with its own Direct2D factory, so the source keeps how to build it.
    struct GeometrySource : winrt::implements<GeometrySource, winrt::Windows::Graphics::IGeometrySource2D,
                                              ABI::Windows::Graphics::IGeometrySource2DInterop>
    {
        using Build = std::function<winrt::com_ptr<ID2D1Geometry>(ID2D1Factory*)>;
        Build build;
        winrt::com_ptr<ID2D1Factory> factory;
        GeometrySource(Build b, winrt::com_ptr<ID2D1Factory> f) : build(std::move(b)), factory(std::move(f)) {}
        HRESULT __stdcall GetGeometry(ID2D1Geometry** value) noexcept override
        {
            return TryGetGeometryUsingFactory(factory.get(), value);
        }
        HRESULT __stdcall TryGetGeometryUsingFactory(ID2D1Factory* f, ID2D1Geometry** value) noexcept override
        {
            *value = nullptr;
            try
            {
                build(f).copy_to(value);
                return *value ? S_OK : E_FAIL;
            }
            catch (...)
            {
                return E_FAIL;
            }
        }
    };

    constexpr int kTraceSegments = 6;       // the trail is drawn as this many pieces, fading toward its end
    constexpr float kTraceTail = 0.42f;     // trail length, as a fraction of the path (half the outline)
    constexpr int kTraceHeadMs = 800;       // the head from the top edge's middle to the bottom edge's middle
    constexpr float kTraceWidth = 1.f;      // px: a hairline
    constexpr int kAlertCycleMs = 2600;     // attention loop: one run, then a rest (DWM idles meanwhile)

    wuc::CompositionEasingFunction MakeEase(wuc::Compositor const& c)
    {
        // Smooth ease-out: quick departure, long gentle landing.
        return c.CreateCubicBezierEasingFunction({ 0.22f, 0.8f }, { 0.28f, 1.f });
    }
}

// Keeps Task Manager's "Memory" (private working set) under ~8MB. Trimming has a cost of its own
// (and pages are faulted back in right after), so it only happens when above that.
void TrimMemory()
{
    PROCESS_MEMORY_COUNTERS_EX2 pmc{ sizeof(pmc) };
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)) &&
        pmc.PrivateWorkingSetSize < 8u * 1024 * 1024)
        return;
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

bool Stage::Init(HINSTANCE inst)
{
    g_stage = this;
    {
        wchar_t path[MAX_PATH];
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"stage-manager.trace");
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            g_trace = _wfsopen(path, L"a, ccs=UTF-8", _SH_DENYNO);   // others can read it while we run
    }

    m_cfg.Load();
    wt::SetExcluded(m_cfg.excluded);
    m_mon = ChosenMonitor();
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(m_mon, &mi);
    m_monitor = mi.rcMonitor;
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(m_mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);

    // The user's minimize-animation setting is kept in the registry while we run, so it can still be
    // restored after a crash or a forced kill: a value left over from last time is the real original.
    DWORD saved = 0, size = sizeof(saved);
    if (RegGetValueW(HKEY_CURRENT_USER, kStateKey, L"MinAnimate", RRF_RT_REG_DWORD, nullptr, &saved, &size) == ERROR_SUCCESS)
        m_savedMinAnimate = static_cast<int>(saved);
    else
    {
        ANIMATIONINFO ai{ sizeof(ai) };
        SystemParametersInfoW(SPI_GETANIMATION, sizeof(ai), &ai, 0);
        m_savedMinAnimate = ai.iMinAnimate;
        saved = static_cast<DWORD>(m_savedMinAnimate);
        RegSetKeyValueW(HKEY_CURRENT_USER, kStateKey, L"MinAnimate", REG_DWORD, &saved, sizeof(saved));
    }
    m_scale = dpiX / 96.f;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpfnWndProc = SidebarProc;
    wc.lpszClassName = L"StageManagerSidebar";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = ViewProc;
    wc.lpszClassName = L"StageManagerView";
    RegisterClassExW(&wc);

    m_sidebar = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        L"StageManagerSidebar", L"Stage Manager", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    m_view = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        L"StageManagerView", L"Stage Manager", WS_POPUP,
        m_monitor.left, m_monitor.top, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!m_sidebar || !m_view)
        return false;

    DispatcherQueueOptions opts{ sizeof(opts), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA };
    winrt::check_hresult(CreateDispatcherQueueController(opts,
        reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(m_queue))));

    m_compositor = wuc::Compositor();
    auto interop = m_compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(m_view, TRUE,
        reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(m_target))));
    m_root = m_compositor.CreateContainerVisual();
    m_target.Root(m_root);
    m_sideContent = m_compositor.CreateContainerVisual();
    m_root.Children().InsertAtTop(m_sideContent);
    m_animStage = m_compositor.CreateContainerVisual();
    m_root.Children().InsertAtTop(m_animStage);
    m_placeholderBrush = m_compositor.CreateColorBrush({ 255, 58, 58, 64 });
    m_ease = MakeEase(m_compositor);
    m_snap.Init(m_compositor);
    m_snap.SetQuality(m_cfg.quality);
    LoadPins();
    m_desktops = winrt::try_create_instance<IVirtualDesktopManager>(CLSID_VirtualDesktopManager);

    Dock();
    ShrinkView();
    m_taskbarMsg = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();
    if (m_cfg.sounds)
        m_audio.Start(m_sidebar, WM_AUDIO);
    RestoreTouchpadGesture();
    SetTimer(m_sidebar, kTimerPopulate, 400, nullptr);
    // While we run, a minimized stage window flies into the sidebar instead of shrinking to the
    // taskbar. Not persisted (no SPIF_UPDATEINIFILE); restored on exit. Only now: an Init that
    // failed earlier never reaches Shutdown, which restores it.
    SetMinAnimate(false);
    return true;
}

void Stage::Shutdown()
{
    SetHotkeys(false);
    m_audio.Stop();
    for (auto hook : m_hooks)
        UnhookWinEvent(hook);
    for (auto& [pid, hook] : m_moveHooks)
        UnhookWinEvent(hook);
    DeregisterShellHookWindow(m_sidebar);
    if (g_trace)
        fclose(g_trace);
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = m_sidebar;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (m_trayIcon)
        DestroyIcon(m_trayIcon);
    for (auto& q : m_quitting)
        CloseHandle(q.proc);
    m_quitting.clear();
    SetMinAnimate(true);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kStateKey, L"MinAnimate");
    DestroyWindow(m_view);
    DestroyWindow(m_sidebar);
}

void Stage::Dock()
{
    // A strip along the left (or right) of the work area. Nothing is reserved: maximized windows use the
    // whole screen, and the sidebar gets out of their way instead (see UpdateDock).
    // Monitor handles can go stale after a display change: look the monitor up again.
    m_mon = ChosenMonitor();
    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(m_mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        m_scale = dpiX / 96.f;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(m_mon, &mi);
    m_monitor = mi.rcMonitor;
    LONG barW = static_cast<LONG>(BarW());
    m_bar = Right() ? RECT{ mi.rcWork.right - barW, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom }
                    : RECT{ mi.rcWork.left, mi.rcWork.top, mi.rcWork.left + barW, mi.rcWork.bottom };

    float2 barSize{ static_cast<float>(m_bar.right - m_bar.left), static_cast<float>(m_bar.bottom - m_bar.top) };
    float2 barOrigin = SideToAnim({ 0.f, 0.f });
    // Where the cards are for the current dock state (a slide in progress is cut short).
    float x = barOrigin.x;
    if (m_dock != DockState::Shown)
        x = Right() ? x + static_cast<float>(m_monitor.right - m_bar.left) : x - static_cast<float>(m_bar.right - m_monitor.left);
    m_sideContent.StopAnimation(L"Offset.X");
    m_sideContent.Offset({ x, barOrigin.y, 0.f });
    if (!m_busy && !m_dragging && !m_menu.open && !m_settings.open)
        ShrinkView();

    float2 eye{ BarW() / 2.f, barSize.y / 2.f };
    m_sideContent.TransformMatrix(Perspective(eye));
    m_animStage.TransformMatrix(Perspective(SideToAnim(eye)));
}

void Stage::AddTrayIcon()
{
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = m_sidebar;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    if (!m_trayIcon)
        m_trayIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    nid.hIcon = m_trayIcon;
    wcscpy_s(nid.szTip, L"Stage Manager");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void Stage::ShowTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_SETTINGS, L"설정");
    AppendMenuW(menu, MF_STRING | (StartsWithWindows() ? MF_CHECKED : 0), ID_AUTOSTART, L"Windows 시작 시 실행");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"종료");
    UINT command = 0;
    ShowMenu(menu, &command);
    if (command == ID_AUTOSTART)
        SetStartsWithWindows(!StartsWithWindows());
    else if (command == ID_SETTINGS && !m_busy && !m_dragging)
    {
        CloseCardMenu();
        OpenSettings();
    }
    else if (command == ID_EXIT)
        PostQuitMessage(0);
}

// REG_MULTI_SZ HKCU\Software\StageManager\Pinned: one executable path per pinned window.
void Stage::LoadPins()
{
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kStateKey, L"Pinned", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS)
        return;
    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kStateKey, L"Pinned", RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(), &size) != ERROR_SUCCESS)
        return;
    for (wchar_t const* p = buffer.c_str(); *p; p += wcslen(p) + 1)
        m_pinnedApps.emplace_back(p);
}

void Stage::SavePins() const
{
    std::wstring buffer;
    for (auto& app : m_pinnedApps)
        buffer.append(app).push_back(L'\0');
    buffer.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kStateKey, L"Pinned", REG_MULTI_SZ, buffer.data(),
        static_cast<DWORD>(buffer.size() * sizeof(wchar_t)));
}

bool Stage::StartsWithWindows()
{
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void Stage::SetStartsWithWindows(bool on)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (on)
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<BYTE const*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    else
        RegDeleteValueW(key, kRunValue);
    RegCloseKey(key);
}

// Popup menus only dismiss properly when their owner is the foreground window; afterwards focus
// goes back to the window on stage.
void Stage::ShowMenu(HMENU menu, UINT* command)
{
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(m_view);
    *command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, m_view, nullptr);
    PostMessageW(m_view, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (m_active && IsWindow(m_active))
        SetForegroundWindow(m_active);
}

void Stage::OpenCardMenu(int index, int anchorY)
{
    if (m_menu.open)
        CloseCardMenu();
    auto card = index >= 0 ? m_visible[index] : nullptr;
    m_menu = {};
    m_menu.open = true;
    m_menu.card = card;
    if (card)
    {
        m_menu.items = {
            card->pinned ? MenuItem{ ID_UNPIN, L"\xE77A", L"고정 해제" } : MenuItem{ ID_PIN, L"\xE718", L"탭 고정" },
            MenuItem{ ID_CLOSE, L"\xE8BB", L"창 닫기" },
        };
        m_menu.items.push_back(MenuItem{ ID_FIT, L"\xE740", L"크기 맞추기" });
        if (wt::QuitTarget(card->hwnd))
            m_menu.items.push_back(MenuItem{ ID_QUIT, L"\xE7E8", L"앱 종료" });
        // The window's own actions, then where to send it, then settings, each group set apart.
        m_menu.items.push_back(MenuItem{ 0, nullptr, nullptr });
        m_menuDesktops = vd::Desktops();            // empty where the move isn't supported
        if (m_menuDesktops.size() > 1)
        {
            // Named as in Task View: the name given to it, else "데스크톱 N".
            auto names = vd::Names();
            m_menu.labels.reserve(kMaxMoveTargets);     // the items point into these strings
            for (size_t d = 0; d < m_menuDesktops.size() && d < kMaxMoveTargets; ++d)
            {
                if (m_menuDesktops[d] == m_desktopId)
                    continue;
                std::wstring name = d < names.size() && !names[d].empty() ? names[d] : L"데스크톱 " + std::to_wstring(d + 1);
                m_menu.labels.push_back(SendLabel(name));
                m_menu.items.push_back(MenuItem{ ID_MOVE + static_cast<UINT>(d), L"\xE8A7", m_menu.labels.back().c_str() });
            }
        }
        if (m_menu.items.back().command != 0)
            m_menu.items.push_back(MenuItem{ 0, nullptr, nullptr });
    }
    m_menu.items.push_back(MenuItem{ ID_SETTINGS, L"\xE713", L"설정" });
    float w = S(kMenuW), h = MenuTop(m_menu.items.size()) + S(kMenuPad);

    auto dwrite = m_snap.Text();
    winrt::com_ptr<IDWriteTextFormat> text, icon;
    dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, S(13.5f), L"ko-kr", text.put());
    if (FAILED(dwrite->CreateTextFormat(L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, S(14.f), L"", icon.put())))
        dwrite->CreateTextFormat(L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, S(14.f), L"", icon.put());
    for (auto* f : { text.get(), icon.get() })
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // One line per item: a label too long for the panel ends in an ellipsis instead of wrapping.
    text->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    winrt::com_ptr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(text.get(), ellipsis.put())))
        text->SetTrimming(&trim, ellipsis.get());

    auto labels = m_snap.Paint(w, h, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> ink, line;
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.94f), ink.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.13f), line.put());
        for (size_t i = 0; i < m_menu.items.size(); ++i)
        {
            auto& item = m_menu.items[i];
            float top = MenuTop(i), bottom = top + MenuItemH(i);
            if (!item.command)
            {
                float mid = std::round((top + bottom) / 2.f);
                dc->FillRectangle({ S(12), mid, w - S(12), mid + 1.f }, line.get());
                continue;
            }
            D2D1_RECT_F textRect{ S(42), top, w - S(10), bottom };
            dc->DrawTextW(item.label, static_cast<UINT32>(wcslen(item.label)), text.get(), textRect, ink.get());
        }
    });
    // Each icon on its own visual, so hovering its row can move it.
    icon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    float iconSize = std::round(S(kMenuIcon));
    for (size_t i = 0; i < m_menu.items.size(); ++i)
    {
        auto& item = m_menu.items[i];
        if (!item.command)
        {
            m_menu.icons.push_back(nullptr);
            m_menu.tints.push_back(nullptr);
            m_menu.iconAt.push_back({});
            continue;
        }
        auto glyph = m_snap.Paint(iconSize, iconSize, [&](ID2D1DeviceContext* dc) {
            winrt::com_ptr<ID2D1SolidColorBrush> white;
            dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f), white.put());
            dc->DrawTextW(item.glyph, 1, icon.get(), { 0, 0, iconSize, iconSize }, white.get());
        });
        // The glyph masks a color brush, so hovering can change its color.
        auto tint = m_compositor.CreateColorBrush({ 255, 255, 255, 255 });
        auto mask = m_compositor.CreateMaskBrush();
        mask.Source(tint);
        mask.Mask(m_compositor.CreateSurfaceBrush(glyph));
        auto sprite = m_compositor.CreateSpriteVisual();
        sprite.Size({ iconSize, iconSize });
        sprite.Brush(mask);
        m_menu.tints.push_back(tint);
        sprite.CenterPoint({ iconSize / 2.f, iconSize / 2.f, 0.f });
        sprite.Opacity(0.72f);
        float3 at{ S(27) - iconSize / 2.f, MenuTop(i) + MenuItemH(i) / 2.f - iconSize / 2.f, 0.f };
        sprite.Offset(at);
        m_menu.icons.push_back(sprite);
        m_menu.iconAt.push_back(at);
    }

    auto rounded = [&](float2 size, float radius) {
        auto g = m_compositor.CreateRoundedRectangleGeometry();
        g.Size(size);
        g.CornerRadius({ radius, radius });
        return m_compositor.CreateGeometricClip(g);
    };
    m_menu.root = m_compositor.CreateContainerVisual();
    m_menu.root.Size({ w, h });

    auto shadow = m_compositor.CreateDropShadow();
    shadow.BlurRadius(S(24));
    shadow.Opacity(0.45f);
    shadow.Offset({ 0.f, S(6), 0.f });
    auto shadowHost = m_compositor.CreateSpriteVisual();
    shadowHost.Size({ w - S(8), h - S(8) });
    shadowHost.Offset({ S(4), S(4), 0.f });
    shadowHost.Shadow(shadow);
    m_menu.root.Children().InsertAtTop(shadowHost);

    auto border = m_compositor.CreateSpriteVisual();                 // 1px hairline around the panel
    border.Size({ w, h });
    border.Brush(m_compositor.CreateColorBrush({ 40, 255, 255, 255 }));
    border.Clip(rounded({ w, h }, S(kMenuRadius)));
    m_menu.root.Children().InsertAtTop(border);

    auto panel = m_compositor.CreateSpriteVisual();
    panel.Size({ w - 2.f, h - 2.f });
    panel.Offset({ 1.f, 1.f, 0.f });
    panel.Brush(m_compositor.CreateColorBrush({ 246, 36, 36, 40 }));
    panel.Clip(rounded({ w - 2.f, h - 2.f }, S(kMenuRadius) - 1.f));
    m_menu.root.Children().InsertAtTop(panel);

    m_menu.highlight = m_compositor.CreateSpriteVisual();
    m_menu.highlight.Size({ w - S(kMenuPad) * 2, S(kMenuItemH) });
    m_menu.highlight.Brush(m_compositor.CreateColorBrush({ 26, 255, 255, 255 }));
    m_menu.highlight.Clip(rounded({ w - S(kMenuPad) * 2, S(kMenuItemH) }, S(6)));
    m_menu.highlight.Opacity(0.f);
    m_menu.root.Children().InsertAtTop(m_menu.highlight);

    auto text2 = m_compositor.CreateSpriteVisual();
    text2.Size({ w, h });
    text2.Brush(m_compositor.CreateSurfaceBrush(labels));
    m_menu.root.Children().InsertAtTop(text2);
    for (auto& i : m_menu.icons)
        if (i)
            m_menu.root.Children().InsertAtTop(i);

    // Next to the card, kept on screen.
    float2 cardCenter{ 0.f, card ? SideToAnim(SlotCenter(index)).y : static_cast<float>(anchorY) };
    float x = Right() ? static_cast<float>(m_bar.left - m_monitor.left) - S(6) - w
                      : static_cast<float>(m_bar.right - m_monitor.left) + S(6);
    float y = std::clamp(cardCenter.y - h / 2.f, S(8), static_cast<float>(m_monitor.bottom - m_monitor.top) - h - S(8));
    m_menu.origin = { x, y };
    m_menu.root.Offset({ x, y, 0.f });
    m_menu.root.CenterPoint({ 0.f, h / 2.f, 0.f });
    m_root.Children().InsertAtTop(m_menu.root);

    auto grow = m_compositor.CreateVector3KeyFrameAnimation();
    grow.InsertKeyFrame(0.f, { 0.94f, 0.94f, 1.f });
    grow.InsertKeyFrame(1.f, { 1.f, 1.f, 1.f }, m_ease);
    grow.Duration(std::chrono::milliseconds(160));
    m_menu.root.StartAnimation(L"Scale", grow);
    auto fade = m_compositor.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.f, 0.f);
    fade.InsertKeyFrame(1.f, 1.f, m_ease);
    fade.Duration(std::chrono::milliseconds(120));
    m_menu.root.StartAnimation(L"Opacity", fade);

    SetHover(-1);
    GrowView();         // the menu sits outside the bar, and a click anywhere else closes it
}

void Stage::CloseCardMenu()
{
    if (!m_menu.open)
        return;
    auto root = m_menu.root;
    m_menu = {};
    auto fade = m_compositor.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1.f, 0.f);
    fade.Duration(std::chrono::milliseconds(90));
    auto batch = m_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    root.StartAnimation(L"Opacity", fade);
    batch.End();
    batch.Completed([this, root](auto&&, auto&&) {
        m_root.Children().Remove(root);         // releases the text surface too
        TrimMemory();
        if (!m_busy && !m_dragging && !m_menu.open && !m_settings.open)
            ShrinkView();
    });
}

float Stage::MenuItemH(size_t item) const
{
    return m_menu.items[item].command ? S(kMenuItemH) : S(kMenuSepH);
}

float Stage::MenuTop(size_t item) const
{
    float y = S(kMenuPad);
    for (size_t i = 0; i < item && i < m_menu.items.size(); ++i)
        y += MenuItemH(i);
    return y;
}

int Stage::MenuItemAt(POINT viewPt) const
{
    float x = viewPt.x - m_menu.origin.x, y = viewPt.y - m_menu.origin.y;
    if (x < 0 || x > S(kMenuW))
        return -1;
    for (size_t i = 0; i < m_menu.items.size(); ++i)
    {
        float top = MenuTop(i);
        if (y >= top && y < top + MenuItemH(i))
            return m_menu.items[i].command ? static_cast<int>(i) : -1;     // a separator is nothing
    }
    return -1;
}

// A hovered row's icon comes alive a little, each in its own way, on a spring so it overshoots
// and settles; it springs back when the pointer moves on.
void Stage::AnimateMenuIcon(size_t item, bool hovered)
{
    if (item >= m_menu.icons.size() || !m_menu.icons[item])
        return;
    auto icon = m_menu.icons[item];
    UINT command = m_menu.items[item].command;
    float angle = 0.f, scale = 1.f, shift = 0.f;
    winrt::Windows::UI::Color color{ 255, 255, 255, 255 };
    if (hovered)
    {
        if (command == ID_PIN || command == ID_UNPIN)
            angle = -20.f, scale = 1.15f, color = { 255, 255, 196, 86 };       // the pin tips over, amber
        else if (command == ID_CLOSE)
            angle = 90.f, scale = 1.1f, color = { 255, 255, 128, 110 };        // the cross turns, coral
        else if (command == ID_QUIT)
            scale = 1.28f, color = { 255, 255, 84, 84 };                       // the power sign pops, red
        else if (command == ID_FIT)
            scale = 1.22f, color = { 255, 186, 150, 255 };                     // the frame stretches, violet
        else if (command >= ID_MOVE && command < ID_MOVE + 9)
            shift = S(4), scale = 1.1f, color = { 255, 110, 182, 255 };        // off toward the other desktop, sky blue
        else if (command == ID_SETTINGS)
            angle = 60.f, scale = 1.1f, color = { 255, 230, 201, 138 };        // the gear turns, champagne gold
    }
    if (auto tint = m_menu.tints[item])
    {
        if (m_cfg.speed == 3)
            tint.Color(color);
        else
        {
            auto shade = m_compositor.CreateColorKeyFrameAnimation();
            shade.InsertKeyFrame(1.f, color);
            shade.Duration(std::chrono::milliseconds(m_cfg.Ms(150)));
            tint.StartAnimation(L"Color", shade);
        }
    }
    if (m_cfg.speed == 3)
    {
        icon.RotationAngleInDegrees(angle);
        icon.Scale({ scale, scale, 1.f });
        icon.Offset(m_menu.iconAt[item] + float3{ shift, 0.f, 0.f });
        icon.Opacity(hovered ? 1.f : 0.72f);
        return;
    }
    auto period = std::chrono::milliseconds(m_cfg.Ms(45));
    auto spinner = m_compositor.CreateSpringScalarAnimation();
    spinner.FinalValue(angle);
    spinner.DampingRatio(0.42f);
    spinner.Period(period);
    icon.StartAnimation(L"RotationAngleInDegrees", spinner);
    auto grow = m_compositor.CreateSpringVector3Animation();
    grow.FinalValue(winrt::box_value(float3{ scale, scale, 1.f }).as<winrt::Windows::Foundation::IReference<float3>>());
    grow.DampingRatio(0.42f);
    grow.Period(period);
    icon.StartAnimation(L"Scale", grow);
    auto slide = m_compositor.CreateSpringVector3Animation();
    slide.FinalValue(winrt::box_value(m_menu.iconAt[item] + float3{ shift, 0.f, 0.f }).as<winrt::Windows::Foundation::IReference<float3>>());
    slide.DampingRatio(0.5f);
    slide.Period(period);
    icon.StartAnimation(L"Offset", slide);
    auto light = m_compositor.CreateScalarKeyFrameAnimation();
    light.InsertKeyFrame(1.f, hovered ? 1.f : 0.72f);
    light.Duration(std::chrono::milliseconds(m_cfg.Ms(120)));
    icon.StartAnimation(L"Opacity", light);
}

void Stage::SetMenuHover(int item)
{
    if (item == m_menu.hover)
        return;
    bool wasHidden = m_menu.hover < 0;
    if (m_menu.hover >= 0)
        AnimateMenuIcon(static_cast<size_t>(m_menu.hover), false);
    m_menu.hover = item;
    if (item >= 0)
        AnimateMenuIcon(static_cast<size_t>(item), true);
    auto fade = m_compositor.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1.f, item >= 0 ? 1.f : 0.f);
    fade.Duration(std::chrono::milliseconds(100));
    m_menu.highlight.StartAnimation(L"Opacity", fade);
    if (item < 0)
        return;
    float3 to{ S(kMenuPad), MenuTop(static_cast<size_t>(item)), 0.f };
    if (wasHidden)
        m_menu.highlight.Offset(to);
    else
    {
        auto slide = m_compositor.CreateVector3KeyFrameAnimation();
        slide.InsertKeyFrame(1.f, to, m_ease);
        slide.Duration(std::chrono::milliseconds(120));
        m_menu.highlight.StartAnimation(L"Offset", slide);
    }
}

void Stage::RunMenuItem(int item)
{
    UINT command = m_menu.items[item].command;
    auto card = m_menu.card;
    CloseCardMenu();
    if (command != ID_SETTINGS && (!card || !IsWindow(card->hwnd)))
        return;                                     // its window closed while the menu was open
    if (command == ID_SETTINGS)
        OpenSettings();
    else if (command == ID_PIN || command == ID_UNPIN)
        SetPinned(*card, command == ID_PIN);
    else if (command == ID_CLOSE)
        PostMessageW(card->hwnd, WM_CLOSE, 0, 0);   // the card goes away when the window is destroyed
    else if (command == ID_QUIT)
        QuitApp(card->hwnd);
    else if (command == ID_FIT)
        FitToSaved(card->hwnd);
    else if (command >= ID_MOVE && command < ID_MOVE + m_menuDesktops.size())
    {
        GUID to = m_menuDesktops[command - ID_MOVE];
        if (vd::MoveWindow(card->hwnd, to))
        {
            Trace(L"sent to another desktop", card->hwnd);
            card->desktop = to;
            LeaveStage(card->hwnd);
            Relayout(true);
        }
    }
}

// Closes every window of the app the way its close buttons would, then, if it keeps running with
// nothing on screen (gone to the tray or the background), ends the process. An app that leaves a
// window open (a "save changes?" question, a window on another desktop) is left alone.
bool Stage::QuitApp(HWND hwnd, bool fromHide)
{
    DWORD pid = wt::QuitTarget(hwnd);
    if (!pid)
        return false;
    // Held open so the id can't be reused by another process meanwhile; checked to still be the
    // window's process after opening it.
    HANDLE proc = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!proc)
    {
        Trace(L"quit: no access (elevated app?)", hwnd);
        return false;
    }
    if (wt::QuitTarget(hwnd) != pid)
    {
        CloseHandle(proc);
        return false;
    }
    if (fromHide && wt::HasVisibleWindow(pid))
    {
        CloseHandle(proc);                          // other windows of it are still open: keep it running
        return false;
    }
    Trace(fromHide ? L"quit (closed to the tray)" : L"quit", hwnd);
    if (!fromHide)
        wt::CloseWindowsOf(pid);
    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    m_quitting.push_back({ proc, hwnd, windowPid, GetTickCount64() + kQuitGraceMs, fromHide });
    SetTimer(m_sidebar, kTimerQuit, kQuitPollMs, nullptr);
    return true;
}

void Stage::FinishQuits()
{
    ULONGLONG now = GetTickCount64();
    bool finished = false;
    auto quitting = std::move(m_quitting);          // nothing below may add to the list being walked
    m_quitting.clear();
    for (auto& q : quitting)
    {
        bool done = WaitForSingleObject(q.proc, 0) != WAIT_TIMEOUT;     // exited by itself
        if (!done && now >= q.deadline)
        {
            if (wt::HasVisibleWindow(GetProcessId(q.proc)))
            {
                Trace(L"quit: spared, a window is still open", q.hwnd);
                DWORD pid = 0;
                if (q.fromHide && IsWindow(q.hwnd) && (GetWindowThreadProcessId(q.hwnd, &pid), pid == q.windowPid))
                    m_putAway.push_back(q.hwnd);
            }
            else
            {
                Trace(L"quit: ended", q.hwnd);
                wt::KillProcess(q.proc);
            }
            done = true;
        }
        if (done)
        {
            CloseHandle(q.proc);
            finished = true;
        }
        else
            m_quitting.push_back(q);
    }
    // Spared windows get their card once nothing is animating.
    if (!m_putAway.empty() && !m_busy && !m_dragging)
    {
        auto putAway = std::move(m_putAway);
        m_putAway.clear();
        for (HWND h : putAway)
            PutAwayHidden(h);
    }
    if (!m_quitting.empty() || !m_putAway.empty())
        SetTimer(m_sidebar, kTimerQuit, kQuitPollMs, nullptr);
    if (finished)
        SetTimer(m_sidebar, kTimerSweep, 300, nullptr);     // cards of windows gone while hidden
}

bool Stage::QuitsOnClose(HWND hwnd) const
{
    if (m_cfg.quitApps.empty())
        return false;
    auto app = wt::AppId(hwnd);
    return !app.empty() && std::any_of(m_cfg.quitApps.begin(), m_cfg.quitApps.end(), [&](auto& e) { return wt::SameApp(e, app); });
}

// A stage window the app hid (closed to the tray): kept as a card it can be brought back from.
void Stage::PutAwayHidden(HWND h)
{
    if (!m_cfg.keepTray || !IsWindow(h) || IsWindowVisible(h) || OnStage(h) ||
        std::any_of(m_cards.begin(), m_cards.end(), [h](auto& c) { return c->hwnd == h; }))
        return;
    m_stage.push_back(h);                           // back where it was, then:
    OnMinimizeStart(h);                             // put away like a minimize: into the sidebar
}

wuc::CompositionSurfaceBrush Stage::PinBrush()
{
    if (m_pinBrush)
        return m_pinBrush;
    float size = std::round(S(kPinBadge));
    winrt::com_ptr<IDWriteTextFormat> glyph;
    if (FAILED(m_snap.Text()->CreateTextFormat(L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * 0.55f, L"", glyph.put())))
        m_snap.Text()->CreateTextFormat(L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * 0.55f, L"", glyph.put());
    glyph->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    glyph->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    auto surface = m_snap.Paint(size, size, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> disc, ring, ink;
        dc->CreateSolidColorBrush(D2D1::ColorF(0.11f, 0.11f, 0.13f, 0.92f), disc.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.35f), ring.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f), ink.put());
        D2D1_ELLIPSE e{ { size / 2, size / 2 }, size / 2 - 1, size / 2 - 1 };
        dc->FillEllipse(e, disc.get());
        dc->DrawEllipse(e, ring.get(), 1.f);
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        dc->DrawTextW(L"\xE718", 1, glyph.get(), { 0, 0, size, size }, ink.get());
    });
    m_pinBrush = m_compositor.CreateSurfaceBrush(surface);
    return m_pinBrush;
}

wuc::CompositionSurfaceBrush Stage::SoundBrush(bool muted)
{
    auto& brush = m_soundBrush[muted ? 1 : 0];
    if (brush)
        return brush;
    float size = std::round(S(kPinBadge));
    winrt::com_ptr<IDWriteTextFormat> glyph;
    if (FAILED(m_snap.Text()->CreateTextFormat(L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * 0.55f, L"", glyph.put())))
        m_snap.Text()->CreateTextFormat(L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * 0.55f, L"", glyph.put());
    glyph->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    glyph->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    auto surface = m_snap.Paint(size, size, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> disc, ring, ink;
        dc->CreateSolidColorBrush(D2D1::ColorF(0.11f, 0.11f, 0.13f, 0.92f), disc.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.35f), ring.put());
        dc->CreateSolidColorBrush(muted ? D2D1::ColorF(1.f, 0.42f, 0.38f) : D2D1::ColorF(1.f, 1.f, 1.f), ink.put());
        D2D1_ELLIPSE e{ { size / 2, size / 2 }, size / 2 - 1, size / 2 - 1 };
        dc->FillEllipse(e, disc.get());
        dc->DrawEllipse(e, ring.get(), 1.f);
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        dc->DrawTextW(muted ? L"\xE74F" : L"\xE767", 1, glyph.get(), { 0, 0, size, size }, ink.get());
    });
    brush = m_compositor.CreateSurfaceBrush(surface);
    return brush;
}

wuc::CompositionSurfaceBrush Stage::AlertBrush()
{
    if (m_alertBrush)
        return m_alertBrush;
    float size = std::round(S(kAlertDot));
    auto surface = m_snap.Paint(size, size, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> dot, ring;
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 0.58f, 0.f), dot.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 0.45f), ring.put());
        D2D1_ELLIPSE e{ { size / 2, size / 2 }, size / 2 - 1, size / 2 - 1 };
        dc->FillEllipse(e, dot.get());
        dc->DrawEllipse(e, ring.get(), 1.f);
    });
    m_alertBrush = m_compositor.CreateSurfaceBrush(surface);
    return m_alertBrush;
}

void Stage::UpdateBadges(Card& c)
{
    auto& v = c.side;
    if (!v.alert)
        return;
    bool attention = m_cfg.alerts && c.alert;
    v.alert.IsVisible(attention);
    bool looping = std::any_of(m_traces.begin(), m_traces.end(), [&](auto& t) { return !t.hover && t.parent == v.sprite; });
    if (!attention && looping)
        StopAlertTrace(v.sprite);
    v.sound.IsVisible(m_cfg.sounds && c.sound != 0);
    if (c.sound)
        v.sound.Brush(SoundBrush(c.sound == 2));
    auto it = std::find_if(m_visible.begin(), m_visible.end(), [&](auto& o) { return o.get() == &c; });
    if (it == m_visible.end())
        return;
    int i = static_cast<int>(it - m_visible.begin());
    Pose p = HoverPose(c, i, i == m_hover);
    float2 a = AlertOffset(c, p), o = SoundOffset(c, p);
    v.alert.Offset({ a.x, a.y, 0.f });
    v.sound.StopAnimation(L"Offset");
    v.sound.Offset({ o.x, o.y, 0.f });
    if (attention && !looping)
        RunTrace(c, false);                         // an orange line keeps going round until it is looked at
}

// A window flashing its taskbar button (a new message...): its card gets the dot and, now and then,
// an orange run of the outline line.
void Stage::OnFlash(HWND hwnd)
{
    if (!m_cfg.alerts || !hwnd)
        return;
    if (HWND owner = wt::ModalOwner(hwnd))
        hwnd = owner;
    if (hwnd == GetForegroundWindow())
        return;
    for (auto& c : m_cards)
    {
        if (c->hwnd != hwnd)
            continue;
        Trace(L"flash", hwnd);
        c->alert = true;
        UpdateBadges(*c);                           // the dot, and the orange loop
        return;
    }
}

// Looking at any window of the app counts as having seen its alert: the window that flashed is often
// not the one read (KakaoTalk flashes its main window, the message is read in a chat window).
void Stage::ClearAlert(HWND hwnd)
{
    DWORD pid = 0;
    if (!hwnd || !GetWindowThreadProcessId(hwnd, &pid))
        return;
    // Cards in the sidebar and parked ones (their window is on stage right now, the card comes back
    // when it leaves): both.
    std::vector<std::shared_ptr<Card>> all(m_cards);
    all.insert(all.end(), m_offstage.begin(), m_offstage.end());
    for (auto& c : all)
    {
        if (!c->alert)
            continue;
        DWORD other = 0;
        if (c->hwnd == hwnd || (GetWindowThreadProcessId(c->hwnd, &other) && other == pid))
        {
            Trace(L"alert seen", c->hwnd);
            c->alert = false;
            UpdateBadges(*c);
        }
    }
}

void Stage::OnAudioChanged()
{
    m_playing = m_cfg.sounds ? m_audio.Current() : std::vector<Audio::Playing>{};
    m_media = m_cfg.sounds ? m_audio.CurrentMedia() : std::vector<Audio::Media>{};
    if (g_trace)
        for (auto& m : m_media)
            Trace((L"media: " + m.model + L" | " + m.title).c_str());
    if (g_trace)
        for (auto& p : m_playing)
            Trace((std::wstring(p.muted ? L"audio (muted): " : L"audio: ") + p.app.substr(p.app.find_last_of(L'\\') + 1)).c_str());
    for (auto& c : m_cards)
    {
        c->sound = SoundOf(*c);
        UpdateBadges(*c);
    }
}

// Which card a sound belongs to. An app with one card: that card. A browser plays for all of its
// windows and web apps through one process, so its media sessions decide: a web app card when its
// own id is playing, a browser window when the title of what plays is in its title (its current tab).
// When nothing tells, only the app's most recent card shows it.
int Stage::SoundOf(Card const& c) const
{
    auto playing = std::find_if(m_playing.begin(), m_playing.end(), [&](auto& p) { return wt::IsCardApp(c.app, p.app, p.model); });
    if (playing == m_playing.end())
        return 0;
    int sound = playing->muted ? 2 : 1;
    auto exe = c.app.substr(0, c.app.find(L'#'));
    auto sameApp = [&](Card const& o) { return wt::SameApp(o.app.substr(0, o.app.find(L'#')), exe); };
    if (std::count_if(m_cards.begin(), m_cards.end(), [&](auto& o) { return sameApp(*o); }) <= 1)
        return sound;
    // Media sessions of this app: their id starts with its executable's name ("chrome.exe" ->
    // "Chrome", "Chrome._crx_..."), or is its UWP model id.
    auto name = exe.substr(exe.find_last_of(L'\\') + 1);
    name = name.substr(0, name.find_last_of(L'.'));
    auto related = [&](Audio::Media const& m) {
        return (!name.empty() && m.model.size() >= name.size() && _wcsnicmp(m.model.c_str(), name.c_str(), name.size()) == 0) ||
               (c.app.rfind(L"uwp:", 0) == 0 && _wcsicmp(m.model.c_str(), c.app.c_str() + 4) == 0);
    };
    bool anyRelated = false;
    auto hash = c.app.find(L'#');
    wchar_t title[256]{};
    GetWindowTextW(c.hwnd, title, ARRAYSIZE(title));
    for (auto& m : m_media)
    {
        if (!related(m))
            continue;
        anyRelated = true;
        if (hash != std::wstring::npos)
        {
            // The web app's id, "_crx_<id>", without the profile suffix a window's id may carry.
            auto crx = [](std::wstring const& id) {
                auto at = id.find(L"_crx_");
                return at == std::wstring::npos ? std::wstring() : id.substr(at, id.find(L'.', at) - at);
            };
            auto mine = crx(c.app.substr(hash + 1));
            if (!mine.empty() && _wcsicmp(crx(m.model).c_str(), mine.c_str()) == 0)
                return sound;                       // this web app itself
        }
        else if (m.model.find(L"_crx_") == std::wstring::npos && !m.title.empty() && wcsstr(title, m.title.c_str()))
            return sound;                           // the tab in front of this window
    }
    if (anyRelated)
        return 0;
    auto first = std::find_if(m_cards.begin(), m_cards.end(), [&](auto& o) { return sameApp(*o); });
    return first != m_cards.end() && first->get() == &c ? sound : 0;
}

int Stage::SoundAt(POINT pt) const
{
    auto hit = [&](int i) {
        auto& c = *m_visible[i];
        if (!c.sound || !m_cfg.sounds)
            return false;
        Pose p = HoverPose(c, i, i == m_hover);
        float2 o = p.center + SoundOffset(c, p);
        float pad = S(3), size = S(kPinBadge);
        return pt.x >= o.x - pad && pt.x <= o.x + size + pad && pt.y >= o.y - pad && pt.y <= o.y + size + pad;
    };
    if (m_hover >= 0 && m_hover < static_cast<int>(m_visible.size()) && hit(m_hover))
        return m_hover;
    for (int i = 0; i < static_cast<int>(m_visible.size()); ++i)
        if (i != m_hover && hit(i))
            return i;
    return -1;
}

void Stage::RestoreTouchpadGesture()
{
    DWORD saved = 0, size = sizeof(saved);
    if (RegGetValueW(HKEY_CURRENT_USER, kStateKey, L"ThreeFingerSlide", RRF_RT_REG_DWORD, nullptr, &saved, &size) != ERROR_SUCCESS)
        return;
    if (saved == 0xFFFFFFFF)                        // it was not set: the system default
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kTouchpadKey, L"ThreeFingerSlideEnabled");
    else
        RegSetKeyValueW(HKEY_CURRENT_USER, kTouchpadKey, L"ThreeFingerSlideEnabled", REG_DWORD, &saved, sizeof(saved));
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kStateKey, L"ThreeFingerSlide");
    // The touchpad stack reads its gesture settings again when its parameters are set.
    TOUCHPAD_PARAMETERS_V1 tp{};
    tp.versionNumber = TOUCHPAD_PARAMETERS_VERSION_1;
    if (SystemParametersInfoW(SPI_GETTOUCHPADPARAMETERS, sizeof(tp), &tp, 0))
        SystemParametersInfoW(SPI_SETTOUCHPADPARAMETERS, sizeof(tp), &tp, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    Trace(L"touchpad: three-finger swipe setting restored");
}

void Stage::FitToSaved(HWND hwnd)
{
    if (!IsWindow(hwnd))
        return;
    // This window's own invisible borders (measured now if it is on screen, otherwise as its card
    // last saw them), so that its visible frame is what lands on the saved one.
    RECT border{};
    if (IsWindowVisible(hwnd) && !IsIconic(hwnd) && !IsZoomed(hwnd))
    {
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        RECT visible = wt::FrameRect(hwnd);
        border = { visible.left - wr.left, visible.top - wr.top, wr.right - visible.right, wr.bottom - visible.bottom };
    }
    else if (auto seen = std::find_if(m_borders.begin(), m_borders.end(), [&](auto& b) { return b.first == hwnd; }); seen != m_borders.end())
        border = seen->second;
    else if (auto c = std::find_if(m_cards.begin(), m_cards.end(), [&](auto& o) { return o->hwnd == hwnd; });
             c != m_cards.end() && ((*c)->border.left || (*c)->border.right || (*c)->border.bottom))
        border = (*c)->border;
    else if (GetWindowLongW(hwnd, GWL_STYLE) & (WS_THICKFRAME | WS_CAPTION))
    {
        // Never seen on screen: a framed window's invisible border is the system's resize border less
        // the 1px that shows, resizable or not (none at the top, where the title bar is).
        UINT dpi = GetDpiForWindow(hwnd);
        int padded = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        int side = std::max(0, GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + padded - 1);
        int bottom = std::max(0, GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + padded - 1);
        border = { side, 0, side, bottom };
    }
    RECT frame = kFitFrame;
    RECT target{ frame.left - border.left, frame.top - border.top, frame.right + border.right, frame.bottom + border.bottom };

    // Placement is in workspace coordinates of the monitor the rectangle is on. The window keeps its
    // state: a card stays minimized (and opens there), a hidden one stays hidden, a maximized one is
    // restored to it.
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST), &mi);
    WINDOWPLACEMENT wp{ sizeof(wp) };
    GetWindowPlacement(hwnd, &wp);
    wp.rcNormalPosition = target;
    OffsetRect(&wp.rcNormalPosition, mi.rcMonitor.left - mi.rcWork.left, mi.rcMonitor.top - mi.rcWork.top);
    wp.flags &= ~WPF_RESTORETOMAXIMIZED;
    if (!IsWindowVisible(hwnd))
        wp.showCmd = SW_HIDE;
    else if (IsIconic(hwnd))
        wp.showCmd = SW_SHOWMINNOACTIVE;
    else
        wp.showCmd = SW_SHOWNOACTIVATE;
    SetWindowPlacement(hwnd, &wp);
    if (g_trace)
    {
        wchar_t what[96];
        swprintf_s(what, L"fit to the saved size (border %ld %ld %ld %ld)", border.left, border.top, border.right, border.bottom);
        Trace(what, hwnd);
    }
    // The card flies from/to the new place from now on.
    if (auto c = std::find_if(m_cards.begin(), m_cards.end(), [&](auto& o) { return o->hwnd == hwnd; }); c != m_cards.end())
        (*c)->frame = frame;
}

// Dragging a pinned card onto another pinned one puts it there; the order is the saved pin list's.
void Stage::MovePin(std::wstring const& from, std::wstring const& to)
{
    auto find = [&](std::wstring const& e) {
        return std::find_if(m_pinnedApps.begin(), m_pinnedApps.end(), [&](auto& x) { return _wcsicmp(x.c_str(), e.c_str()) == 0; });
    };
    auto a = find(from), b = find(to);
    if (a == m_pinnedApps.end() || b == m_pinnedApps.end() || a == b)
        return;
    bool down = a < b;
    auto entry = *a;
    m_pinnedApps.erase(a);
    b = find(to);
    m_pinnedApps.insert(down ? b + 1 : b, entry);
    SavePins();
}

void Stage::SetPinned(Card& c, bool pinned)
{
    Trace(pinned ? L"pinned" : L"unpinned", c.hwnd);
    if (c.pinned == pinned)
        return;
    c.pinned = pinned;
    if (!c.app.empty())
    {
        if (pinned)
        {
            c.desktop = DesktopOf(c.hwnd);
            c.pinKey = PinKey(c);
            m_pinnedApps.push_back(c.pinKey);
        }
        else
        {
            // The entry it was pinned with; the card's desktop may have changed since (moved in Task View).
            auto it = std::find_if(m_pinnedApps.begin(), m_pinnedApps.end(), [&](auto const& e) {
                return c.pinKey.empty() ? PinMatches(e, c) : _wcsicmp(e.c_str(), c.pinKey.c_str()) == 0; });
            if (it == m_pinnedApps.end())
                it = std::find_if(m_pinnedApps.begin(), m_pinnedApps.end(), [&](auto const& e) { return PinMatches(e, c); });
            if (it != m_pinnedApps.end())
                m_pinnedApps.erase(it);
            c.pinKey.clear();
            c.desktop = DesktopOf(c.hwnd);
        }
        SavePins();
    }
    if (c.side.pin)
        c.side.pin.IsVisible(pinned);
    m_hover = -1;
    if (!pinned && OnStage(c.hwnd))
    {
        RemoveCard(c.hwnd);                         // an unpinned window on stage has no sidebar card
        return;
    }
    Relayout(true);
}

// ---- layout ---------------------------------------------------------------

float Stage::BarW() const { return S(kSidebarW) * m_cfg.SizeFactor(); }
float Stage::ThumbW() const { return S(kThumbW) * m_cfg.SizeFactor(); }
float Stage::ThumbH() const { return S(kThumbH) * m_cfg.SizeFactor(); }
// Cards turn to face the stage: away from the left edge, or from the right one.
float Stage::TiltAngle() const { return Right() ? -static_cast<float>(m_cfg.tilt) : static_cast<float>(m_cfg.tilt); }
// Stand-ins are drawn at twice their size for quality, except in icons-only mode, which is about
// using as little memory as possible.
float Stage::PlaceholderScale() const { return m_cfg.iconsOnly ? 1.f : kPlaceholderScale; }

// The monitor chosen in the settings (by device name), or the primary one.
HMONITOR Stage::ChosenMonitor() const
{
    if (!m_cfg.monitor.empty())
    {
        struct Ctx { std::wstring const* name; HMONITOR found; } ctx{ &m_cfg.monitor, nullptr };
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto c = reinterpret_cast<Ctx*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi) && _wcsicmp(mi.szDevice, c->name->c_str()) == 0)
            {
                c->found = mon;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.found)
            return ctx.found;
    }
    return MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
}

// A window snapped (or placed) against the screen edge the sidebar is on: Win+Left, a snap layout's
// left column... It is moved next to the bar instead of being covered by it. Maximized windows are
// left alone; the sidebar slides away from them.
bool Stage::FitsBeside(HWND hwnd) const
{
    if (!m_cfg.fitSnapped || m_dock == DockState::Hidden || IsZoomed(hwnd) || IsIconic(hwnd) ||
        !IsWindowVisible(hwnd) || wt::IsCloaked(hwnd))
        return false;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(m_mon, &mi);
    RECT f = wt::FrameRect(hwnd), work = mi.rcWork;
    LONG workW = work.right - work.left, tol = static_cast<LONG>(S(10));
    bool atEdge = Right() ? std::abs(f.right - work.right) <= tol && f.left < m_bar.left - tol
                          : std::abs(f.left - work.left) <= tol && f.right > m_bar.right + tol;
    // Narrower than 3/4 of the screen (a half or a third), and room left for it next to the bar.
    return atEdge && f.right - f.left <= workW * 3 / 4 && f.right - f.left > static_cast<LONG>(BarW()) + S(240);
}

void Stage::FitBeside()
{
    for (HWND h : m_stage)
    {
        if (!FitsBeside(h))
            continue;
        RECT wr, f = wt::FrameRect(h);
        GetWindowRect(h, &wr);
        // Only the edge against the sidebar moves; the other stays where the snap put it.
        if (Right())
            wr.right -= f.right - m_bar.left;
        else
            wr.left += m_bar.right - f.left;
        Trace(L"fit next to the sidebar", h);
        SetWindowPos(h, nullptr, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
    UpdateDock();
}

// Whether a window coming to the front sits beside everything already on stage (overlapping little),
// as two snapped windows do. Then it joins the stage instead of sending the others away.
bool Stage::SideBySide(HWND hwnd) const
{
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
        return false;
    RECT a = wt::FrameRect(hwnd);
    LONG areaA = (a.right - a.left) * (a.bottom - a.top);
    bool any = false;
    for (HWND h : m_stage)
    {
        if (h == hwnd || !IsWindowVisible(h) || IsIconic(h) || wt::IsCloaked(h))
            continue;
        RECT b = wt::FrameRect(h), overlap;
        LONG areaB = (b.right - b.left) * (b.bottom - b.top);
        LONG shared = IntersectRect(&overlap, &a, &b) ? (overlap.right - overlap.left) * (overlap.bottom - overlap.top) : 0;
        if (shared * 10 > std::min(areaA, areaB))  // more than 10% covered: one is in front of the other
            return false;
        any = true;
    }
    return any;
}

float2 Stage::SlotCenter(size_t i) const
{
    // The stack is centered vertically in the bar.
    float n = static_cast<float>(m_visible.size());
    float barH = static_cast<float>(m_bar.bottom - m_bar.top);
    // Tighter when many large cards would not fit the bar's height.
    float pitch = std::min(S(kPitch) * m_cfg.SizeFactor(), (barH - S(24)) / std::max(n, 1.f));
    return { BarW() / 2.f, barH / 2.f + (i - (n - 1.f) / 2.f) * pitch };
}

float Stage::ThumbScale(Card const& c) const
{
    return ThumbW() / CropFor(c, true).size.x;
}

// Sidebar cards all share one shape; a window with another aspect ratio is cropped (top-anchored).
Crop Stage::CropFor(Card const& c, bool thumb) const
{
    if (!thumb)
        return { { 0.f, 0.f }, { c.w, c.h } };
    float aspect = kThumbW / kThumbH;
    float cw = c.w, ch = c.h;
    if (cw / ch > aspect)
        cw = ch * aspect;
    else
        ch = cw / aspect;
    return { { (c.w - cw) / 2.f, 0.f }, { cw, ch } };
}

// One camera for the whole sidebar, looking at its center (like macOS): the cards' edges then run
// parallel instead of each card fanning out around its own center.
// Where the app icon goes, relative to the card's center: on the card's bottom-left corner as it
// actually appears after the tilt and the shared camera, so every slot gets the same look.
float2 Stage::Project(Pose const& p, float2 local, float z) const
{
    float2 eye{ BarW() / 2.f, (m_bar.bottom - m_bar.top) / 2.f };
    float2 point = p.center + local;
    return eye + (point - eye) / (1.f - z / (kDepthRatio * ThumbW())) - p.center;
}

float2 Stage::BadgeOffset(Card const& c, Pose const& p) const
{
    Crop k = CropFor(c, p.thumb);
    float halfW = k.size.x * p.scale / 2.f, halfH = k.size.y * p.scale / 2.f;
    float rad = p.angle * 3.14159265f / 180.f;
    // The left edge swings toward the viewer.
    float2 corner = Project(p, { -halfW * std::cos(rad), halfH }, halfW * std::sin(rad));
    float2 offset = corner + float2{ S(12), -S(8) } - float2{ S(kBadge), S(kBadge) } / 2.f;
    offset.x = std::max(offset.x, -p.center.x + S(2));               // never past the bar's left edge
    if (Right())
        offset.x = std::min(offset.x, BarW() - p.center.x - S(kBadge) - S(2));
    return offset;
}

// The top-left corner as it appears after the tilt: the attention dot, then the speaker beside it.
float2 Stage::AlertOffset(Card const& c, Pose const& p) const
{
    Crop k = CropFor(c, p.thumb);
    float halfW = k.size.x * p.scale / 2.f, halfH = k.size.y * p.scale / 2.f;
    float rad = p.angle * 3.14159265f / 180.f;
    float2 corner = Project(p, { -halfW * std::cos(rad), -halfH }, halfW * std::sin(rad));
    return corner + float2{ S(13), S(13) } - float2{ S(kAlertDot), S(kAlertDot) } / 2.f;
}

float2 Stage::SoundOffset(Card const& c, Pose const& p) const
{
    Crop k = CropFor(c, p.thumb);
    float halfW = k.size.x * p.scale / 2.f, halfH = k.size.y * p.scale / 2.f;
    float rad = p.angle * 3.14159265f / 180.f;
    float2 corner = Project(p, { -halfW * std::cos(rad), -halfH }, halfW * std::sin(rad));
    float x = (c.alert && m_cfg.alerts) ? S(33) : S(16);
    return corner + float2{ x, S(14) } - float2{ S(kPinBadge), S(kPinBadge) } / 2.f;
}

float2 Stage::PinOffset(Card const& c, Pose const& p) const
{
    Crop k = CropFor(c, p.thumb);
    float halfW = k.size.x * p.scale / 2.f, halfH = k.size.y * p.scale / 2.f;
    float rad = p.angle * 3.14159265f / 180.f;
    float2 corner = Project(p, { halfW * std::cos(rad), -halfH }, -halfW * std::sin(rad));
    return corner + float2{ -S(16), S(14) } - float2{ S(kPinBadge), S(kPinBadge) } / 2.f;
}

float4x4 Stage::Perspective(float2 eye) const
{
    using namespace winrt::Windows::Foundation::Numerics;
    auto p = float4x4::identity();
    p.m34 = -1.f / (kDepthRatio * ThumbW());
    return make_float4x4_translation(-eye.x, -eye.y, 0.f) * p * make_float4x4_translation(eye.x, eye.y, 0.f);
}

Pose Stage::SlotPose(Card const& c, size_t i) const
{
    return { SlotCenter(i), ThumbScale(c), TiltAngle(), true };
}

// ---- cards ----------------------------------------------------------------

void Stage::Populate()
{
    auto windows = wt::EnumManageable(m_mon);
    if (windows.empty())
        return;
    HWND fg = GetForegroundWindow();
    m_active = std::find(windows.begin(), windows.end(), fg) != windows.end() ? fg : windows.front();
    m_stage = { m_active };

    for (HWND h : windows)
    {
        if (h != m_active)
            Adopt(h);
    }
    AdoptPinnedElsewhere();
    Relayout(false);
}

// ---- outside changes -------------------------------------------------------

void CALLBACK Stage::WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD a, DWORD b)
{
    if (!g_stage || idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd)
        return;
    try
    {
        g_stage->OnWinEvent(event, hwnd);
    }
    catch (winrt::hresult_error const& e)
    {
        LogError(event, e.code(), e.message().c_str());
    }
    catch (...)
    {
        LogError(event, E_FAIL, L"unknown exception");
    }
}

void Stage::OnWinEvent(DWORD event, HWND hwnd)
{
    auto g_stage = this;
    switch (event)
    {
    case EVENT_SYSTEM_FOREGROUND:
        g_stage->OnForeground(hwnd);
        break;
    case EVENT_SYSTEM_MINIMIZESTART:
        g_stage->OnMinimizeStart(hwnd);
        break;
    case EVENT_OBJECT_LOCATIONCHANGE:
        if (g_stage->OnStage(hwnd))
        {
            g_stage->UpdateDock();
            if (m_cfg.fitSnapped && FitsBeside(hwnd))
                SetTimer(m_sidebar, kTimerFit, 250, nullptr);    // once the snap has settled
        }
        break;
    case EVENT_OBJECT_HIDE:
        if (g_stage->OnStage(hwnd))
            g_stage->OnHidden(hwnd);
        break;
    case EVENT_OBJECT_CLOAKED:
    case EVENT_OBJECT_UNCLOAKED:
        if (wt::IsInputPanel(hwnd))
        {
            std::erase(m_inputPanels, hwnd);
            if (event == EVENT_OBJECT_UNCLOAKED)
                m_inputPanels.push_back(hwnd);
            Trace(event == EVENT_OBJECT_UNCLOAKED ? L"input panel shown" : L"input panel hidden", hwnd);
            // Re-applies the z-order; a grown view (hover, menu, animation) is topmost anyway and gets
            // it right when it shrinks.
            if (!m_busy && !m_dragging && m_hover < 0)
                ShrinkView();
        }
        break;
    }
}

void Stage::OnForeground(HWND hwnd)
{
    Trace(L"foreground", hwnd);
    // A dialog stands in for the window it blocks (a file picker in front of its chat window).
    if (HWND owner = wt::ModalOwner(hwnd); owner && (OnStage(owner) || !IsIconic(owner)))
        hwnd = owner;
    ClearAlert(hwnd);
    CloseCardMenu();
    // Another window took focus (Alt+Tab...): the settings panel steps aside, like a menu.
    // (Focus handed back to the stage window, as after the tray menu, doesn't count.)
    if (m_settings.open && hwnd != m_active)
        CloseSettings();
    if (!m_ready || m_busy || m_syncing)
        return;
    GUID desktop = CurrentDesktopId();
    if (desktop != m_desktopId)
    {
        m_desktopId = desktop;
        SyncDesktop();
        return;
    }
    if (hwnd == m_active || GetTickCount64() < m_quietUntil)
    {
        UpdateDock();
        return;
    }
    if (OnStage(hwnd))
    {
        m_stage.erase(std::find(m_stage.begin(), m_stage.end(), hwnd));
        m_stage.push_back(hwnd);
        m_active = hwnd;
        SnapActiveSoon();
        UpdateDock();
        return;
    }
    if (!wt::IsManageable(hwnd, m_mon))
    {
        UpdateDock();                               // e.g. a fullscreen game came to the front
        // An app window still being set up (not shown or untitled yet) becomes manageable in a moment;
        // look again soon instead of waiting for the next focus change. Not the taskbar, the desktop,
        // tool windows or our own.
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (IsWindow(hwnd) && !GetWindow(hwnd, GW_OWNER) && (!IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd) == 0) &&
            !wt::IsShellWindow(hwnd) && pid != GetCurrentProcessId() && !(GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
        {
            if (hwnd != m_newWindow)
            {
                m_newWindow = hwnd;
                m_rechecks = kMaxRechecks;
            }
            SetTimer(m_sidebar, kTimerNewWindow, kRecheckMs, nullptr);
        }
        return;
    }
    m_newWindow = nullptr;
    if (SideBySide(hwnd))
    {
        // Next to what is on stage (two windows snapped side by side): both stay.
        Trace(L"joins stage side by side", hwnd);
        RemoveCard(hwnd);
        m_stage.push_back(hwnd);
        m_active = hwnd;
        SnapActiveSoon();
        StageChanged();
        if (m_cfg.fitSnapped && FitsBeside(hwnd))
            SetTimer(m_sidebar, kTimerFit, 250, nullptr);
        return;
    }
    // Already on screen (Alt+Tab, taskbar, a new window): only the previous one needs to leave.
    RemoveCard(hwnd);
    BeginTransition(hwnd, nullptr, {});
}

void Stage::OnMinimizeStart(HWND hwnd)
{
    if (m_ready)
        SetTimer(m_sidebar, kTimerDock, 100, nullptr);     // e.g. a fullscreen game minimized by Alt+Tab
    if (!m_ready || !OnStage(hwnd))
        return;
    // Minimized by the user (button, Win+D, four-finger swipe). It can't be photographed any more, so
    // use the picture taken when it became the focused stage window, or the one from a sidebar hover.
    wuc::CompositionDrawingSurface picture{ nullptr };
    RECT frame{};
    if (m_activeSnap && m_activeSnapHwnd == hwnd)
    {
        picture = m_activeSnap;
        frame = m_activeSnapFrame;
        m_activeSnap = nullptr;
    }
    else if (m_prefetch && m_prefetchHwnd == hwnd)
    {
        picture = m_prefetch;
        frame = m_prefetchFrame;
        m_prefetch = nullptr;
    }
    Trace(picture ? L"minimize (has snap)" : L"minimize (no snap)", hwnd);

    LeaveStage(hwnd);
    auto card = TakeCard(hwnd);
    if (!card->pinned)
        m_cards.insert(m_cards.begin(), card);
    bool standIn = false;
    if (!picture && m_cfg.iconsOnly)
    {
        // No pictures: its icon card flies in instead.
        frame = card->frame;
        picture = MakePlaceholder(*card);
        standIn = true;
    }
    if (picture)
        SetSnapshot(*card, frame, picture, standIn);
    Relayout(true);
    StageChanged();

    // It flies into its slot from where it was, unless something else is animating, or its card is
    // out of view (e.g. while Windows is still moving the window between desktops).
    if (!picture || m_busy || !card->side.holder)
        return;
    m_busy = true;
    SetTimer(m_sidebar, kTimerWatchdog, kWatchdogMs, nullptr);
    m_inCard = nullptr;
    auto flight = std::make_shared<OutFlight>();
    flight->card = card;
    m_outs = { flight };
    m_pending = 1;
    card->side.holder.Opacity(0.f);
    GrowView();
    OnOutCaptured(flight, frame, picture);
}

void Stage::SnapActiveSoon()
{
    m_activeSnap = nullptr;
    KillTimer(m_sidebar, kTimerActiveSnap);
    if (m_active)
        SetTimer(m_sidebar, kTimerActiveSnap, kActiveSnapDelayMs, nullptr);
}

// The shell reports a window "destroyed" when it leaves the taskbar, which also happens when an app
// merely hides it (closing to the tray). Only a window that really no longer exists loses its card.
void Stage::OnGone(HWND hwnd)
{
    Trace(IsWindow(hwnd) ? L"gone (hidden)" : L"gone (destroyed)", hwnd);
    // A fullscreen app closing may bring no focus change with it (e.g. focus already on a pet window).
    SetTimer(m_sidebar, kTimerDock, 100, nullptr);
    if (IsWindow(hwnd))
    {
        if (!IsWindowVisible(hwnd) && OnStage(hwnd))
            OnHidden(hwnd);
        // Many apps hide a window just before destroying it; the second notice never comes, since it
        // already left the taskbar. Check again shortly.
        SetTimer(m_sidebar, kTimerSweep, 300, nullptr);
        return;
    }
    if (m_menu.open && m_menu.card && m_menu.card->hwnd == hwnd)
        CloseCardMenu();
    LeaveStage(hwnd);
    if (!m_busy)
        RemoveCard(hwnd, true);
}

// A stage window was hidden: closed to the tray (KakaoTalk, Discord: kept as a card, it can be
// brought back), or about to be destroyed (a KakaoTalk chat closed with Esc: hidden first, destroyed
// right after). Decided a moment later, so a closed window doesn't fly into the sidebar first.
void Stage::OnHidden(HWND hwnd)
{
    if (std::find(m_hidden.begin(), m_hidden.end(), hwnd) == m_hidden.end())
        m_hidden.push_back(hwnd);
    // Off the stage meanwhile, or a focus change in between would fly it out as a leaving window.
    LeaveStage(hwnd);
    SetTimer(m_sidebar, kTimerHidden, kHiddenMs, nullptr);
}

bool Stage::OnStage(HWND hwnd) const
{
    return hwnd && std::find(m_stage.begin(), m_stage.end(), hwnd) != m_stage.end();
}

void Stage::LeaveStage(HWND hwnd)
{
    std::erase(m_stage, hwnd);
    if (hwnd == m_active)
        m_active = m_stage.empty() ? nullptr : m_stage.back();
}

GUID Stage::DesktopOf(HWND hwnd) const
{
    GUID id{};
    if (m_desktops)
        m_desktops->GetWindowDesktopId(hwnd, &id);
    return id;
}

// Card desktops are cached so layout never has to ask Explorer; they are refreshed when the desktop
// changes (windows may have been moved between desktops in Task View meanwhile).
// DWM cloaks windows that live on other desktops. Reading that attribute is a cheap local call and,
// unlike a cached desktop id, follows windows moved between desktops in Task View.
bool Stage::Here(Card const& c) const
{
    return !wt::IsCloaked(c.hwnd);
}

// A pin is remembered as "{desktop}|executable": it belongs to that app's window on that desktop only.
// Entries without a desktop (saved by older versions) match the app anywhere.
std::wstring Stage::PinKey(Card const& c)
{
    wchar_t guid[64]{};
    StringFromGUID2(c.desktop, guid, ARRAYSIZE(guid));
    return std::wstring(guid) + L"|" + c.app;
}

bool Stage::PinMatches(std::wstring const& entry, Card const& c)
{
    auto bar = entry.find(L'|');
    if (bar == std::wstring::npos)
        return wt::SameApp(entry, c.app);
    auto key = PinKey(c);
    return _wcsnicmp(entry.c_str(), key.c_str(), bar + 1) == 0 && wt::SameApp(entry.substr(bar + 1), c.app);
}

// Pinned apps whose windows live on other desktops still get their (pinned) cards.
void Stage::AdoptPinnedElsewhere()
{
    if (m_pinnedApps.empty())
        return;
    for (HWND h : wt::EnumManageable(m_mon, true))
    {
        if (!wt::IsCloaked(h) || OnStage(h) || std::any_of(m_cards.begin(), m_cards.end(), [&](auto& c) { return c->hwnd == h; }))
            continue;
        auto app = wt::AppId(h);
        if (app.empty() || std::none_of(m_pinnedApps.begin(), m_pinnedApps.end(),
                [&](auto& e) { return wt::SameApp(e.substr(e.find(L'|') + 1), app); }))
            continue;
        auto card = MakeCard(h);
        if (!card->pinned)
            continue;                               // not pinned on its desktop, or taken by another window
        m_cards.push_back(card);
        if (m_cfg.iconsOnly)
            continue;
        m_snap.CaptureMinimized(h, [this, card](auto const& surface) {
            if (surface)
                SetSnapshot(*card, card->frame, surface);
            TrimMemory();
        });
    }
}

// Explorer keeps the current desktop in the registry: under Explorer\VirtualDesktops on Windows 11,
// under the session's key on Windows 10. Reading it is far cheaper than asking Explorer.
GUID Stage::CurrentDesktopId()
{
    GUID id{};
    if (vd::Current(&id))
        return id;
    DWORD size = sizeof(id);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops",
            L"CurrentVirtualDesktop", RRF_RT_REG_BINARY, nullptr, &id, &size) == ERROR_SUCCESS)
        return id;
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    wchar_t key[160];
    swprintf_s(key, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SessionInfo\\%lu\\VirtualDesktops", session);
    size = sizeof(id);
    RegGetValueW(HKEY_CURRENT_USER, key, L"CurrentVirtualDesktop", RRF_RT_REG_BINARY, nullptr, &id, &size);
    return id;
}

// After switching desktops the previous stage window stays put on its own desktop; the new desktop's
// windows take over the sidebar, and ones never seen before are adopted.
void Stage::SyncDesktop()
{
    Trace(L"desktop switched");
    auto cards = m_cards;                           // DesktopOf calls into Explorer, which may pump messages
    m_syncing = true;
    for (auto& c : cards)
        c->desktop = DesktopOf(c->hwnd);
    m_syncing = false;
    HWND fg = GetForegroundWindow();
    m_active = wt::IsManageable(fg, m_mon) ? fg : nullptr;
    m_stage.clear();
    if (m_active)
    {
        m_stage.push_back(m_active);
        RemoveCard(m_active);
    }
    for (HWND h : wt::EnumManageable(m_mon))
    {
        if (!OnStage(h) && std::none_of(m_cards.begin(), m_cards.end(), [&](auto& c) { return c->hwnd == h; }))
            Adopt(h);
    }
    AdoptPinnedElsewhere();
    m_hover = -1;
    Relayout(false);
    SnapActiveSoon();
    StageChanged();
}

// A window seen for the first time on this desktop joins the sidebar like at startup.
void Stage::Adopt(HWND hwnd)
{
    auto card = MakeCard(hwnd);
    m_cards.push_back(card);
    if (m_cfg.iconsOnly)
    {
        if (!IsIconic(hwnd))
            Minimize(hwnd);                         // its icon card is ready already
        return;
    }
    if (IsIconic(hwnd))
    {
        m_snap.CaptureMinimized(hwnd, [this, card](auto const& surface) {
            Trace(surface ? L"minimized window photographed via DWM" : L"minimized window: no DWM picture", card->hwnd);
            if (surface)
                SetSnapshot(*card, card->frame, surface);
            TrimMemory();
        });
        return;
    }
    RECT frame = wt::FrameRect(hwnd);
    m_snap.CaptureAsync(hwnd, frame, [this, card, frame](auto const& surface) {
        SetSnapshot(*card, frame, surface);
        TrimMemory();
        if (!OnStage(card->hwnd) && IsWindow(card->hwnd) && IsWindowVisible(card->hwnd))
            Minimize(card->hwnd);
    });
}

// Takes the window's card out of the sidebar: parked if the window went on stage, dropped if it is
// gone for good (`evenIfPinned`: destroyed windows lose their pinned card too).
void Stage::RemoveCard(HWND hwnd, bool evenIfPinned)
{
    if (evenIfPinned)
        std::erase_if(m_offstage, [&](auto& c) { return c->hwnd == hwnd; });
    auto it = std::find_if(m_cards.begin(), m_cards.end(), [&](auto& c) { return c->hwnd == hwnd; });
    if (it == m_cards.end() || ((*it)->pinned && !evenIfPinned))
        return;
    auto card = *it;
    m_cards.erase(it);
    if (evenIfPinned)
    {
        if (card->side.holder)
            m_sideContent.Children().Remove(card->side.holder);
    }
    else
        Park(card);
    m_hover = -1;
    Relayout(true);
}

void Stage::Park(std::shared_ptr<Card> card)
{
    if (card->side.holder)
        m_sideContent.Children().Remove(card->side.holder);
    card->side = {};
    std::erase_if(m_offstage, [&](auto& c) { return c->hwnd == card->hwnd; });
    m_offstage.push_back(card);
    if (m_offstage.size() > 4)                      // only windows currently on stage need one
        m_offstage.erase(m_offstage.begin());
}

std::shared_ptr<Card> Stage::MakeCard(HWND hwnd)
{
    auto c = std::make_shared<Card>();
    c->hwnd = hwnd;
    c->app = wt::AppId(hwnd);
    c->desktop = m_desktopId;                   // cards are only made for windows on the current desktop
    // Pinned if this app on this desktop has more pin entries than pinned cards. The desktop is only
    // asked for (a call into Explorer) when the app has a pin at all.
    if (!c->app.empty() && std::any_of(m_pinnedApps.begin(), m_pinnedApps.end(),
            [&](auto const& e) { return wt::SameApp(e.substr(e.find(L'|') + 1), c->app); }))
    {
        c->desktop = DesktopOf(hwnd);
        auto wanted = std::count_if(m_pinnedApps.begin(), m_pinnedApps.end(), [&](auto const& e) { return PinMatches(e, *c); });
        auto have = std::count_if(m_cards.begin(), m_cards.end(), [&](auto& o) { return o->pinned && o->desktop == c->desktop && wt::SameApp(o->app, c->app); });
        c->pinned = have < wanted;
        // Upgrade an old app-only entry to this window's desktop, so it stops matching elsewhere.
        auto old = std::find_if(m_pinnedApps.begin(), m_pinnedApps.end(),
            [&](auto const& e) { return e.find(L'|') == std::wstring::npos && PinMatches(e, *c); });
        if (c->pinned && old != m_pinnedApps.end() && c->desktop != GUID_NULL)
        {
            *old = PinKey(*c);
            SavePins();
        }
        if (c->pinned)
        {
            auto e = std::find_if(m_pinnedApps.begin(), m_pinnedApps.end(), [&](auto const& e) { return PinMatches(e, *c); });
            if (e != m_pinnedApps.end())
                c->pinKey = *e;
        }
    }
    c->frame = IsIconic(hwnd) ? wt::RestoreRect(hwnd) : wt::FrameRect(hwnd);
    c->w = static_cast<float>(std::max(1L, c->frame.right - c->frame.left));
    c->h = static_cast<float>(std::max(1L, c->frame.bottom - c->frame.top));
    c->snapshot = m_compositor.CreateSurfaceBrush();
    c->snapshot.Stretch(wuc::CompositionStretch::Fill);
    c->icon = m_compositor.CreateSurfaceBrush(m_snap.Icon(hwnd, static_cast<int>(std::lround(S(kBadge)))));
    // Only a window that can't be photographed right now (minimized, or on another desktop) needs a
    // stand-in; a visible one is photographed right away.
    if (m_cfg.iconsOnly || IsIconic(hwnd) || !IsWindowVisible(hwnd) || wt::IsCloaked(hwnd))
    {
        c->snapshot.Surface(MakePlaceholder(*c));
        c->hasPicture = true;
        c->placeholder = true;
    }
    return c;
}

void Stage::SetSnapshot(Card& c, RECT const& frame, wuc::CompositionDrawingSurface const& surface, bool placeholder)
{
    if (!surface)
        return;
    c.placeholder = placeholder;
    if (!IsZoomed(c.hwnd) && !IsIconic(c.hwnd))
    {
        RECT wr;
        if (GetWindowRect(c.hwnd, &wr))
            c.border = { frame.left - wr.left, frame.top - wr.top, wr.right - frame.right, wr.bottom - frame.bottom };
    }
    c.frame = frame;
    c.w = static_cast<float>(frame.right - frame.left);
    c.h = static_cast<float>(frame.bottom - frame.top);
    c.snapshot.Surface(surface);
    c.hasSnapshot = true;
    c.hasPicture = true;
    if (c.side.holder)
        ApplySize(c.side, c);
}

void Stage::ApplySize(CardVis const& v, Card const& c)
{
    v.sprite.Size({ c.w, c.h });
    v.sprite.Brush(c.hasPicture ? wuc::CompositionBrush(c.snapshot) : wuc::CompositionBrush(m_placeholderBrush));
    if (v.glass)
        v.glass.IsVisible(m_cfg.cardStyle == 1 && c.hasPicture && c.placeholder);
}

wuc::CompositionDrawingSurface Stage::MakePlaceholder(Card const& c)
{
    return m_snap.Placeholder(c.hwnd, c.w, c.h, PlaceholderScale() * ThumbW() * c.w / CropFor(c, true).size.x, m_cfg.cardStyle == 1);
}

// The stand-in style changed in Settings: stand-ins are drawn again, glass shown or hidden.
void Stage::RestylePlaceholders()
{
    for (auto& c : m_cards)
    {
        if (!c->hasPicture || !c->placeholder)
            continue;
        c->snapshot.Surface(MakePlaceholder(*c));
        if (c->side.holder)
            ApplySize(c->side, *c);
    }
}

void Stage::SetMinAnimate(bool on)
{
    if (!m_savedMinAnimate)
        return;
    ANIMATIONINFO ai{ sizeof(ai), on ? m_savedMinAnimate : 0 };
    SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, 0);
}

CardVis Stage::MakeVis(Card const& c, bool withBadge)
{
    CardVis v;
    v.holder = m_compositor.CreateContainerVisual();

    v.sprite = m_compositor.CreateSpriteVisual();
    v.sprite.RotationAxis({ 0.f, 1.f, 0.f });
    v.clip = m_compositor.CreateRoundedRectangleGeometry();
    v.sprite.Clip(m_compositor.CreateGeometricClip(v.clip));

    // Glass: the desktop behind the card, blurred, under the see-through stand-in. It copies the
    // sprite's placement and shape through expressions, so every pose and animation carries it along.
    if (!m_backdrop)
    {
        BOOL on = TRUE;
        DwmSetWindowAttribute(m_view, DWMWA_USE_HOSTBACKDROPBRUSH, &on, sizeof(on));
        m_backdrop = m_compositor.CreateHostBackdropBrush();
    }
    v.glass = m_compositor.CreateSpriteVisual();
    v.glass.RotationAxis({ 0.f, 1.f, 0.f });
    v.glass.Brush(m_backdrop);
    auto glassClip = m_compositor.CreateRoundedRectangleGeometry();
    v.glass.Clip(m_compositor.CreateGeometricClip(glassClip));
    auto follow = [&](wuc::CompositionObject const& target, wuc::CompositionObject const& source, wchar_t const* prop) {
        auto e = m_compositor.CreateExpressionAnimation(std::wstring(L"s.") + prop);
        e.SetReferenceParameter(L"s", source);
        target.StartAnimation(prop, e);
    };
    for (auto prop : { L"Offset", L"CenterPoint", L"Scale", L"RotationAngleInDegrees", L"Size" })
        follow(v.glass, v.sprite, prop);
    for (auto prop : { L"Offset", L"Size", L"CornerRadius" })
        follow(glassClip, v.clip, prop);
    v.holder.Children().InsertAtTop(v.glass);

    ApplySize(v, c);
    v.holder.Children().InsertAtTop(v.sprite);

    if (withBadge)
    {
        v.pin = m_compositor.CreateSpriteVisual();
        v.pin.Size({ S(kPinBadge), S(kPinBadge) });
        v.pin.Brush(PinBrush());
        v.pin.IsVisible(c.pinned);
        v.holder.Children().InsertAtTop(v.pin);
    }
    if (withBadge)
    {
        v.alert = m_compositor.CreateSpriteVisual();
        v.alert.Size({ S(kAlertDot), S(kAlertDot) });
        v.alert.Brush(AlertBrush());
        v.alert.IsVisible(false);
        v.holder.Children().InsertAtTop(v.alert);
        v.sound = m_compositor.CreateSpriteVisual();
        v.sound.Size({ S(kPinBadge), S(kPinBadge) });
        v.sound.IsVisible(false);
        v.holder.Children().InsertAtTop(v.sound);
    }
    if (withBadge && c.icon)
    {
        v.badge = m_compositor.CreateSpriteVisual();
        v.badge.Size({ S(kBadge), S(kBadge) });
        v.badge.Brush(c.icon);
        v.holder.Children().InsertAtTop(v.badge);
    }
    return v;
}

// The holder sits at the card's visual center; the sprite is shifted so the visible (cropped) part
// is centered on it, and scales/tilts around that same point.
void Stage::ApplyPose(CardVis const& v, Card const& c, Pose const& p)
{
    Crop k = CropFor(c, p.thumb);
    float2 mid = k.offset + k.size / 2.f;
    v.holder.Offset({ p.center.x, p.center.y, 0.f });
    v.sprite.Offset({ -mid.x, -mid.y, 0.f });
    v.sprite.CenterPoint({ mid.x, mid.y, 0.f });
    v.sprite.Scale({ p.scale, p.scale, 1.f });
    v.sprite.RotationAngleInDegrees(p.angle);
    v.clip.Offset(k.offset);
    v.clip.Size(k.size);
    v.clip.CornerRadius({ S(kRadius) / p.scale, S(kRadius) / p.scale });
    if (v.badge)
    {
        float2 b = BadgeOffset(c, p);
        v.badge.Offset({ b.x, b.y, 0.f });
    }
    if (v.pin)
    {
        float2 pin = PinOffset(c, p);
        v.pin.Offset({ pin.x, pin.y, 0.f });
    }
    if (v.alert)
    {
        float2 a = AlertOffset(c, p), o = SoundOffset(c, p);
        v.alert.Offset({ a.x, a.y, 0.f });
        v.sound.Offset({ o.x, o.y, 0.f });
    }
}

void Stage::AnimatePose(CardVis const& v, Card const& c, Pose const& from, Pose const& to, int ms)
{
    std::chrono::milliseconds dur(ms);
    Crop k0 = CropFor(c, from.thumb), k1 = CropFor(c, to.thumb);
    float2 mid0 = k0.offset + k0.size / 2.f, mid1 = k1.offset + k1.size / 2.f;

    auto vec3 = [&](auto const& target, wchar_t const* prop, float3 a, float3 b) {
        auto anim = m_compositor.CreateVector3KeyFrameAnimation();
        anim.InsertKeyFrame(0.f, a);
        anim.InsertKeyFrame(1.f, b, m_ease);
        anim.Duration(dur);
        target.StartAnimation(prop, anim);
    };
    auto vec2 = [&](auto const& target, wchar_t const* prop, float2 a, float2 b) {
        auto anim = m_compositor.CreateVector2KeyFrameAnimation();
        anim.InsertKeyFrame(0.f, a);
        anim.InsertKeyFrame(1.f, b, m_ease);
        anim.Duration(dur);
        target.StartAnimation(prop, anim);
    };

    vec3(v.holder, L"Offset", { from.center.x, from.center.y, 0.f }, { to.center.x, to.center.y, 0.f });
    vec3(v.sprite, L"Offset", { -mid0.x, -mid0.y, 0.f }, { -mid1.x, -mid1.y, 0.f });
    vec3(v.sprite, L"CenterPoint", { mid0.x, mid0.y, 0.f }, { mid1.x, mid1.y, 0.f });
    vec3(v.sprite, L"Scale", { from.scale, from.scale, 1.f }, { to.scale, to.scale, 1.f });
    vec2(v.clip, L"Offset", k0.offset, k1.offset);
    vec2(v.clip, L"Size", k0.size, k1.size);
    vec2(v.clip, L"CornerRadius", { S(kRadius) / from.scale, S(kRadius) / from.scale },
        { S(kRadius) / to.scale, S(kRadius) / to.scale });

    auto angle = m_compositor.CreateScalarKeyFrameAnimation();
    angle.InsertKeyFrame(0.f, from.angle);
    angle.InsertKeyFrame(1.f, to.angle, m_ease);
    angle.Duration(dur);
    v.sprite.StartAnimation(L"RotationAngleInDegrees", angle);

    if (v.badge)
    {
        float2 b = BadgeOffset(c, to);
        auto badge = m_compositor.CreateVector3KeyFrameAnimation();
        badge.InsertKeyFrame(1.f, { b.x, b.y, 0.f }, m_ease);
        badge.Duration(dur);
        v.badge.StartAnimation(L"Offset", badge);
    }
    if (v.pin)
    {
        float2 pin = PinOffset(c, to);
        auto move = m_compositor.CreateVector3KeyFrameAnimation();
        move.InsertKeyFrame(1.f, { pin.x, pin.y, 0.f }, m_ease);
        move.Duration(dur);
        v.pin.StartAnimation(L"Offset", move);
    }
    if (v.alert)
    {
        for (auto [visual, at] : { std::pair{ v.alert, AlertOffset(c, to) }, std::pair{ v.sound, SoundOffset(c, to) } })
        {
            auto move = m_compositor.CreateVector3KeyFrameAnimation();
            move.InsertKeyFrame(1.f, { at.x, at.y, 0.f }, m_ease);
            move.Duration(dur);
            visual.StartAnimation(L"Offset", move);
        }
    }
}

void Stage::Relayout(bool animate)
{
    auto pinnedEnd = std::stable_partition(m_cards.begin(), m_cards.end(), [](auto& c) { return c->pinned; });
    auto rank = [&](Card const& c) {
        auto it = std::find_if(m_pinnedApps.begin(), m_pinnedApps.end(), [&](auto& e) { return _wcsicmp(e.c_str(), c.pinKey.c_str()) == 0; });
        return it - m_pinnedApps.begin();
    };
    std::stable_sort(m_cards.begin(), pinnedEnd, [&](auto& a, auto& b) { return rank(*a) < rank(*b); });
    m_visible.clear();
    // Every desktop's sidebar shows its 4 most recent cards; a card pushed out of its own desktop's
    // top 4 can never be seen again, so its snapshot is released.
    std::vector<std::pair<GUID, size_t>> perDesktop;
    for (auto& c : m_cards)
    {
        bool here = Here(*c);
        if (here && m_visible.size() < static_cast<size_t>(m_cfg.cards))
        {
            m_visible.push_back(c);
            continue;
        }
        if (c->side.holder)
            c->side.holder.IsVisible(false);
        bool shown = false;
        if (!here)
        {
            auto it = std::find_if(perDesktop.begin(), perDesktop.end(), [&](auto& d) { return d.first == c->desktop; });
            if (it == perDesktop.end())
                it = perDesktop.insert(perDesktop.end(), { c->desktop, 0 });
            shown = it->second++ < static_cast<size_t>(m_cfg.cards);
        }
        if (!shown && c->hasPicture)
        {
            c->snapshot.Surface(nullptr);
            c->hasSnapshot = false;
            c->hasPicture = false;
            if (c->side.holder)
                ApplySize(c->side, *c);
        }
    }
    for (size_t i = 0; i < m_visible.size(); ++i)
    {
        auto& c = *m_visible[i];
        if (!c.hasPicture && (IsIconic(c.hwnd) || m_cfg.iconsOnly))
        {
            // Back in view after its picture was released.
            c.snapshot.Surface(MakePlaceholder(c));
            c.hasPicture = true;
            c.placeholder = true;
            if (c.side.holder)
                ApplySize(c.side, c);
        }
        Pose target = SlotPose(c, i);
        if (m_cfg.sounds)
            c.sound = SoundOf(c);
        if (!c.side.holder)
        {
            c.side = MakeVis(c, true);
            m_sideContent.Children().InsertAtTop(c.side.holder);
            ApplyPose(c.side, c, target);
            UpdateBadges(c);
            continue;
        }
        c.side.holder.IsVisible(true);
        float2 now{ c.side.holder.Offset().x, c.side.holder.Offset().y };
        if (animate && (now.x != target.center.x || now.y != target.center.y))
            AnimatePose(c.side, c, { now, target.scale, target.angle, true }, target, m_cfg.Ms(kSlideMs));
        else
            ApplyPose(c.side, c, target);
    }
}

int Stage::HitTest(POINT pt) const
{
    // The hovered card is checked first: it is enlarged and shifted, and drawn above its neighbours.
    if (m_hover >= 0 && m_hover < static_cast<int>(m_visible.size()))
    {
        Pose p = HoverPose(*m_visible[m_hover], m_hover, true);
        float halfW = ThumbW() * kHoverGrow / 2.f, halfH = ThumbH() * kHoverGrow / 2.f;
        if (std::abs(pt.x - p.center.x) <= halfW && std::abs(pt.y - p.center.y) <= halfH)
            return m_hover;
    }
    for (size_t i = 0; i < m_visible.size(); ++i)
    {
        float2 ctr = SlotCenter(i);
        if (std::abs(pt.x - ctr.x) <= ThumbW() / 2.f && std::abs(pt.y - ctr.y) <= ThumbH() / 2.f)
            return static_cast<int>(i);
    }
    return -1;
}

// The hovered card turns to face the viewer and grows; the previous one tilts back. Only end values
// are given, so a quick sweep across cards continues from wherever each card is instead of jumping.
Pose Stage::HoverPose(Card const& c, size_t i, bool hovered) const
{
    Pose p = SlotPose(c, i);
    if (hovered)
    {
        p.scale *= kHoverGrow;
        p.angle = 0.f;
        // Facing front it is wider than the bar: keep its outer edge on screen.
        float half = CropFor(c, true).size.x * p.scale / 2.f;
        p.center.x = Right() ? std::min(p.center.x, BarW() - half - S(8)) : std::max(p.center.x, half + S(8));
    }
    return p;
}

void Stage::SetHover(int index)
{
    if (index == m_hover)
        return;
    ClearTrace();
    auto settle = [&](int i, bool hovered) {
        if (i < 0 || i >= static_cast<int>(m_visible.size()))
            return;
        auto& c = *m_visible[i];
        auto& v = c.side;
        Pose p = HoverPose(c, i, hovered);
        std::chrono::milliseconds dur(m_cfg.Ms(hovered ? 200 : 170));
        auto vec = [&](auto const& target, wchar_t const* prop, float3 to) {
            auto anim = m_compositor.CreateVector3KeyFrameAnimation();
            anim.InsertKeyFrame(1.f, to, m_ease);
            anim.Duration(dur);
            target.StartAnimation(prop, anim);
        };
        vec(v.sprite, L"Scale", { p.scale, p.scale, 1.f });
        auto angle = m_compositor.CreateScalarKeyFrameAnimation();
        angle.InsertKeyFrame(1.f, p.angle, m_ease);
        angle.Duration(dur);
        v.sprite.StartAnimation(L"RotationAngleInDegrees", angle);
        auto radius = m_compositor.CreateVector2KeyFrameAnimation();
        radius.InsertKeyFrame(1.f, { S(kRadius) / p.scale, S(kRadius) / p.scale }, m_ease);
        radius.Duration(dur);
        v.clip.StartAnimation(L"CornerRadius", radius);
        if (v.badge)
        {
            float2 o = BadgeOffset(c, p);
            vec(v.badge, L"Offset", { o.x, o.y, 0.f });
        }
        if (v.pin)
        {
            float2 o = PinOffset(c, p);
            vec(v.pin, L"Offset", { o.x, o.y, 0.f });
        }
        if (v.alert)
        {
            float2 a = AlertOffset(c, p), o = SoundOffset(c, p);
            vec(v.alert, L"Offset", { a.x, a.y, 0.f });
            vec(v.sound, L"Offset", { o.x, o.y, 0.f });
        }
        if (hovered)
        {
            // In front of its neighbours while it is larger than its slot.
            m_sideContent.Children().Remove(v.holder);
            m_sideContent.Children().InsertAtTop(v.holder);
        }
    };
    if (index >= 0 && index < static_cast<int>(m_visible.size()) && !m_busy && m_dock == DockState::Shown)
    {
        // The enlarged card sticks out past the bar toward the stage; widen the view for as long as it does.
        auto& c = *m_visible[index];
        Pose p = HoverPose(c, index, true);
        float half = CropFor(c, true).size.x * p.scale / 2.f + S(8);
        KillTimer(m_sidebar, kTimerShrink);
        RECT r;
        GetWindowRect(m_view, &r);
        OffsetRect(&r, -m_monitor.left, -m_monitor.top);
        if (Right())
        {
            LONG left = static_cast<LONG>(SideToAnim({ p.center.x - half, 0.f }).x);
            if (left < r.left)
                PlaceView(left, r.right, m_bar.bottom - m_monitor.top);
        }
        else
        {
            LONG right = static_cast<LONG>(SideToAnim({ p.center.x + half, 0.f }).x);
            if (right > r.right)
                PlaceView(r.left, right, m_bar.bottom - m_monitor.top);
        }
    }
    else if (index < 0 && !m_busy && !m_dragging && !m_menu.open)
        SetTimer(m_sidebar, kTimerShrink, 200, nullptr);   // back to the bar once the card has settled
    settle(m_hover, false);
    settle(index, true);
    m_hover = index;
    StartTrace(index);
}

void Stage::StartTrace(int index)
{
    if (index < 0 || index >= static_cast<int>(m_visible.size()) || m_busy || m_dragging ||
        m_dock != DockState::Shown || !m_cfg.hoverTrace)
        return;
    RunTrace(*m_visible[index], true);
}

void Stage::RunTrace(Card& c, bool hover)
{
    auto sprite = c.side.sprite;
    if (!sprite || m_cfg.speed == 3)
        return;
    if (!hover && std::any_of(m_traces.begin(), m_traces.end(), [&](auto& t) { return !t.hover && t.parent == sprite; }))
        return;
    auto it = std::find_if(m_visible.begin(), m_visible.end(), [&](auto& o) { return o.get() == &c; });
    if (it == m_visible.end())
        return;
    try
    {
        // Drawn inside the card's sprite, in its own (unscaled window) pixels: the line tilts, turns
        // and grows with the card. Sizes that should look fixed on screen are divided by its scale.
        int index = static_cast<int>(it - m_visible.begin());
        Pose p = hover ? HoverPose(c, index, true) : SlotPose(c, index);
        Crop k = CropFor(c, true);
        float width = kTraceWidth / p.scale, inset = width / 2.f;  // inside the card's clip
        float left = k.offset.x + inset, top = k.offset.y + inset;
        float right = k.offset.x + k.size.x - inset, bottom = k.offset.y + k.size.y - inset;
        float r = std::max(0.f, std::min(S(kRadius) / p.scale - inset, std::min(right - left, bottom - top) / 2.f));
        if (!m_d2d)
            winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d.put()));
        auto& brushes = hover ? m_traceBrushes : m_alertTraceBrushes;
        if (brushes.empty())
            for (int i = 0; i < kTraceSegments; ++i)
            {
                float fade = std::pow(1.f - static_cast<float>(i) / kTraceSegments, 1.6f);
                auto a = static_cast<uint8_t>(235 * fade);
                brushes.push_back(m_compositor.CreateColorBrush(hover ? winrt::Windows::UI::Color{ a, 255, 255, 255 }
                                                                      : winrt::Windows::UI::Color{ a, 255, 149, 0 }));
            }

        // One half of the outline: top middle, along the top to a corner, down the side, along the
        // bottom to its middle. The left half mirrors the right one.
        auto half = [&](bool toRight) {
            auto build = [=](ID2D1Factory* factory) {
                winrt::com_ptr<ID2D1PathGeometry> path;
                winrt::check_hresult(factory->CreatePathGeometry(path.put()));
                winrt::com_ptr<ID2D1GeometrySink> sink;
                winrt::check_hresult(path->Open(sink.put()));
                float mid = (left + right) / 2.f;
                auto x = [&](float v) { return toRight ? v : left + right - v; };
                auto sweep = toRight ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
                sink->BeginFigure({ mid, top }, D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine({ x(right - r), top });
                sink->AddArc({ { x(right), top + r }, { r, r }, 0.f, sweep, D2D1_ARC_SIZE_SMALL });
                sink->AddLine({ x(right), bottom - r });
                sink->AddArc({ { x(right - r), bottom }, { r, r }, 0.f, sweep, D2D1_ARC_SIZE_SMALL });
                sink->AddLine({ mid, bottom });
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                winrt::check_hresult(sink->Close());
                return path.as<ID2D1Geometry>();
            };
            return wuc::CompositionPath(winrt::make<GeometrySource>(build, m_d2d));
        };

        // Every piece runs the path at the same steady speed, each a little later than the one ahead:
        // piece i covers [p - (i+1)*seg, p - i*seg], so the head leads and dimmer pieces follow; past
        // the end they bunch up at the bottom middle and vanish. Nothing shows before its start.
        int wait = hover ? m_cfg.Ms(200) : 0, headMs = m_cfg.Ms(kTraceHeadMs);   // hover: once the card faces front
        // The attention loop repeats: each cycle is one run of the line followed by a rest.
        int cycle = std::max(m_cfg.Ms(kAlertCycleMs), static_cast<int>((1.f + kTraceTail) * headMs) + 1);
        auto linear = m_compositor.CreateLinearEasingFunction();
        auto shape = m_compositor.CreateShapeVisual();
        shape.Size({ c.w, c.h });
        float seg = kTraceTail / kTraceSegments;
        for (bool toRight : { true, false })
        {
            auto path = half(toRight);
            for (int i = 0; i < kTraceSegments; ++i)
            {
                auto geometry = m_compositor.CreatePathGeometry(path);
                geometry.TrimStart(0.f);
                geometry.TrimEnd(0.f);
                auto trim = [&](wchar_t const* prop, float lag) {
                    auto run = m_compositor.CreateScalarKeyFrameAnimation();
                    if (hover)
                    {
                        run.InsertKeyFrame(0.f, 0.f);
                        run.InsertKeyFrame(1.f, 1.f, linear);
                        run.Duration(std::chrono::milliseconds(std::max(1, headMs)));
                        run.DelayTime(std::chrono::milliseconds(wait + static_cast<int>(lag * headMs)));
                        run.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
                    }
                    else
                    {
                        float from = lag * headMs / cycle, to = (lag * headMs + headMs) / cycle;
                        run.InsertKeyFrame(0.f, 0.f);
                        run.InsertKeyFrame(from, 0.f);
                        run.InsertKeyFrame(to, 1.f, linear);
                        run.InsertKeyFrame(1.f, 1.f);
                        run.Duration(std::chrono::milliseconds(cycle));
                        run.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
                    }
                    geometry.StartAnimation(prop, run);
                };
                trim(L"TrimEnd", seg * i);
                trim(L"TrimStart", seg * (i + 1));
                auto stroke = m_compositor.CreateSpriteShape(geometry);
                stroke.StrokeBrush(brushes[i]);
                stroke.StrokeThickness(width);
                stroke.StrokeStartCap(wuc::CompositionStrokeCap::Round);
                stroke.StrokeEndCap(wuc::CompositionStrokeCap::Round);
                shape.Shapes().Append(stroke);
            }
        }
        // One hover run per card; its attention loop steps aside meanwhile (see SweepTraces).
        for (auto& t : m_traces)
            if (t.parent == sprite)
            {
                if (t.hover)
                    t.until = 0;
                else
                    t.shape.IsVisible(false);
            }
        SweepTraces();
        sprite.Children().InsertAtTop(shape);
        ULONGLONG until = hover ? GetTickCount64() + wait + static_cast<ULONGLONG>((1.f + kTraceTail) * headMs) + 60 : ~0ull;
        m_traces.push_back({ shape, sprite, until, hover });
        SweepTraces();                              // (re)arms the timer for the earliest end
        Trace(hover ? L"trace: started" : L"trace: alert", c.hwnd);
    }
    catch (winrt::hresult_error const& e)
    {
        LogError(0, e.code(), L"card trace");
    }
}

void Stage::SweepTraces()
{
    ULONGLONG now = GetTickCount64(), next = 0;
    std::vector<wuc::ContainerVisual> freed;        // cards whose hover run ended
    // An attention loop on a picture that is no longer any card's (its visuals were made again) goes.
    for (auto& t : m_traces)
        if (!t.hover && std::none_of(m_cards.begin(), m_cards.end(), [&](auto& c) { return c->side.sprite == t.parent; }))
            t.until = 0;
    std::erase_if(m_traces, [&](TraceRun& t) {
        if (t.until > now)
        {
            if (t.until != ~0ull)
                next = next ? std::min(next, t.until) : t.until;
            return false;
        }
        try
        {
            t.parent.Children().Remove(t.shape);
        }
        catch (...)
        {
        }
        if (t.hover)
            freed.push_back(t.parent);
        return true;
    });
    // Their attention loop comes back.
    for (auto& t : m_traces)
        if (!t.hover && std::find(freed.begin(), freed.end(), t.parent) != freed.end() &&
            std::none_of(m_traces.begin(), m_traces.end(), [&](auto& o) { return o.hover && o.parent == t.parent; }))
            t.shape.IsVisible(true);
    if (next)
        SetTimer(m_sidebar, kTimerTrace, static_cast<UINT>(next - now), nullptr);
    else
        KillTimer(m_sidebar, kTimerTrace);
}

void Stage::ClearTrace()
{
    for (auto& t : m_traces)
        if (t.hover)
            t.until = 0;
    SweepTraces();
}

void Stage::StopAlertTrace(wuc::ContainerVisual const& sprite)
{
    for (auto& t : m_traces)
        if (!t.hover && t.parent == sprite)
            t.until = 0;
    SweepTraces();
}

// ---- transitions ----------------------------------------------------------

float2 Stage::SideToAnim(float2 p) const
{
    return { p.x + (m_bar.left - m_monitor.left), p.y + (m_bar.top - m_monitor.top) };
}

Pose PoseForRect(RECT const& r, RECT const& monitor, float spriteW)
{
    return { { (r.left + r.right) / 2.f - monitor.left, (r.top + r.bottom) / 2.f - monitor.top },
             (r.right - r.left) / spriteW, 0.f, false };
}

Pose Stage::FramePose(Card const& c) const
{
    return PoseForRect(c.frame, m_monitor, c.w);
}

Pose Stage::TargetPose(Card const& c) const
{
    RECT r;
    if (IsIconic(c.hwnd))
    {
        // The placement rect includes invisible resize borders; land exactly on the visible frame.
        bool maximized = false;
        r = wt::RestoreRect(c.hwnd, &maximized);
        if (!maximized)
            r = { r.left + c.border.left, r.top + c.border.top, r.right - c.border.right, r.bottom - c.border.bottom };
    }
    else
        r = wt::FrameRect(c.hwnd);
    return PoseForRect(r, m_monitor, c.w);
}

// Removes the window's card from the list (callers put it back on top), or makes a new one.
// A pinned card stays where it is; callers must not insert it again.
std::shared_ptr<Card> Stage::TakeCard(HWND hwnd)
{
    auto it = std::find_if(m_cards.begin(), m_cards.end(), [&](auto& c) { return c->hwnd == hwnd; });
    if (it == m_cards.end())
    {
        auto parked = std::find_if(m_offstage.begin(), m_offstage.end(), [&](auto& c) { return c->hwnd == hwnd; });
        if (parked == m_offstage.end())
            return MakeCard(hwnd);
        auto card = *parked;
        m_offstage.erase(parked);
        card->desktop = m_desktopId;
        return card;
    }
    auto card = *it;
    if (!card->pinned)
        m_cards.erase(it);
    return card;
}

void Stage::SwitchTo(size_t index)
{
    ClearTrace();
    auto next = m_visible[index];
    if (wt::IsCloaked(next->hwnd))
    {
        // On another desktop: activating it makes Windows switch there (with its own animation);
        // SyncDesktop takes over once the switch is seen.
        SetHover(-1);
        BringBack(next->hwnd);
        ForceForeground(next->hwnd);
        return;
    }
    Pose from = SlotPose(*next, index);
    from.center = SideToAnim(from.center);
    if (m_hover == static_cast<int>(index))
        from = HoverPose(*next, index, true), from.center = SideToAnim(from.center);
    if (next->hwnd == m_active)
    {
        SetHover(-1);                               // already on stage: the card just settles back
        return;
    }
    m_hover = -1;
    if (!next->pinned)
    {
        m_cards.erase(std::find(m_cards.begin(), m_cards.end(), next));
        Park(next);
    }

    if (!IsWindow(next->hwnd))
    {
        Relayout(true);
        return;
    }
    BeginTransition(next->hwnd, next, from);
}

void Stage::BeginTransition(HWND next, std::shared_ptr<Card> nextCard, Pose const& nextFrom)
{
    ClearTrace();
    ClearAlert(next);
    m_busy = true;
    SetTimer(m_sidebar, kTimerWatchdog, kWatchdogMs, nullptr);
    std::vector<HWND> leaving;
    for (HWND h : m_stage)
    {
        if (h == next || !IsWindow(h) || wt::IsCloaked(h))
            continue;
        if (IsWindowVisible(h) && !IsIconic(h))
            leaving.push_back(h);
        else
        {
            // Minimized or hidden without us noticing in time: no flight, but it keeps its card.
            auto card = TakeCard(h);
            if (!card->pinned)
                m_cards.insert(m_cards.begin(), card);
        }
    }
    m_stage = { next };
    m_active = next;
    m_flyIn = {};
    m_inCard = nextCard;
    m_outs.clear();
    m_pending = 0;

    // Most recently focused ends up on top of the sidebar.
    for (HWND h : leaving)
    {
        auto flight = std::make_shared<OutFlight>();
        flight->card = TakeCard(h);
        if (!flight->card->pinned)
            m_cards.insert(m_cards.begin(), flight->card);
        m_outs.push_back(flight);
    }
    Relayout(true);

    // A leaving window whose card is not in view has nowhere to land: it is just minimized.
    std::erase_if(m_outs, [](auto& flight) {
        if (flight->card->side.holder)
            return false;
        Minimize(flight->card->hwnd);
        return true;
    });
    for (auto& flight : m_outs)
    {
        // The sidebar copy stays hidden until the flying copy lands on it. The outgoing window stays
        // visible until its fresh snapshot is ready, so the incoming one can start moving right away.
        ++m_pending;
        flight->card->side.holder.Opacity(0.f);
        HWND h = flight->card->hwnd;
        RECT frame = wt::FrameRect(h);
        bool fresh = m_prefetch && m_prefetchHwnd == h && GetTickCount64() - m_prefetchAt < kPrefetchMaxAge &&
                     EqualRect(&frame, &m_prefetchFrame);
        bool recent = m_activeSnap && m_activeSnapHwnd == h && GetTickCount64() - m_activeSnapAt < kReuseMaxAge &&
                      EqualRect(&frame, &m_activeSnapFrame);
        if (m_cfg.iconsOnly)
        {
            auto& c = *flight->card;
            OnOutCaptured(flight, frame, MakePlaceholder(c), true);
        }
        else if (fresh)
        {
            auto surface = m_prefetch;
            m_prefetch = nullptr;
            OnOutCaptured(flight, frame, surface);
        }
        else if (recent)
        {
            auto surface = m_activeSnap;
            m_activeSnap = nullptr;
            OnOutCaptured(flight, frame, surface);
        }
        else if (m_prefetching && m_prefetchHwnd == h)
            flight->waitsForPrefetch = true;
        else
            m_snap.CaptureAsync(h, frame, [this, flight, frame](auto const& surface) { OnOutCaptured(flight, frame, surface); });
    }
    if (m_inCard)
    {
        ++m_pending;
        m_flyIn = MakeVis(*m_inCard, false);
        m_animStage.Children().InsertAtTop(m_flyIn.holder);
        Pose to = TargetPose(*m_inCard);
        ApplyPose(m_flyIn, *m_inCard, nextFrom);
        m_inBatch = m_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        AnimatePose(m_flyIn, *m_inCard, nextFrom, to, m_cfg.Ms(kFlyMs));
        m_inBatch.End();
        m_inBatch.Completed([this, gen = m_gen](auto&&, auto&&) { if (gen == m_gen) OnFlyInDone(); });
    }
    if (!m_pending)
    {
        FinishTransition();
        return;
    }
    GrowView();
    SetDock(DockState::Shown);                      // the cards fly to and from the sidebar
}

// The view's origin stays at the monitor's top-left, so resizing never moves any content.
void Stage::GrowView()
{
    KillTimer(m_sidebar, kTimerShrink);
    // One pixel short of the monitor: a borderless topmost window covering it exactly is taken by the
    // shell for a fullscreen app, which made us tuck our own sidebar away mid-transition (the flicker).
    PlaceView(0, m_monitor.right - m_monitor.left, m_monitor.bottom - m_monitor.top - 1, true);
}

// The view in monitor coordinates. Its content is laid out from the monitor's top-left, so when the
// view does not start there (sidebar on the right) the root visual is shifted to match.
// Any emoji panel / touch keyboard still up. One that went away without a cloak event (its process
// restarted) no longer counts.
bool Stage::InputPanelUp()
{
    std::erase_if(m_inputPanels, [](HWND h) { return !IsWindow(h) || !IsWindowVisible(h) || wt::IsCloaked(h); });
    return !m_inputPanels.empty();
}

void Stage::PlaceView(LONG left, LONG right, LONG height, bool grown)
{
    // Always on top, or "show desktop" (four fingers down, Win+D) sweeps the sidebar away with the
    // windows. The one exception is while the emoji panel (Win+.) or touch keyboard is up: those never
    // take focus and would open underneath it. A shown sidebar at rest has nothing else over it (a
    // window covering it makes it tuck away).
    bool topmost = !InputPanelUp() || grown || m_busy || m_dragging || m_menu.open || m_settings.open ||
                   m_dock != DockState::Shown || m_revealed || !m_cfg.autoTuck;
    SetWindowPos(m_view, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, m_monitor.left + left, m_monitor.top, right - left, height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (left != m_viewX)
    {
        m_viewX = left;
        m_root.Offset({ -static_cast<float>(left), 0.f, 0.f });
    }
}

void Stage::ShrinkView()
{
    // Covering only the bar (or just a thin strip at the edge while tucked) keeps us off other apps'
    // pixels, so their overlay/flip optimizations stay intact.
    // An open menu or settings panel lies outside the bar and needs the whole monitor.
    if (m_settings.open || m_menu.open)
        return;
    if (m_dock == DockState::Hidden || (m_dock == DockState::Tucked && !m_cfg.edgeReveal))
    {
        ShowWindow(m_view, SW_HIDE);
        return;
    }
    LONG strip = static_cast<LONG>(S(kEdgeStrip)), height = m_bar.bottom - m_monitor.top;
    LONG barLeft = m_bar.left - m_monitor.left, barRight = m_bar.right - m_monitor.left;
    if (Right())
        PlaceView(m_dock == DockState::Tucked ? barRight - strip : barLeft, barRight, height);
    else
        PlaceView(0, m_dock == DockState::Tucked ? barLeft + strip : barRight, height);
}

// Slides the cards out to the left (tucked or hidden) or back in, then sizes the view to match.
void Stage::SetDock(DockState state)
{
    if (state == m_dock)
        return;
    bool wasOut = m_dock != DockState::Shown, out = state != DockState::Shown;
    Trace(state == DockState::Shown ? L"dock shown" : state == DockState::Tucked ? L"dock tucked" : L"dock hidden");
    m_dock = state;
    // While a fullscreen app runs, Alt+number belongs to it (VS Code tabs, JetBrains tool windows...).
    SetHotkeys(state != DockState::Hidden);
    if (wasOut == out)
    {
        if (!m_busy && !m_dragging && !m_menu.open)
            ShrinkView();
        return;
    }
    float base = SideToAnim({ 0.f, 0.f }).x;
    float hidden = Right() ? base + static_cast<float>(m_monitor.right - m_bar.left)
                           : base - static_cast<float>(m_bar.right - m_monitor.left);
    if (!out && !m_busy && !m_dragging && !m_menu.open)
        ShrinkView();                               // full bar size first, so the slide-in is visible
    auto anim = m_compositor.CreateScalarKeyFrameAnimation();
    anim.InsertKeyFrame(1.f, out ? hidden : base, m_ease);
    anim.Duration(std::chrono::milliseconds(m_cfg.Ms(kSlideMs)));
    m_slideBatch = m_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    m_sideContent.StartAnimation(L"Offset.X", anim);
    m_slideBatch.End();
    m_slideBatch.Completed([this](auto&&, auto&&) {
        m_slideBatch = nullptr;
        if (m_dock != DockState::Shown && !m_busy && !m_dragging && !m_menu.open)
            ShrinkView();                           // down to the edge strip, or gone
    });
}

bool Stage::CoversBar(HWND hwnd) const
{
    // Windows on other desktops are cloaked by DWM: a cheap attribute read instead of asking Explorer.
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || wt::IsCloaked(hwnd))
        return false;
    RECT frame = wt::FrameRect(hwnd), overlap;
    if (FitsBeside(hwnd))
        return false;                               // about to be moved next to the bar instead
    return IntersectRect(&overlap, &frame, &m_bar) && overlap.right - overlap.left > S(4);
}

// Fullscreen app in front -> hidden. A stage window over the bar -> tucked (unless the pointer pulled
// it out). Otherwise shown. Nothing changes while something is animating; it is re-checked after.
void Stage::UpdateDock()
{
    if (!m_ready || m_busy || m_dragging || m_menu.open || m_settings.open)
        return;
    HWND top = wt::TopWindow(m_mon);
    bool fullscreen = wt::IsFullscreen(top);
    // The front window counts too, in case it reached the front without us seeing the focus change.
    bool covered = (top && CoversBar(top)) || std::any_of(m_stage.begin(), m_stage.end(), [&](HWND h) { return CoversBar(h); });
    SetDock(fullscreen ? DockState::Hidden : covered && !m_revealed && m_cfg.autoTuck ? DockState::Tucked : DockState::Shown);
}

// Moving or maximizing a window only raises EVENT_OBJECT_LOCATIONCHANGE, which fires constantly
// system-wide (every cursor and caret move). So we listen only to the threads that own stage windows;
// busy apps (a playing music app, a browser) keep their other threads' events away from us.
void Stage::StageChanged()
{
    std::vector<DWORD> threads;
    for (HWND h : m_stage)
    {
        DWORD tid = GetWindowThreadProcessId(h, nullptr);
        if (tid && std::find(threads.begin(), threads.end(), tid) == threads.end())
            threads.push_back(tid);
    }
    std::erase_if(m_moveHooks, [&](auto& entry) {
        if (std::find(threads.begin(), threads.end(), entry.first) != threads.end())
            return false;
        UnhookWinEvent(entry.second);
        return true;
    });
    for (DWORD tid : threads)
    {
        if (std::none_of(m_moveHooks.begin(), m_moveHooks.end(), [&](auto& e) { return e.first == tid; }))
        {
            DWORD pid = 0;
            for (HWND h : m_stage)
                if (GetWindowThreadProcessId(h, &pid) == tid)
                    break;
            m_moveHooks.emplace_back(tid, SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                nullptr, WinEventProc, pid, tid, WINEVENT_OUTOFCONTEXT));
            // Apps that "close" to the tray (KakaoTalk, Discord...) only hide their window.
            m_moveHooks.emplace_back(tid, SetWinEventHook(EVENT_OBJECT_HIDE, EVENT_OBJECT_HIDE,
                nullptr, WinEventProc, pid, tid, WINEVENT_OUTOFCONTEXT));
        }
    }
    UpdateDock();
}

void Stage::SetHotkeys(bool on)
{
    on = on && m_cfg.hotkeys;
    if (on == m_hotkeysOn)
        return;
    m_hotkeysOn = on;
    for (int i = 0; i < kMaxHotkeys; ++i)
    {
        UnregisterHotKey(m_sidebar, kHotkeyBase + i);
        if (on && i < m_cfg.cards && !RegisterHotKey(m_sidebar, kHotkeyBase + i, m_cfg.HotkeyModifiers() | MOD_NOREPEAT, '1' + i))
            LogError(WM_HOTKEY, HRESULT_FROM_WIN32(GetLastError()), L"hotkey already registered by another app");
    }
}

void Stage::Prefetch()
{
    HWND h = m_active;
    if (m_cfg.iconsOnly || m_busy || m_prefetching || !h || !IsWindowVisible(h) || IsIconic(h))
        return;
    if (h == m_prefetchHwnd && m_prefetch && GetTickCount64() - m_prefetchAt < kPrefetchMaxAge / 2)
        return;
    if (h == m_activeSnapHwnd && m_activeSnap && GetTickCount64() - m_activeSnapAt < kReuseMaxAge)
        return;                                     // the stage window's recent picture will do
    m_prefetching = true;
    m_prefetchHwnd = h;
    m_prefetchFrame = wt::FrameRect(h);
    m_snap.CaptureAsync(h, m_prefetchFrame, [this, h](auto const& surface) {
        m_prefetching = false;
        bool wanted = m_tracking || std::any_of(m_outs.begin(), m_outs.end(), [](auto& f) { return f->waitsForPrefetch; });
        m_prefetch = wanted ? surface : nullptr;    // the pointer already left: don't hold it at idle
        m_prefetchAt = GetTickCount64();
        for (auto& flight : m_outs)
        {
            if (flight->waitsForPrefetch && flight->card->hwnd == h)
            {
                flight->waitsForPrefetch = false;
                m_prefetch = nullptr;
                OnOutCaptured(flight, m_prefetchFrame, surface);
            }
        }
    });
}

void Stage::OnOutCaptured(std::shared_ptr<OutFlight> flight, RECT const& frame, wuc::CompositionDrawingSurface const& surface, bool placeholder)
{
    if (std::find(m_outs.begin(), m_outs.end(), flight) == m_outs.end())
        return;
    auto card = flight->card;
    SetSnapshot(*card, frame, surface, placeholder);

    // Below the incoming copy, so the new window visibly lands on top.
    flight->vis = MakeVis(*card, false);
    m_animStage.Children().InsertAtBottom(flight->vis.holder);
    Pose from = PoseForRect(frame, m_monitor, card->w);
    size_t slot = static_cast<size_t>(std::find(m_visible.begin(), m_visible.end(), card) - m_visible.begin());
    Pose to = SlotPose(*card, std::min(slot, m_visible.empty() ? 0 : m_visible.size() - 1));
    to.center = SideToAnim(to.center);
    ApplyPose(flight->vis, *card, from);
    flight->batch = m_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    AnimatePose(flight->vis, *card, from, to, m_cfg.Ms(kFlyMs));
    flight->batch.End();
    flight->batch.Completed([this, flight, gen = m_gen](auto&&, auto&&) {
        flight->batch = nullptr;
        if (flight->vis.holder)
            m_animStage.Children().Remove(flight->vis.holder);
        flight->vis = {};
        if (flight->card->side.holder)
            flight->card->side.holder.Opacity(1.f);
        if (gen == m_gen)                           // not a transition the watchdog already ended
            StepDone();
    });
    // Hide the real window only once the flying copy is on screen above it.
    m_toMinimize.push_back(card->hwnd);
    SetTimer(m_sidebar, kTimerMinimizeOut, 30, nullptr);
}

void Stage::OnFlyInDone()
{
    if (!m_inCard)
        return;
    m_inBatch = nullptr;
    BringBack(m_inCard->hwnd);
    ForceForeground(m_inCard->hwnd);
    // Give the real window a moment to paint before the flying copy fades away.
    SetTimer(m_sidebar, kTimerFade, kPaintWaitMs, nullptr);
}

void Stage::FadeOutFlyIn()
{
    if (!m_busy || !m_flyIn.holder)
        return;
    auto fade = m_compositor.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.f, 1.f);
    fade.InsertKeyFrame(1.f, 0.f);
    fade.Duration(std::chrono::milliseconds(m_cfg.Ms(kFadeMs)));
    m_inBatch = m_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    m_flyIn.holder.StartAnimation(L"Opacity", fade);
    m_inBatch.End();
    m_inBatch.Completed([this, gen = m_gen](auto&&, auto&&) {
        if (gen != m_gen)
            return;
        m_inBatch = nullptr;
        StepDone();
    });
}

void Stage::StepDone()
{
    if (--m_pending <= 0)
        FinishTransition();
}

void Stage::FinishTransition()
{
    ++m_gen;                                        // callbacks still pending belong to the old one
    KillTimer(m_sidebar, kTimerWatchdog);
    KillTimer(m_sidebar, kTimerFade);
    m_inBatch = nullptr;
    for (auto& flight : m_outs)
    {
        if (flight->vis.holder)
            m_animStage.Children().Remove(flight->vis.holder);
        if (flight->card->side.holder)
            flight->card->side.holder.Opacity(1.f);
    }
    m_outs.clear();
    if (m_flyIn.holder)
        m_animStage.Children().Remove(m_flyIn.holder);
    m_flyIn = {};
    m_inCard = nullptr;
    m_pending = 0;
    m_prefetch = nullptr;
    // Shrink only after the removal above has been composited, or the last frame could be cut.
    SetTimer(m_sidebar, kTimerShrink, 50, nullptr);
    m_quietUntil = GetTickCount64() + kQuietMs / 2;
    SnapActiveSoon();
    m_revealed = false;
    m_busy = false;
    // Windows destroyed while we were animating were skipped by OnGone; drop them now.
    std::erase_if(m_offstage, [](auto& c) { return !IsWindow(c->hwnd); });
    bool stale = false;
    for (auto& c : m_cards)
        stale |= !IsWindow(c->hwnd);
    if (stale)
    {
        std::vector<HWND> dead;
        for (auto& c : m_cards)
            if (!IsWindow(c->hwnd))
                dead.push_back(c->hwnd);
        for (HWND h : dead)
            RemoveCard(h, true);
    }
    std::erase_if(m_stage, [](HWND h) { return !IsWindow(h); });
    if (m_active && !IsWindow(m_active))
        m_active = m_stage.empty() ? nullptr : m_stage.back();
    StageChanged();                                 // after m_busy is cleared, or the dock won't update
    // Focus changes that arrived while animating were ignored; catch up once things have settled.
    SetTimer(m_sidebar, kTimerRecheck, static_cast<UINT>(kQuietMs / 2) + 30, nullptr);
    TrimMemory();
}

// Makes a window visible again however it was put away: hidden to the tray (KakaoTalk, Discord...
// close to a hidden window), or minimized, including apps that handle minimizing themselves and
// only react to the taskbar's restore command.
void Stage::BringBack(HWND hwnd)
{
    if (!IsWindowVisible(hwnd))
        ShowWindowAsync(hwnd, SW_SHOW);
    if (IsIconic(hwnd))
    {
        ShowWindowAsync(hwnd, SW_RESTORE);
        PostMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
    }
    // An unowned dialog blocking it was put away with it (see Minimize); owned ones follow by themselves.
    if (HWND dialog = wt::ModalDialog(hwnd); dialog && IsIconic(dialog))
        ShowWindowAsync(dialog, SW_RESTORE);
}

// Minimizes a window leaving the stage, together with an unowned dialog blocking it, which would
// otherwise stay behind on screen with its window gone.
void Stage::Minimize(HWND hwnd)
{
    ShowWindowAsync(hwnd, SW_MINIMIZE);
    if (HWND dialog = wt::ModalDialog(hwnd); dialog && !GetWindow(dialog, GW_OWNER) && !IsIconic(dialog))
        ShowWindowAsync(dialog, SW_MINIMIZE);
}

void Stage::ForceForeground(HWND hwnd)
{
    // A window blocked by a dialog can't take focus; the dialog is what the user needs.
    if (HWND dialog = wt::ModalDialog(hwnd))
        hwnd = dialog;
    if (SetForegroundWindow(hwnd))
        return;
    // Foreground lock: a synthetic Alt tap counts as input and lets us hand focus over. Not while the
    // user holds Alt (Alt+number): our key-up would leave Alt stuck up, and the real release would
    // then open the new window's menu bar.
    if (GetAsyncKeyState(VK_MENU) & 0x8000)
    {
        // Sharing input state with the foreground thread lifts the lock without touching the keyboard.
        DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), self = GetCurrentThreadId();
        if (fgThread && fgThread != self && AttachThreadInput(self, fgThread, TRUE))
        {
            SetForegroundWindow(hwnd);
            AttachThreadInput(self, fgThread, FALSE);
        }
        return;
    }
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_MENU;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
    SetForegroundWindow(hwnd);
}

// ---- drag a card onto the stage ---------------------------------------------

void Stage::BeginDrag(POINT viewPt)
{
    if (m_pressIndex < 0 || m_pressIndex >= static_cast<int>(m_visible.size()))
        return;
    m_dragging = true;
    m_dragCard = m_visible[m_pressIndex];
    // Starts from how the card looks right now (usually hovered: facing front and enlarged).
    Pose from = HoverPose(*m_dragCard, m_pressIndex, m_hover == m_pressIndex);
    from.center = SideToAnim(from.center);
    float slotScale = SlotPose(*m_dragCard, m_pressIndex).scale;
    SetHover(-1);
    if (!m_dragCard->pinned && m_dragCard->side.holder)
        m_dragCard->side.holder.Opacity(0.f);
    m_dragVis = MakeVis(*m_dragCard, false);
    m_animStage.Children().InsertAtTop(m_dragVis.holder);
    // Lifted: it straightens out and grows a little while following the pointer.
    m_dragPose = { { static_cast<float>(viewPt.x), static_cast<float>(viewPt.y) }, slotScale * 1.25f, 0.f, true };
    ApplyPose(m_dragVis, *m_dragCard, from);
    AnimatePose(m_dragVis, *m_dragCard, from, m_dragPose, m_cfg.Ms(180));
    GrowView();
}

void Stage::MoveDrag(POINT viewPt)
{
    m_dragPose.center = { static_cast<float>(viewPt.x), static_cast<float>(viewPt.y) };
    m_dragVis.holder.StopAnimation(L"Offset");
    m_dragVis.holder.Offset({ m_dragPose.center.x, m_dragPose.center.y, 0.f });
}

void Stage::EndDrag(POINT viewPt, bool cancel)
{
    m_dragging = false;
    m_pressIndex = -1;
    auto card = m_dragCard;
    m_dragCard = nullptr;
    // The card may have been taken away mid-drag (its window came to the front, a hotkey...).
    bool stillOurs = std::find(m_cards.begin(), m_cards.end(), card) != m_cards.end();
    bool pastBar = Right() ? viewPt.x < m_bar.left - m_monitor.left : viewPt.x > m_bar.right - m_monitor.left;
    bool onStage = !cancel && stillOurs && pastBar && IsWindow(card->hwnd);
    if (onStage && !m_busy)
    {
        JoinStage(card, viewPt);
        return;
    }
    // Dropped on another pinned card: it takes that place (and keeps it after a restart).
    if (!cancel && stillOurs && card->pinned && !card->pinKey.empty())
    {
        int target = HitTest(ViewToSide(MAKELPARAM(viewPt.x, viewPt.y)));
        if (target >= 0 && m_visible[target] != card && m_visible[target]->pinned && !m_visible[target]->pinKey.empty())
        {
            Trace(L"pin moved", card->hwnd);
            MovePin(card->pinKey, m_visible[target]->pinKey);
            Relayout(true);
        }
    }
    // Dropped back on the sidebar: return to its slot.
    size_t slot = static_cast<size_t>(std::find(m_visible.begin(), m_visible.end(), card) - m_visible.begin());
    Pose home = SlotPose(*card, std::min(slot, m_visible.empty() ? 0 : m_visible.size() - 1));
    home.center = SideToAnim(home.center);
    auto vis = m_dragVis;
    m_dragVis = {};
    auto batch = m_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    AnimatePose(vis, *card, m_dragPose, home, m_cfg.Ms(kSlideMs));
    batch.End();
    batch.Completed([this, vis, card](auto&&, auto&&) {
        m_animStage.Children().Remove(vis.holder);
        if (card->side.holder)
            card->side.holder.Opacity(1.f);
        if (!m_busy)
            SetTimer(m_sidebar, kTimerShrink, 50, nullptr);
    });
}

// The dragged window opens where it was dropped, at its own size, next to what is already on stage.
void Stage::JoinStage(std::shared_ptr<Card> card, POINT viewPt)
{
    m_busy = true;
    SetTimer(m_sidebar, kTimerWatchdog, kWatchdogMs, nullptr);
    HWND h = card->hwnd;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(m_mon, &mi);
    RECT work = mi.rcWork;

    bool maximized = false;
    RECT restore = wt::RestoreRect(h, &maximized);
    LONG w = std::min<LONG>(restore.right - restore.left, work.right - work.left);
    LONG hgt = std::min<LONG>(restore.bottom - restore.top, work.bottom - work.top);
    if (maximized)
    {
        w = (work.right - work.left) * 2 / 3;
        hgt = (work.bottom - work.top) * 2 / 3;
    }
    LONG cx = viewPt.x + m_monitor.left, cy = viewPt.y + m_monitor.top;
    RECT target{ cx - w / 2, cy - hgt / 2, cx - w / 2 + w, cy - hgt / 2 + hgt };
    OffsetRect(&target, std::max(0L, work.left - target.left) - std::max(0L, target.right - work.right),
        std::max(0L, work.top - target.top) - std::max(0L, target.bottom - work.bottom));

    // Put the window there while it is still minimized, so restoring it doesn't flash at the old spot.
    WINDOWPLACEMENT wp{ sizeof(wp) };
    GetWindowPlacement(h, &wp);
    wp.flags &= ~WPF_RESTORETOMAXIMIZED;
    wp.showCmd = IsIconic(h) ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE;
    wp.rcNormalPosition = target;
    OffsetRect(&wp.rcNormalPosition, mi.rcMonitor.left - mi.rcWork.left, mi.rcMonitor.top - mi.rcWork.top);
    SetWindowPlacement(h, &wp);

    if (!card->pinned)
    {
        m_cards.erase(std::find(m_cards.begin(), m_cards.end(), card));
        Park(card);
    }
    Relayout(true);

    RECT frame{ target.left + card->border.left, target.top + card->border.top,
                target.right - card->border.right, target.bottom - card->border.bottom };
    std::erase(m_stage, h);                         // e.g. a pinned card of a window already on stage
    m_stage.push_back(h);
    m_active = h;
    m_inCard = card;
    m_flyIn = m_dragVis;
    m_dragVis = {};
    m_outs.clear();
    m_pending = 1;
    m_inBatch = m_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    AnimatePose(m_flyIn, *card, m_dragPose, PoseForRect(frame, m_monitor, card->w), m_cfg.Ms(kFlyMs));
    m_inBatch.End();
    m_inBatch.Completed([this, gen = m_gen](auto&&, auto&&) { if (gen == m_gen) OnFlyInDone(); });
}

// ---- window procs ---------------------------------------------------------

LRESULT CALLBACK Stage::SidebarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_stage && g_stage->m_sidebar == hwnd)
    {
        // Exceptions must not unwind through user32; Windows would swallow them and leave us half-updated.
        try
        {
            return g_stage->OnSidebarMessage(msg, wp, lp);
        }
        catch (winrt::hresult_error const& e)
        {
            LogError(msg, e.code(), e.message().c_str());
        }
        catch (...)
        {
            LogError(msg, E_FAIL, L"unknown exception");
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK Stage::ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_stage && g_stage->m_view == hwnd)
    {
        try
        {
            return g_stage->OnViewMessage(msg, wp, lp);
        }
        catch (winrt::hresult_error const& e)
        {
            LogError(msg, e.code(), e.message().c_str());
        }
        catch (...)
        {
            LogError(msg, E_FAIL, L"unknown exception");
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The view's origin is the monitor's top-left; cards are laid out in bar coordinates.
POINT Stage::ViewToSide(LPARAM lp) const
{
    return { GET_X_LPARAM(lp) - (m_bar.left - m_monitor.left), GET_Y_LPARAM(lp) - (m_bar.top - m_monitor.top) };
}

LRESULT Stage::OnViewMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    // Everything below works in monitor coordinates; the view may start further right.
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && msg != WM_MOUSEWHEEL && msg != WM_MOUSEHWHEEL && m_viewX)
        lp = MAKELPARAM(GET_X_LPARAM(lp) + m_viewX, GET_Y_LPARAM(lp));
    if (m_settings.open)
    {
        if (msg == WM_MOUSEWHEEL)
        {
            SettingsWheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (msg == WM_MOUSEMOVE)
            SettingsMove(pt);
        else if (msg == WM_LBUTTONUP)
            SettingsClick(pt);
        else if (msg == WM_RBUTTONUP && !InSettingsPanel(pt))
            CloseSettings();
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)
            return 0;
    }
    if (m_menu.open)
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        switch (msg)
        {
        case WM_MOUSEMOVE:
            SetMenuHover(MenuItemAt(pt));
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        {
            int item = MenuItemAt(pt);
            if (item >= 0)
                RunMenuItem(item);
            else
                CloseCardMenu();
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            return 0;
        }
    }
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        KillTimer(m_sidebar, kTimerTuck);
        if (m_dock == DockState::Tucked && !m_busy)
        {
            m_revealed = true;
            SetDock(DockState::Shown);
        }
        if (!m_tracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_view, 0 };
            m_tracking = TrackMouseEvent(&tme);
            Prefetch();
        }
        if (m_dragging)
        {
            // No mouse capture: we never take focus, so Windows won't keep capture for us. Once a drag
            // starts the view covers the whole monitor anyway; a release we missed shows up here.
            if (wp & MK_LBUTTON)
                MoveDrag({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            else
                EndDrag({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }, false);
            return 0;
        }
        if (!(wp & MK_LBUTTON))
            m_pressIndex = -1;          // released outside the view before it became a drag
        if (m_pressIndex >= 0 && !m_busy &&
            std::hypot(GET_X_LPARAM(lp) - m_pressPt.x, GET_Y_LPARAM(lp) - m_pressPt.y) > S(kDragStart))
        {
            BeginDrag({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            return 0;
        }
        if (!m_busy)
            SetHover(HitTest(ViewToSide(lp)));
        return 0;
    case WM_LBUTTONDOWN:
        if (!m_busy)
        {
            // The speaker is a button of its own: no switch, no drag.
            m_pressSound = SoundAt(ViewToSide(lp));
            m_pressIndex = m_pressSound >= 0 ? -1 : HitTest(ViewToSide(lp));
            m_pressPt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        }
        return 0;
    case WM_LBUTTONUP:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (m_pressSound >= 0)
        {
            int s = m_pressSound;
            m_pressSound = -1;
            if (!m_busy && s < static_cast<int>(m_visible.size()) && SoundAt(ViewToSide(lp)) == s)
            {
                auto& c = *m_visible[s];
                m_audio.SetMute(c.app, c.sound != 2);
            }
            return 0;
        }
        int pressed = m_pressIndex;
        m_pressIndex = -1;
        if (m_dragging)
            EndDrag(pt, false);
        else if (!m_busy && pressed >= 0 && HitTest(ViewToSide(lp)) == pressed)
            SwitchTo(static_cast<size_t>(pressed));
        return 0;
    }

    case WM_RBUTTONUP:
        if (!m_busy)
        {
            POINT side = ViewToSide(lp);
            if (side.x >= 0 && side.x < m_bar.right - m_bar.left)
                OpenCardMenu(HitTest(side), GET_Y_LPARAM(lp));
        }
        return 0;
    case WM_MOUSELEAVE:
        m_tracking = false;
        if (m_revealed)
            SetTimer(m_sidebar, kTimerTuck, 350, nullptr);     // brief grace period before tucking back
        if (!m_busy)
            m_prefetch = nullptr;       // don't hold the memory once the pointer leaves
        SetHover(-1);
        return 0;
    }
    return DefWindowProcW(m_view, msg, wp, lp);
}

LRESULT Stage::OnSidebarMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == m_taskbarMsg && m_taskbarMsg)
    {
        AddTrayIcon();                              // Explorer restarted: the old icon is gone
        return 0;
    }
    if (msg == m_shellMsg && m_shellMsg)
    {
        if ((wp & 0x7FFF) == HSHELL_WINDOWDESTROYED)
            OnGone(reinterpret_cast<HWND>(lp));
        else if (wp == (HSHELL_REDRAW | HSHELL_HIGHBIT))           // HSHELL_FLASH
            OnFlash(reinterpret_cast<HWND>(lp));
        else if ((wp & 0x7FFF) == HSHELL_WINDOWCREATED)
        {
            // A new window on the taskbar: often shown (or titled) only after it took focus, when the
            // foreground event already turned it down.
            Trace(L"created", reinterpret_cast<HWND>(lp));
            m_newWindow = reinterpret_cast<HWND>(lp);
            m_rechecks = kMaxRechecks;
            SetTimer(m_sidebar, kTimerNewWindow, kRecheckMs, nullptr);
        }
        return 0;
    }
    switch (msg)
    {
    case WM_AUDIO:
        OnAudioChanged();
        return 0;
    case WM_HOTKEY:
    {
        size_t index = static_cast<size_t>(wp - kHotkeyBase);
        CloseCardMenu();
        if (!m_busy && !m_dragging && !m_settings.open && m_dock != DockState::Hidden && index < m_visible.size())
        {
            Prefetch();     // no hover happened; the outgoing window is captured on the way
            SwitchTo(index);
        }
        return 0;
    }
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP)
            ShowTrayMenu();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_EXIT)
            PostQuitMessage(0);
        else if (LOWORD(wp) == ID_SETTINGS && !m_busy && !m_dragging)
        {
            CloseCardMenu();
            OpenSettings();                         // also reachable by other tools (and tests)
        }
        else if (LOWORD(wp) == ID_FIT)
        {
            Trace(L"fit requested", reinterpret_cast<HWND>(lp));
            if (IsWindow(reinterpret_cast<HWND>(lp)))
                FitToSaved(reinterpret_cast<HWND>(lp));
        }
        return 0;
    case WM_TIMER:
        KillTimer(m_sidebar, wp);
        if (wp == kTimerPopulate)
        {
            m_desktopId = CurrentDesktopId();
            Populate();
            SnapActiveSoon();
            m_ready = true;
            StageChanged();
            SetTimer(m_sidebar, kTimerTrim, 3000, nullptr);     // after the startup captures have landed
            m_quietUntil = GetTickCount64() + kQuietMs;
            DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
            m_hooks.push_back(SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0, flags));
            m_hooks.push_back(SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr, WinEventProc, 0, 0, flags));
            // The emoji panel and touch keyboard are shown and put away by cloaking. Cloak events are
            // rare (desktop switches, UWP windows), unlike object show/hide.
            m_hooks.push_back(SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, WinEventProc, 0, 0, flags));
            // Object-level destroy/hide WinEvents would wake us for every caret, tooltip and menu in the
            // system; the shell hook only reports top-level app windows.
            m_shellMsg = RegisterWindowMessageW(L"SHELLHOOK");
            RegisterShellHookWindow(m_sidebar);
            SetHotkeys(m_dock != DockState::Hidden);
        }
        else if (wp == kTimerActiveSnap)
        {
            HWND h = m_active;
            if (h && IsWindowVisible(h) && !IsIconic(h) && !IsZoomed(h))
            {
                // Its invisible borders, for fitting it later while it is minimized.
                RECT wr{};
                GetWindowRect(h, &wr);
                RECT visible = wt::FrameRect(h);
                RECT border{ visible.left - wr.left, visible.top - wr.top, wr.right - visible.right, wr.bottom - visible.bottom };
                std::erase_if(m_borders, [&](auto& b) { return b.first == h || !IsWindow(b.first); });
                m_borders.push_back({ h, border });
            }
            if (h && !m_busy && !m_cfg.iconsOnly && IsWindowVisible(h) && !IsIconic(h))
            {
                RECT frame = wt::FrameRect(h);
                m_snap.CaptureAsync(h, frame, [this, h, frame](auto const& surface) {
                    if (!surface || h != m_active)
                        return;
                    Trace(L"stage window photographed", h);
                    TrimMemory();
                    m_activeSnap = surface;
                    m_activeSnapHwnd = h;
                    m_activeSnapFrame = frame;
                    m_activeSnapAt = GetTickCount64();
                });
            }
        }
        else if (wp == kTimerMinimizeOut)
        {
            for (HWND h : m_toMinimize)
            {
                // A window the app hid itself (closed to the tray) stays hidden; minimizing it would
                // bring it back onto the taskbar.
                if (IsWindowVisible(h) && !IsIconic(h))
                    Minimize(h);
            }
            m_toMinimize.clear();
        }
        else if (wp == kTimerFade)
            FadeOutFlyIn();
        else if (wp == kTimerShrink && !m_busy && !m_dragging && !m_menu.open && !m_settings.open && m_hover < 0)
        {
            ShrinkView();
            UpdateDock();
        }
        else if (wp == kTimerTrim)
            TrimMemory();
        else if (wp == kTimerDock)
            UpdateDock();
        else if (wp == kTimerHidden)
        {
            if (m_busy)
                SetTimer(m_sidebar, kTimerHidden, kHiddenMs, nullptr);
            else
            {
                auto hidden = std::move(m_hidden);
                m_hidden.clear();
                KillTimer(m_sidebar, kTimerHidden);
                for (HWND h : hidden)
                {
                    if (!IsWindow(h))
                        OnGone(h);                  // closed: no card
                    else if (IsWindowVisible(h))
                    {
                        // Shown again: back on stage as it was.
                        if (!OnStage(h) && std::none_of(m_cards.begin(), m_cards.end(), [h](auto& c) { return c->hwnd == h; }))
                        {
                            m_stage.push_back(h);
                            if (GetForegroundWindow() == h)
                            {
                                m_active = h;
                                SnapActiveSoon();
                            }
                            StageChanged();
                        }
                    }
                    else if (QuitsOnClose(h) && QuitApp(h, true))
                        continue;                   // an app chosen to quit on X: quitting (a card if spared)
                    else
                        PutAwayHidden(h);           // closed to the tray: a card, if wanted
                }
                UpdateDock();
            }
        }
        else if (wp == kTimerTrace)
            SweepTraces();
        else if (wp == kTimerQuit)
            FinishQuits();                          // also hands out cards held back during an animation
        else if (wp == kTimerSweep)
        {
            if (m_busy)
                SetTimer(m_sidebar, kTimerSweep, 300, nullptr);
            else
            {
                std::vector<HWND> dead;
                for (auto& c : m_cards)
                    if (!IsWindow(c->hwnd))
                        dead.push_back(c->hwnd);
                for (HWND h : dead)
                    RemoveCard(h, true);
                std::erase_if(m_offstage, [](auto& c) { return !IsWindow(c->hwnd); });
                std::erase_if(m_stage, [](HWND h) { return !IsWindow(h); });
                if (m_active && !IsWindow(m_active))
                    m_active = m_stage.empty() ? nullptr : m_stage.back();
            }
        }
        else if (wp == kTimerWatchdog && m_busy)
        {
            // Something went wrong mid-transition (an exception, a lost device, a batch that never
            // completed). Never leave input blocked or the view covering the screen.
            LogError(WM_TIMER, E_FAIL, L"transition watchdog fired");
            // The clicked window comes up (hidden or still minimized), the leaving ones go down, as the
            // transition would have left them.
            if (m_inCard && IsWindow(m_inCard->hwnd) && (!IsWindowVisible(m_inCard->hwnd) || IsIconic(m_inCard->hwnd)))
            {
                BringBack(m_inCard->hwnd);
                ForceForeground(m_inCard->hwnd);
            }
            for (auto& flight : m_outs)
            {
                HWND h = flight->card->hwnd;
                if (IsWindow(h) && IsWindowVisible(h) && !IsIconic(h) && !OnStage(h))
                    Minimize(h);
            }
            FinishTransition();
            ShrinkView();
        }
        else if (wp == kTimerRecheck)
        {
            HWND fg = GetForegroundWindow();
            if (fg && fg != m_active)
                OnForeground(fg);
        }
        else if (wp == kTimerNewWindow)
        {
            // Acted on only once the focused window is an app window (so an open menu or settings
            // panel isn't closed by these checks); mid-animation or just after one, later.
            HWND fg = GetForegroundWindow();
            if (fg && fg != m_active && m_rechecks-- > 0)
            {
                if (!m_busy && GetTickCount64() >= m_quietUntil && wt::IsManageable(fg, m_mon))
                    OnForeground(fg);
                else
                    SetTimer(m_sidebar, kTimerNewWindow, kRecheckMs, nullptr);
            }
        }
        else if (wp == kTimerTuck)
        {
            m_revealed = false;
            UpdateDock();
        }
        else if (wp == kTimerFit)
        {
            if (m_busy || m_dragging)
                SetTimer(m_sidebar, kTimerFit, 250, nullptr);
            else
                FitBeside();
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (wp != SPI_SETWORKAREA)
            break;
        [[fallthrough]];
    case WM_DISPLAYCHANGE:
        Dock();
        Relayout(false);
        return 0;
    }
    return DefWindowProcW(m_sidebar, msg, wp, lp);
}
