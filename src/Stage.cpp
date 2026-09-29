#include "Stage.h"
#include "WindowTracker.h"

namespace
{
    constexpr UINT WM_TRAY = WM_APP + 2;
    constexpr UINT ID_EXIT = 1;
    constexpr UINT ID_PIN = 2;
    constexpr UINT ID_UNPIN = 3;
    constexpr UINT ID_CLOSE = 4;
    constexpr UINT ID_SETTINGS = 6;
    constexpr UINT ID_AUTOSTART = 5;
    constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kRunValue[] = L"StageManager";
    constexpr wchar_t kStateKey[] = L"Software\\StageManager";

    // Card menu metrics (at 96 dpi).
    constexpr float kMenuW = 176.f;
    constexpr float kMenuItemH = 34.f;
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
    constexpr UINT_PTR kTimerDock = 9;
    constexpr UINT_PTR kTimerWatchdog = 10;
    constexpr UINT_PTR kTimerHidden = 12;     // a hidden stage window: closed, or only put away?
    constexpr UINT kHiddenMs = 300;
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
    // While we run, a minimized stage window flies into the sidebar instead of shrinking to the
    // taskbar. Not persisted (no SPIF_UPDATEINIFILE); restored on exit.
    SetMinAnimate(false);
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
    AddTrayIcon();
    SetTimer(m_sidebar, kTimerPopulate, 400, nullptr);
    return true;
}

void Stage::Shutdown()
{
    SetHotkeys(false);
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
    m_sideContent.Offset({ barOrigin.x, barOrigin.y, 0.f });
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
    nid.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
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
        m_menu.items = {
            card->pinned ? MenuItem{ ID_UNPIN, L"\xE77A", L"고정 해제" } : MenuItem{ ID_PIN, L"\xE718", L"탭 고정" },
            MenuItem{ ID_CLOSE, L"\xE8BB", L"창 닫기" },
        };
    m_menu.items.push_back(MenuItem{ ID_SETTINGS, L"\xE713", L"설정" });
    float w = S(kMenuW), h = S(kMenuPad) * 2 + S(kMenuItemH) * m_menu.items.size();

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

    auto labels = m_snap.Paint(w, h, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> ink, dim;
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.94f), ink.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.72f), dim.put());
        for (size_t i = 0; i < m_menu.items.size(); ++i)
        {
            auto& item = m_menu.items[i];
            float top = S(kMenuPad) + S(kMenuItemH) * i;
            D2D1_RECT_F iconRect{ S(14), top, S(40), top + S(kMenuItemH) };
            D2D1_RECT_F textRect{ S(42), top, w - S(10), top + S(kMenuItemH) };
            dc->DrawTextW(item.glyph, 1, icon.get(), iconRect, dim.get());
            dc->DrawTextW(item.label, static_cast<UINT32>(wcslen(item.label)), text.get(), textRect, ink.get());
        }
    });

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

int Stage::MenuItemAt(POINT viewPt) const
{
    float x = viewPt.x - m_menu.origin.x, y = viewPt.y - m_menu.origin.y - S(kMenuPad);
    if (x < 0 || x > S(kMenuW) || y < 0)
        return -1;
    int item = static_cast<int>(y / S(kMenuItemH));
    return item < static_cast<int>(m_menu.items.size()) ? item : -1;
}

void Stage::SetMenuHover(int item)
{
    if (item == m_menu.hover)
        return;
    bool wasHidden = m_menu.hover < 0;
    m_menu.hover = item;
    auto fade = m_compositor.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1.f, item >= 0 ? 1.f : 0.f);
    fade.Duration(std::chrono::milliseconds(100));
    m_menu.highlight.StartAnimation(L"Opacity", fade);
    if (item < 0)
        return;
    float3 to{ S(kMenuPad), S(kMenuPad) + S(kMenuItemH) * item, 0.f };
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
    if (command == ID_SETTINGS)
        OpenSettings();
    else if (command == ID_PIN || command == ID_UNPIN)
        SetPinned(*card, command == ID_PIN);
    else if (command == ID_CLOSE)
        PostMessageW(card->hwnd, WM_CLOSE, 0, 0);   // the card goes away when the window is destroyed
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
            m_pinnedApps.push_back(PinKey(c));
        }
        else
        {
            // Look the entry up with the desktop it was pinned on, before refreshing it.
            auto it = std::find_if(m_pinnedApps.begin(), m_pinnedApps.end(), [&](auto const& e) { return PinMatches(e, c); });
            if (it != m_pinnedApps.end())
                m_pinnedApps.erase(it);
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
    }
}

void Stage::OnForeground(HWND hwnd)
{
    Trace(L"foreground", hwnd);
    // A dialog stands in for the window it blocks (a file picker in front of its chat window).
    if (HWND owner = wt::ModalOwner(hwnd); owner && (OnStage(owner) || !IsIconic(owner)))
        hwnd = owner;
    CloseCardMenu();
    // Another window took focus (Alt+Tab...): the settings panel steps aside, like a menu.
    // (Focus handed back to the stage window, as after the tray menu, doesn't count.)
    if (m_settings.open && hwnd != m_active)
        CloseSettings();
    if (!m_ready || m_busy)
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
        return;
    }
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
    if (!picture && m_cfg.iconsOnly)
    {
        // No pictures: its icon card flies in instead.
        frame = card->frame;
        picture = m_snap.Placeholder(hwnd, card->w, card->h, PlaceholderScale() * ThumbW() * card->w / CropFor(*card, true).size.x);
    }
    if (picture)
        SetSnapshot(*card, frame, picture);
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
        return _wcsicmp(entry.c_str(), c.app.c_str()) == 0;
    return _wcsicmp(entry.c_str(), PinKey(c).c_str()) == 0;
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
                [&](auto& e) { return e.size() >= app.size() && _wcsicmp(e.c_str() + e.size() - app.size(), app.c_str()) == 0; }))
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
    for (auto& c : m_cards)
        c->desktop = DesktopOf(c->hwnd);
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
        if (!OnStage(card->hwnd) && IsWindow(card->hwnd))
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
            [&](auto const& e) { return e.size() >= c->app.size() && _wcsicmp(e.c_str() + e.size() - c->app.size(), c->app.c_str()) == 0; }))
    {
        c->desktop = DesktopOf(hwnd);
        auto wanted = std::count_if(m_pinnedApps.begin(), m_pinnedApps.end(), [&](auto const& e) { return PinMatches(e, *c); });
        auto have = std::count_if(m_cards.begin(), m_cards.end(), [&](auto& o) { return o->pinned && o->desktop == c->desktop && o->app == c->app; });
        c->pinned = have < wanted;
        // Upgrade an old app-only entry to this window's desktop, so it stops matching elsewhere.
        auto old = std::find_if(m_pinnedApps.begin(), m_pinnedApps.end(),
            [&](auto const& e) { return e.find(L'|') == std::wstring::npos && PinMatches(e, *c); });
        if (c->pinned && old != m_pinnedApps.end() && c->desktop != GUID_NULL)
        {
            *old = PinKey(*c);
            SavePins();
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
        c->snapshot.Surface(m_snap.Placeholder(hwnd, c->w, c->h, PlaceholderScale() * ThumbW() * c->w / CropFor(*c, true).size.x));
        c->hasPicture = true;
    }
    return c;
}

void Stage::SetSnapshot(Card& c, RECT const& frame, wuc::CompositionDrawingSurface const& surface)
{
    if (!surface)
        return;
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
}

void Stage::Relayout(bool animate)
{
    std::stable_partition(m_cards.begin(), m_cards.end(), [](auto& c) { return c->pinned; });
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
            c.snapshot.Surface(m_snap.Placeholder(c.hwnd, c.w, c.h, PlaceholderScale() * ThumbW() * c.w / CropFor(c, true).size.x));
            c.hasPicture = true;
            if (c.side.holder)
                ApplySize(c.side, c);
        }
        Pose target = SlotPose(c, i);
        if (!c.side.holder)
        {
            c.side = MakeVis(c, true);
            m_sideContent.Children().InsertAtTop(c.side.holder);
            ApplyPose(c.side, c, target);
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
    auto next = m_visible[index];
    if (wt::IsCloaked(next->hwnd))
    {
        // On another desktop: activating it makes Windows switch there (with its own animation);
        // SyncDesktop takes over once the switch is seen.
        m_hover = -1;
        BringBack(next->hwnd);
        ForceForeground(next->hwnd);
        return;
    }
    Pose from = SlotPose(*next, index);
    from.center = SideToAnim(from.center);
    if (m_hover == static_cast<int>(index))
        from = HoverPose(*next, index, true), from.center = SideToAnim(from.center);
    m_hover = -1;

    if (next->hwnd == m_active)
        return;
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
            OnOutCaptured(flight, frame, m_snap.Placeholder(h, c.w, c.h, PlaceholderScale() * ThumbW() * c.w / CropFor(c, true).size.x));
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
        m_inBatch.Completed([this](auto&&, auto&&) { OnFlyInDone(); });
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
    PlaceView(0, m_monitor.right - m_monitor.left, m_monitor.bottom - m_monitor.top - 1);
}

// The view in monitor coordinates. Its content is laid out from the monitor's top-left, so when the
// view does not start there (sidebar on the right) the root visual is shifted to match.
void Stage::PlaceView(LONG left, LONG right, LONG height)
{
    SetWindowPos(m_view, HWND_TOPMOST, m_monitor.left + left, m_monitor.top, right - left, height,
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

void Stage::OnOutCaptured(std::shared_ptr<OutFlight> flight, RECT const& frame, wuc::CompositionDrawingSurface const& surface)
{
    if (std::find(m_outs.begin(), m_outs.end(), flight) == m_outs.end())
        return;
    auto card = flight->card;
    SetSnapshot(*card, frame, surface);

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
    flight->batch.Completed([this, flight](auto&&, auto&&) {
        flight->batch = nullptr;
        if (flight->vis.holder)
            m_animStage.Children().Remove(flight->vis.holder);
        flight->vis = {};
        if (flight->card->side.holder)
            flight->card->side.holder.Opacity(1.f);
        StepDone();
    });
    // Hide the real window only once the flying copy is on screen above it.
    m_toMinimize.push_back(card->hwnd);
    SetTimer(m_sidebar, kTimerMinimizeOut, 30, nullptr);
}

void Stage::OnFlyInDone()
{
    m_inBatch = nullptr;
    BringBack(m_inCard->hwnd);
    ForceForeground(m_inCard->hwnd);
    // Give the real window a moment to paint before the flying copy fades away.
    SetTimer(m_sidebar, kTimerFade, kPaintWaitMs, nullptr);
}

void Stage::FadeOutFlyIn()
{
    auto fade = m_compositor.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.f, 1.f);
    fade.InsertKeyFrame(1.f, 0.f);
    fade.Duration(std::chrono::milliseconds(m_cfg.Ms(kFadeMs)));
    m_inBatch = m_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    m_flyIn.holder.StartAnimation(L"Opacity", fade);
    m_inBatch.End();
    m_inBatch.Completed([this](auto&&, auto&&) {
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
    KillTimer(m_sidebar, kTimerWatchdog);
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
    // Foreground lock: a synthetic Alt tap counts as input and lets us hand focus over.
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
    m_inBatch.Completed([this](auto&&, auto&&) { OnFlyInDone(); });
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
            m_pressIndex = HitTest(ViewToSide(lp));
            m_pressPt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        }
        return 0;
    case WM_LBUTTONUP:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
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
    if (msg == m_shellMsg && m_shellMsg)
    {
        if ((wp & 0x7FFF) == HSHELL_WINDOWDESTROYED)
            OnGone(reinterpret_cast<HWND>(lp));
        return 0;
    }
    switch (msg)
    {
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
            // Object-level destroy/hide WinEvents would wake us for every caret, tooltip and menu in the
            // system; the shell hook only reports top-level app windows.
            m_shellMsg = RegisterWindowMessageW(L"SHELLHOOK");
            RegisterShellHookWindow(m_sidebar);
            SetHotkeys(m_dock != DockState::Hidden);
        }
        else if (wp == kTimerActiveSnap)
        {
            HWND h = m_active;
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
                    else if (!m_cfg.keepTray && !IsWindowVisible(h))
                        continue;                   // closed to the tray, and the user wants no card for it
                    else if (!OnStage(h) && std::none_of(m_cards.begin(), m_cards.end(), [h](auto& c) { return c->hwnd == h; }))
                    {
                        m_stage.push_back(h);       // back where it was, then:
                        if (!IsWindowVisible(h))
                            OnMinimizeStart(h);     // put away like a minimize: into the sidebar
                    }
                }
                UpdateDock();
            }
        }
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
            }
        }
        else if (wp == kTimerWatchdog && m_busy)
        {
            // Something went wrong mid-transition (an exception, a lost device, a batch that never
            // completed). Never leave input blocked or the view covering the screen.
            LogError(WM_TIMER, E_FAIL, L"transition watchdog fired");
            if (m_inCard && !IsWindowVisible(m_inCard->hwnd))
                BringBack(m_inCard->hwnd);
            FinishTransition();
            ShrinkView();
        }
        else if (wp == kTimerRecheck)
        {
            HWND fg = GetForegroundWindow();
            if (fg && fg != m_active)
                OnForeground(fg);
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
