#include "Stage.h"
#include "AppBar.h"
#include "WindowTracker.h"

namespace
{
    constexpr UINT WM_APPBAR_CB = WM_APP + 1;
    constexpr UINT WM_TRAY = WM_APP + 2;
    constexpr UINT ID_EXIT = 1;
    constexpr UINT ID_PIN = 2;
    constexpr UINT ID_UNPIN = 3;
    constexpr UINT ID_CLOSE = 4;
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

    constexpr float kSidebarW = 210.f;
    constexpr float kThumbW = 210.f;     // card size before the tilt foreshortens it; every card has this shape
    constexpr float kThumbH = 140.f;
    constexpr float kPitch = 172.f;
    constexpr float kTilt = 42.f;        // degrees
    constexpr float kDepthRatio = 2.4f;  // camera distance relative to card width
    constexpr float kRadius = 8.f;
    constexpr float kBadge = 34.f;
    constexpr float kHoverGrow = 1.07f;
    constexpr size_t kMaxCards = 4;
    constexpr int kSlideMs = 260;
    constexpr int kFlyMs = 420;
    constexpr int kFadeMs = 150;
    constexpr int kPaintWaitMs = 110;    // restored windows need a moment to repaint before the copy fades
    constexpr ULONGLONG kPrefetchMaxAge = 4000;
    constexpr float kDragStart = 8.f;    // px of movement before a press turns into a drag
    constexpr ULONGLONG kQuietMs = 700;

    Stage* g_stage = nullptr;


    // Opt-in event trace for diagnosing window-manager interactions: create %TEMP%\stage-manager.trace
    // before starting and events are appended to it. Off (one check at startup) otherwise.
    bool g_trace = false;

    void Trace(wchar_t const* what, HWND hwnd = nullptr)
    {
        if (!g_trace)
            return;
        wchar_t path[MAX_PATH], title[64]{};
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"stage-manager.trace");
        if (hwnd)
            GetWindowTextW(hwnd, title, ARRAYSIZE(title));
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"a, ccs=UTF-8") == 0 && f)
        {
            fwprintf(f, L"%llu %s %p '%s'\n", GetTickCount64(), what, hwnd, title);
            fclose(f);
        }
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

bool Stage::Init(HINSTANCE inst)
{
    g_stage = this;
    {
        wchar_t path[MAX_PATH];
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"stage-manager.trace");
        g_trace = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    }

    m_mon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
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
    LoadPins();
    m_desktops = winrt::try_create_instance<IVirtualDesktopManager>(CLSID_VirtualDesktopManager);

    appbar::Register(m_sidebar, WM_APPBAR_CB);
    Dock();
    ShrinkView();
    AddTrayIcon();

    // Give maximized windows a moment to shrink out of the newly reserved strip before capturing them.
    SetTimer(m_sidebar, kTimerPopulate, 400, nullptr);
    return true;
}

void Stage::Shutdown()
{
    SetHotkeys(false);
    for (auto hook : m_hooks)
        UnhookWinEvent(hook);
    DeregisterShellHookWindow(m_sidebar);
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = m_sidebar;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    appbar::Remove(m_sidebar);
    SetMinAnimate(true);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kStateKey, L"MinAnimate");
    DestroyWindow(m_view);
    DestroyWindow(m_sidebar);
}

void Stage::Dock()
{
    m_bar = appbar::Dock(m_sidebar, m_monitor, static_cast<int>(S(kSidebarW)));

    float2 barSize{ static_cast<float>(m_bar.right - m_bar.left), static_cast<float>(m_bar.bottom - m_bar.top) };
    float2 barOrigin = SideToAnim({ 0.f, 0.f });
    m_sideContent.Offset({ barOrigin.x, barOrigin.y, 0.f });
    if (!m_busy)
        ShrinkView();

    float2 eye{ S(kSidebarW) / 2.f, barSize.y / 2.f };
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
    AppendMenuW(menu, MF_STRING | (StartsWithWindows() ? MF_CHECKED : 0), ID_AUTOSTART, L"Windows 시작 시 실행");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"종료");
    UINT command = 0;
    ShowMenu(menu, &command);
    if (command == ID_AUTOSTART)
        SetStartsWithWindows(!StartsWithWindows());
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

void Stage::OpenCardMenu(size_t index)
{
    if (m_menu.open)
        CloseCardMenu();
    auto card = m_visible[index];
    m_menu = {};
    m_menu.open = true;
    m_menu.card = card;
    m_menu.items = {
        card->pinned ? MenuItem{ ID_UNPIN, L"\xE77A", L"고정 해제" } : MenuItem{ ID_PIN, L"\xE718", L"탭 고정" },
        MenuItem{ ID_CLOSE, L"\xE8BB", L"창 닫기" },
    };
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
    float2 cardCenter = SideToAnim(SlotCenter(index));
    float x = static_cast<float>(m_bar.right - m_monitor.left) + S(6);
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
        if (!m_busy && !m_dragging && !m_menu.open)
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
    if (command == ID_PIN || command == ID_UNPIN)
        SetPinned(*card, command == ID_PIN);
    else if (command == ID_CLOSE)
        PostMessageW(card->hwnd, WM_CLOSE, 0, 0);   // the card goes away when the window is destroyed
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
            m_pinnedApps.push_back(c.app);
        else
        {
            auto it = std::find_if(m_pinnedApps.begin(), m_pinnedApps.end(),
                [&](auto const& app) { return _wcsicmp(app.c_str(), c.app.c_str()) == 0; });
            if (it != m_pinnedApps.end())
                m_pinnedApps.erase(it);
        }
        SavePins();
    }
    if (c.side.pin)
        c.side.pin.IsVisible(pinned);
    m_hover = -1;
    Relayout(true);
}

// ---- layout ---------------------------------------------------------------

float2 Stage::SlotCenter(size_t i) const
{
    // The stack is centered vertically in the bar.
    float n = static_cast<float>(m_visible.size());
    float barH = static_cast<float>(m_bar.bottom - m_bar.top);
    return { S(kSidebarW) / 2.f, barH / 2.f + (i - (n - 1.f) / 2.f) * S(kPitch) };
}

float Stage::ThumbScale(Card const& c) const
{
    return S(kThumbW) / CropFor(c, true).size.x;
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
    float2 eye{ S(kSidebarW) / 2.f, (m_bar.bottom - m_bar.top) / 2.f };
    float2 point = p.center + local;
    return eye + (point - eye) / (1.f - z / (kDepthRatio * S(kThumbW))) - p.center;
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
    return offset;
}

float2 Stage::PinOffset(Card const& c, Pose const& p) const
{
    Crop k = CropFor(c, p.thumb);
    float halfW = k.size.x * p.scale / 2.f, halfH = k.size.y * p.scale / 2.f;
    float rad = p.angle * 3.14159265f / 180.f;
    float2 corner = Project(p, { halfW * std::cos(rad), -halfH }, -halfW * std::sin(rad));
    return corner + float2{ -S(14), S(10) } - float2{ S(4), S(4) };
}

float4x4 Stage::Perspective(float2 eye) const
{
    using namespace winrt::Windows::Foundation::Numerics;
    auto p = float4x4::identity();
    p.m34 = -1.f / (kDepthRatio * S(kThumbW));
    return make_float4x4_translation(-eye.x, -eye.y, 0.f) * p * make_float4x4_translation(eye.x, eye.y, 0.f);
}

Pose Stage::SlotPose(Card const& c, size_t i) const
{
    return { SlotCenter(i), ThumbScale(c), kTilt, true };
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
    Relayout(false);
}

// ---- outside changes -------------------------------------------------------

void CALLBACK Stage::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (!g_stage || idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd)
        return;
    switch (event)
    {
    case EVENT_SYSTEM_FOREGROUND:
        g_stage->OnForeground(hwnd);
        break;
    case EVENT_SYSTEM_MINIMIZESTART:
        g_stage->OnMinimizeStart(hwnd);
        break;
    }
}

void Stage::OnForeground(HWND hwnd)
{
    Trace(L"foreground", hwnd);
    CloseCardMenu();
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
        return;
    if (OnStage(hwnd))
    {
        m_stage.erase(std::find(m_stage.begin(), m_stage.end(), hwnd));
        m_stage.push_back(hwnd);
        m_active = hwnd;
        SnapActiveSoon();
        return;
    }
    if (!wt::IsManageable(hwnd, m_mon))
        return;
    // Already on screen (Alt+Tab, taskbar, a new window): only the previous one needs to leave.
    RemoveCard(hwnd);
    BeginTransition(hwnd, nullptr, {});
}

void Stage::OnMinimizeStart(HWND hwnd)
{
    Trace(m_activeSnap && m_activeSnapHwnd == hwnd ? L"minimize (has snap)" : L"minimize (no snap)", hwnd);
    if (!m_ready || m_busy || !OnStage(hwnd))
        return;
    // Minimized by the user (button, Win+D, four-finger swipe): it flies into the sidebar from where
    // it was, using the picture taken when it became the focused stage window.
    LeaveStage(hwnd);
    auto card = TakeCard(hwnd);
    if (!card->pinned)
        m_cards.insert(m_cards.begin(), card);
    Relayout(true);
    // No flight without a picture to fly, or without a visible sidebar card to land on (the card can
    // be out of view, e.g. while Windows is still moving the window between desktops).
    if (!m_activeSnap || m_activeSnapHwnd != hwnd || !card->side.holder)
        return;
    m_busy = true;
    m_inCard = nullptr;
    auto flight = std::make_shared<OutFlight>();
    flight->card = card;
    m_outs = { flight };
    m_pending = 1;
    card->side.holder.Opacity(0.f);
    GrowView();
    auto surface = m_activeSnap;
    m_activeSnap = nullptr;
    OnOutCaptured(flight, m_activeSnapFrame, surface);
}

void Stage::SnapActiveSoon()
{
    m_activeSnap = nullptr;
    KillTimer(m_sidebar, kTimerActiveSnap);
    if (m_active)
        SetTimer(m_sidebar, kTimerActiveSnap, kActiveSnapDelayMs, nullptr);
}

void Stage::OnGone(HWND hwnd)
{
    Trace(L"gone", hwnd);
    if (!IsWindowVisible(hwnd))
        LeaveStage(hwnd);
    if (m_busy || IsWindowVisible(hwnd))
        return;
    RemoveCard(hwnd, true);
}

bool Stage::OnStage(HWND hwnd) const
{
    return hwnd && std::find(m_stage.begin(), m_stage.end(), hwnd) != m_stage.end();
}

void Stage::LeaveStage(HWND hwnd)
{
    auto it = std::find(m_stage.begin(), m_stage.end(), hwnd);
    if (it != m_stage.end())
        m_stage.erase(it);
    if (hwnd == m_active)
        m_active = m_stage.empty() ? nullptr : m_stage.back();
}

bool Stage::OnCurrentDesktop(HWND hwnd) const
{
    BOOL on = TRUE;
    if (m_desktops && FAILED(m_desktops->IsWindowOnCurrentVirtualDesktop(hwnd, &on)))
        return true;
    return on != FALSE;
}

GUID Stage::CurrentDesktopId()
{
    GUID id{};
    DWORD size = sizeof(id);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops",
        L"CurrentVirtualDesktop", RRF_RT_REG_BINARY, nullptr, &id, &size);
    return id;
}

// After switching desktops the previous stage window stays put on its own desktop; the new desktop's
// windows take over the sidebar, and ones never seen before are adopted.
void Stage::SyncDesktop()
{
    Trace(L"desktop switched");
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
    m_hover = -1;
    Relayout(false);
    SnapActiveSoon();
}

// A window seen for the first time on this desktop joins the sidebar like at startup.
void Stage::Adopt(HWND hwnd)
{
    auto card = MakeCard(hwnd);
    m_cards.push_back(card);
    if (IsIconic(hwnd))
        return;
    RECT frame = wt::FrameRect(hwnd);
    m_snap.CaptureAsync(hwnd, frame, [this, card, frame](auto const& surface) {
        SetSnapshot(*card, frame, surface);
        if (!OnStage(card->hwnd) && IsWindow(card->hwnd))
            ShowWindowAsync(card->hwnd, SW_MINIMIZE);
    });
}

void Stage::RemoveCard(HWND hwnd, bool evenIfPinned)
{
    auto it = std::find_if(m_cards.begin(), m_cards.end(), [&](auto& c) { return c->hwnd == hwnd; });
    if (it == m_cards.end() || ((*it)->pinned && !evenIfPinned))
        return;
    if ((*it)->side.holder)
        m_sideContent.Children().Remove((*it)->side.holder);
    m_cards.erase(it);
    m_hover = -1;
    Relayout(true);
}

std::shared_ptr<Card> Stage::MakeCard(HWND hwnd)
{
    auto c = std::make_shared<Card>();
    c->hwnd = hwnd;
    c->app = wt::ProcessPath(hwnd);
    // Pinned if this app has more pin entries than it currently has pinned cards.
    auto same = [&](auto const& app) { return !c->app.empty() && _wcsicmp(app.c_str(), c->app.c_str()) == 0; };
    auto wanted = std::count_if(m_pinnedApps.begin(), m_pinnedApps.end(), same);
    auto have = std::count_if(m_cards.begin(), m_cards.end(), [&](auto& o) { return o->pinned && same(o->app); });
    c->pinned = have < wanted;
    c->frame = IsIconic(hwnd) ? wt::RestoreRect(hwnd) : wt::FrameRect(hwnd);
    c->w = static_cast<float>(std::max(1L, c->frame.right - c->frame.left));
    c->h = static_cast<float>(std::max(1L, c->frame.bottom - c->frame.top));
    c->snapshot = m_compositor.CreateSurfaceBrush();
    c->snapshot.Stretch(wuc::CompositionStretch::Fill);
    c->icon = m_compositor.CreateSurfaceBrush(m_snap.Icon(hwnd, static_cast<int>(std::lround(S(kBadge)))));
    c->snapshot.Surface(m_snap.Placeholder(hwnd, c->w, c->h, S(kThumbW) * c->w / CropFor(*c, true).size.x));
    c->hasPicture = true;
    return c;
}

void Stage::SetSnapshot(Card& c, RECT const& frame, wuc::CompositionDrawingSurface const& surface)
{
    if (!surface)
        return;
    if (!IsZoomed(c.hwnd))
    {
        RECT wr;
        GetWindowRect(c.hwnd, &wr);
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
        v.pin.Size({ S(8), S(8) });
        v.pin.Brush(m_compositor.CreateColorBrush({ 230, 255, 255, 255 }));
        auto dot = m_compositor.CreateEllipseGeometry();
        dot.Center({ S(4), S(4) });
        dot.Radius({ S(4), S(4) });
        v.pin.Clip(m_compositor.CreateGeometricClip(dot));
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
        bool here = OnCurrentDesktop(c->hwnd);
        if (here && m_visible.size() < kMaxCards)
        {
            m_visible.push_back(c);
            continue;
        }
        if (c->side.holder)
            c->side.holder.IsVisible(false);
        bool shown = false;
        GUID desk{};
        if (!here && m_desktops && SUCCEEDED(m_desktops->GetWindowDesktopId(c->hwnd, &desk)))
        {
            auto it = std::find_if(perDesktop.begin(), perDesktop.end(), [&](auto& d) { return d.first == desk; });
            if (it == perDesktop.end())
                it = perDesktop.insert(perDesktop.end(), { desk, 0 });
            shown = it->second++ < kMaxCards;
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
        if (!c.hasPicture)
        {
            // Back in view after its picture was released.
            c.snapshot.Surface(m_snap.Placeholder(c.hwnd, c.w, c.h, S(kThumbW) * c.w / CropFor(c, true).size.x));
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
            AnimatePose(c.side, c, { now, target.scale, target.angle, true }, target, kSlideMs);
        else
            ApplyPose(c.side, c, target);
    }
}

int Stage::HitTest(POINT pt) const
{
    for (size_t i = 0; i < m_visible.size(); ++i)
    {
        float2 ctr = SlotCenter(i);
        if (std::abs(pt.x - ctr.x) <= S(kThumbW) / 2.f && std::abs(pt.y - ctr.y) <= S(kThumbH) / 2.f)
            return static_cast<int>(i);
    }
    return -1;
}

void Stage::SetHover(int index)
{
    if (index == m_hover)
        return;
    auto grow = [&](int i, float factor) {
        if (i < 0 || i >= static_cast<int>(m_visible.size()))
            return;
        auto& c = *m_visible[i];
        float s = ThumbScale(c) * factor;
        auto a = m_compositor.CreateVector3KeyFrameAnimation();
        a.InsertKeyFrame(1.f, { s, s, 1.f }, m_ease);
        a.Duration(std::chrono::milliseconds(150));
        c.side.sprite.StartAnimation(L"Scale", a);
    };
    grow(m_hover, 1.f);
    grow(index, kHoverGrow);
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
        return MakeCard(hwnd);
    auto card = *it;
    if (!card->pinned)
        m_cards.erase(it);
    return card;
}

void Stage::SwitchTo(size_t index)
{
    auto next = m_visible[index];
    Pose from = SlotPose(*next, index);
    from.center = SideToAnim(from.center);
    if (m_hover == static_cast<int>(index))
        from.scale *= kHoverGrow;
    m_hover = -1;

    if (next->hwnd == m_active)
        return;
    if (!next->pinned)
    {
        m_cards.erase(std::find(m_cards.begin(), m_cards.end(), next));
        if (next->side.holder)
            m_sideContent.Children().Remove(next->side.holder);
        next->side = {};
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
    std::vector<HWND> leaving;
    for (HWND h : m_stage)
    {
        if (h != next && IsWindow(h) && IsWindowVisible(h) && !IsIconic(h) && OnCurrentDesktop(h))
            leaving.push_back(h);
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
        ShowWindowAsync(flight->card->hwnd, SW_MINIMIZE);
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
        if (fresh)
        {
            auto surface = m_prefetch;
            m_prefetch = nullptr;
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
        AnimatePose(m_flyIn, *m_inCard, nextFrom, to, kFlyMs);
        m_inBatch.End();
        m_inBatch.Completed([this](auto&&, auto&&) { OnFlyInDone(); });
    }
    if (!m_pending)
    {
        FinishTransition();
        return;
    }
    GrowView();
}

// The view's origin stays at the monitor's top-left, so resizing never moves any content.
void Stage::GrowView()
{
    KillTimer(m_sidebar, kTimerShrink);
    // One pixel short of the monitor: a borderless topmost window covering it exactly is taken by the
    // shell for a fullscreen app, which made us tuck our own sidebar away mid-transition (the flicker).
    SetWindowPos(m_view, HWND_TOPMOST, m_monitor.left, m_monitor.top, m_monitor.right - m_monitor.left,
        m_monitor.bottom - m_monitor.top - 1, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Stage::ShrinkView()
{
    // Covering only the bar keeps us off other apps' pixels, so their overlay/flip optimizations stay intact.
    SetWindowPos(m_view, HWND_TOPMOST, m_monitor.left, m_monitor.top, m_bar.right - m_monitor.left,
        m_bar.bottom - m_monitor.top, SWP_NOACTIVATE | (m_tucked ? 0 : SWP_SHOWWINDOW));
}

// Slides the cards off the left edge and then hides the window entirely, so nothing of ours sits
// above a fullscreen game or video; slides back in when it exits.
void Stage::SlideSidebar(bool out)
{
    if (out == m_tucked)
        return;
    m_tucked = out;
    // While a fullscreen app runs, Alt+number belongs to it (VS Code tabs, JetBrains tool windows...).
    SetHotkeys(!out);
    float hidden = -static_cast<float>(m_bar.right - m_bar.left);
    auto anim = m_compositor.CreateScalarKeyFrameAnimation();
    anim.Duration(std::chrono::milliseconds(kSlideMs));
    if (!out)
        ShowWindow(m_view, SW_SHOWNOACTIVATE);
    float base = SideToAnim({ 0.f, 0.f }).x;
    anim.InsertKeyFrame(0.f, base + (out ? 0.f : hidden));
    anim.InsertKeyFrame(1.f, base + (out ? hidden : 0.f), m_ease);
    m_slideBatch = m_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    m_sideContent.StartAnimation(L"Offset.X", anim);
    m_slideBatch.End();
    m_slideBatch.Completed([this](auto&&, auto&&) {
        m_slideBatch = nullptr;
        if (m_tucked)
            ShowWindow(m_view, SW_HIDE);
    });
}

void Stage::SetHotkeys(bool on)
{
    for (int i = 0; i < static_cast<int>(kMaxCards); ++i)
    {
        if (on && !RegisterHotKey(m_sidebar, kHotkeyBase + i, MOD_ALT | MOD_NOREPEAT, '1' + i))
            LogError(WM_HOTKEY, HRESULT_FROM_WIN32(GetLastError()), L"Alt+number already registered by another app");
        else if (!on)
            UnregisterHotKey(m_sidebar, kHotkeyBase + i);
    }
}

void Stage::Prefetch()
{
    HWND h = m_active;
    if (m_busy || m_prefetching || !h || !IsWindowVisible(h) || IsIconic(h))
        return;
    if (h == m_prefetchHwnd && m_prefetch && GetTickCount64() - m_prefetchAt < kPrefetchMaxAge / 2)
        return;
    m_prefetching = true;
    m_prefetchHwnd = h;
    m_prefetchFrame = wt::FrameRect(h);
    m_snap.CaptureAsync(h, m_prefetchFrame, [this, h](auto const& surface) {
        m_prefetching = false;
        m_prefetch = surface;
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
    AnimatePose(flight->vis, *card, from, to, kFlyMs);
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
    if (IsIconic(m_inCard->hwnd))
        ShowWindowAsync(m_inCard->hwnd, SW_RESTORE);
    ForceForeground(m_inCard->hwnd);
    // Give the real window a moment to paint before the flying copy fades away.
    SetTimer(m_sidebar, kTimerFade, kPaintWaitMs, nullptr);
}

void Stage::FadeOutFlyIn()
{
    auto fade = m_compositor.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.f, 1.f);
    fade.InsertKeyFrame(1.f, 0.f);
    fade.Duration(std::chrono::milliseconds(kFadeMs));
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
    m_busy = false;
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

void Stage::ForceForeground(HWND hwnd)
{
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
    SetHover(-1);
    if (!m_dragCard->pinned && m_dragCard->side.holder)
        m_dragCard->side.holder.Opacity(0.f);
    m_dragVis = MakeVis(*m_dragCard, false);
    m_animStage.Children().InsertAtTop(m_dragVis.holder);
    Pose from = SlotPose(*m_dragCard, m_pressIndex);
    from.center = SideToAnim(from.center);
    // Lifted: it straightens out and grows a little while following the pointer.
    m_dragPose = { { static_cast<float>(viewPt.x), static_cast<float>(viewPt.y) }, from.scale * 1.25f, 0.f, true };
    ApplyPose(m_dragVis, *m_dragCard, from);
    AnimatePose(m_dragVis, *m_dragCard, from, m_dragPose, 180);
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
    bool onStage = !cancel && viewPt.x > m_bar.right - m_monitor.left && IsWindow(card->hwnd);
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
    AnimatePose(vis, *card, m_dragPose, home, kSlideMs);
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
        if (card->side.holder)
            m_sideContent.Children().Remove(card->side.holder);
        card->side = {};
    }
    Relayout(true);

    RECT frame{ target.left + card->border.left, target.top + card->border.top,
                target.right - card->border.right, target.bottom - card->border.bottom };
    m_stage.push_back(h);
    m_active = h;
    m_inCard = card;
    m_flyIn = m_dragVis;
    m_dragVis = {};
    m_outs.clear();
    m_pending = 1;
    m_inBatch = m_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    AnimatePose(m_flyIn, *card, m_dragPose, PoseForRect(frame, m_monitor, card->w), kFlyMs);
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
            int index = HitTest(ViewToSide(lp));
            if (index >= 0)
                OpenCardMenu(static_cast<size_t>(index));
        }
        return 0;
    case WM_MOUSELEAVE:
        m_tracking = false;
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
        if (!m_busy && !m_tucked && index < m_visible.size())
        {
            Prefetch();     // no hover happened; the outgoing window is captured on the way
            SwitchTo(index);
        }
        return 0;
    }
    case WM_APPBAR_CB:
        if (wp == ABN_POSCHANGED)
            Dock();
        else if (wp == ABN_FULLSCREENAPP)
        {
            // Trust it only when the foreground window really is a fullscreen app (not us).
            HWND fg = GetForegroundWindow();
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            bool real = lp && pid != GetCurrentProcessId() && wt::IsFullscreen(fg);
            SlideSidebar(real);
        }
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP)
            ShowTrayMenu();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_EXIT)
            PostQuitMessage(0);
        return 0;
    case WM_TIMER:
        KillTimer(m_sidebar, wp);
        if (wp == kTimerPopulate)
        {
            Populate();
            SnapActiveSoon();
            SetTimer(m_sidebar, kTimerTrim, 3000, nullptr);     // after the startup captures have landed
            m_desktopId = CurrentDesktopId();
            m_ready = true;
            m_quietUntil = GetTickCount64() + kQuietMs;
            DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
            m_hooks.push_back(SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0, flags));
            m_hooks.push_back(SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr, WinEventProc, 0, 0, flags));
            // Object-level destroy/hide WinEvents would wake us for every caret, tooltip and menu in the
            // system; the shell hook only reports top-level app windows.
            m_shellMsg = RegisterWindowMessageW(L"SHELLHOOK");
            RegisterShellHookWindow(m_sidebar);
            SetHotkeys(true);
        }
        else if (wp == kTimerActiveSnap)
        {
            HWND h = m_active;
            if (h && !m_busy && IsWindowVisible(h) && !IsIconic(h))
            {
                RECT frame = wt::FrameRect(h);
                m_snap.CaptureAsync(h, frame, [this, h, frame](auto const& surface) {
                    if (!surface || h != m_active)
                        return;
                    Trace(L"stage window photographed", h);
                    m_activeSnap = surface;
                    m_activeSnapHwnd = h;
                    m_activeSnapFrame = frame;
                });
            }
        }
        else if (wp == kTimerMinimizeOut)
        {
            for (HWND h : m_toMinimize)
                ShowWindowAsync(h, SW_MINIMIZE);
            m_toMinimize.clear();
        }
        else if (wp == kTimerFade)
            FadeOutFlyIn();
        else if (wp == kTimerShrink && !m_busy && !m_dragging && !m_menu.open)
            ShrinkView();
        else if (wp == kTimerTrim)
            SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
        return 0;
    case WM_DISPLAYCHANGE:
        Dock();
        Relayout(false);
        return 0;
    }
    return DefWindowProcW(m_sidebar, msg, wp, lp);
}
