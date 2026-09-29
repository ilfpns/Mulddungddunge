#include "Stage.h"
#include "AppBar.h"
#include "WindowTracker.h"

namespace
{
    constexpr UINT WM_APPBAR_CB = WM_APP + 1;
    constexpr UINT WM_TRAY = WM_APP + 2;
    constexpr UINT ID_EXIT = 1;
    constexpr int kHotkeyBase = 100;     // hotkey ids 100..103 = Alt+1..4
    constexpr UINT_PTR kTimerPopulate = 1;
    constexpr UINT_PTR kTimerActiveSnap = 2;
    constexpr UINT kActiveSnapDelayMs = 1200;
    constexpr UINT_PTR kTimerMinimizeOut = 3;
    constexpr UINT_PTR kTimerFade = 4;
    constexpr UINT_PTR kTimerShrink = 5;

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
    m_inst = inst;
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

    ANIMATIONINFO ai{ sizeof(ai) };
    SystemParametersInfoW(SPI_GETANIMATION, sizeof(ai), &ai, 0);
    m_savedMinAnimate = ai.iMinAnimate;
    // While we run, a minimized stage window flies into the sidebar instead of shrinking to the
    // taskbar. Not persisted (no SPIF_UPDATEINIFILE); restored on exit.
    SetMinAnimate(false);
    m_scale = dpiX / 96.f;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
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
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"Stage Manager");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void Stage::ShowTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"종료");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(m_sidebar);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_sidebar, nullptr);
    PostMessageW(m_sidebar, WM_NULL, 0, 0);
    DestroyMenu(menu);
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
float2 Stage::BadgeOffset(Card const& c, Pose const& p) const
{
    Crop k = CropFor(c, p.thumb);
    float halfW = k.size.x * p.scale / 2.f, halfH = k.size.y * p.scale / 2.f;
    float rad = p.angle * 3.14159265f / 180.f;
    float x = -halfW * std::cos(rad), z = halfW * std::sin(rad);    // left edge swings toward the viewer
    float depth = kDepthRatio * S(kThumbW);
    float2 eye{ S(kSidebarW) / 2.f, (m_bar.bottom - m_bar.top) / 2.f };
    float2 corner{ p.center.x + x, p.center.y + halfH };
    float2 seen = eye + (corner - eye) / (1.f - z / depth);
    float2 badgeCenter = seen - p.center + float2{ S(12), -S(8) };
    float2 offset = badgeCenter - float2{ S(kBadge), S(kBadge) } / 2.f;
    offset.x = std::max(offset.x, -p.center.x + S(2));               // never past the bar's left edge
    return offset;
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

    for (HWND h : windows)
    {
        if (h == m_active)
            continue;
        auto card = MakeCard(h);
        if (!IsIconic(h))
        {
            Refresh(*card);
            MinimizeQuiet(h);
        }
        m_cards.push_back(card);
    }
    Relayout(false);
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
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
    if (!wt::IsManageable(hwnd, m_mon))
        return;
    // Already on screen (Alt+Tab, taskbar, a new window): only the previous one needs to leave.
    RemoveCard(hwnd);
    BeginTransition(hwnd, nullptr, {});
}

void Stage::OnMinimizeStart(HWND hwnd)
{
    Trace(m_activeSnap && m_activeSnapHwnd == hwnd ? L"minimize (has snap)" : L"minimize (no snap)", hwnd);
    if (!m_ready || m_busy || hwnd != m_active)
        return;
    // Minimized by the user (button, Win+D, four-finger swipe): it flies into the sidebar from where
    // it was, using the picture taken when it came on stage.
    auto card = TakeCard(hwnd);
    m_cards.insert(m_cards.begin(), card);
    m_active = nullptr;
    Relayout(true);
    if (!m_activeSnap || m_activeSnapHwnd != hwnd)
        return;
    m_busy = true;
    m_inCard = nullptr;
    m_outCard = card;
    m_pending = 1;
    card->side.holder.Opacity(0.f);
    GrowView();
    auto surface = m_activeSnap;
    m_activeSnap = nullptr;
    OnOutCaptured(card, m_activeSnapFrame, surface);
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
    if (hwnd == m_active && !IsWindowVisible(hwnd))
        m_active = nullptr;
    if (m_busy || IsWindowVisible(hwnd))
        return;
    RemoveCard(hwnd);
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
    if (m_active)
        RemoveCard(m_active);
    for (HWND h : wt::EnumManageable(m_mon))
    {
        if (h != m_active && std::none_of(m_cards.begin(), m_cards.end(), [&](auto& c) { return c->hwnd == h; }))
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
        if (card->hwnd != m_active && IsWindow(card->hwnd))
            MinimizeQuiet(card->hwnd);
    });
}

void Stage::RemoveCard(HWND hwnd)
{
    auto it = std::find_if(m_cards.begin(), m_cards.end(), [&](auto& c) { return c->hwnd == hwnd; });
    if (it == m_cards.end())
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
    c->frame = IsIconic(hwnd) ? wt::RestoreRect(hwnd) : wt::FrameRect(hwnd);
    c->w = static_cast<float>(std::max(1L, c->frame.right - c->frame.left));
    c->h = static_cast<float>(std::max(1L, c->frame.bottom - c->frame.top));
    c->snapshot = m_compositor.CreateSurfaceBrush();
    c->snapshot.Stretch(wuc::CompositionStretch::Fill);
    c->icon = m_compositor.CreateSurfaceBrush(m_snap.Icon(hwnd, static_cast<int>(S(kBadge) * 1.5f)));
    return c;
}

void Stage::Refresh(Card& c)
{
    RECT frame = wt::FrameRect(c.hwnd);
    SetSnapshot(c, frame, m_snap.Capture(c.hwnd, frame));
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
    if (c.side.holder)
        ApplySize(c.side, c);
}

void Stage::ApplySize(CardVis const& v, Card const& c)
{
    v.sprite.Size({ c.w, c.h });
    v.sprite.Brush(c.hasSnapshot ? wuc::CompositionBrush(c.snapshot) : wuc::CompositionBrush(m_placeholderBrush));
}

void Stage::SetMinAnimate(bool on)
{
    if (!m_savedMinAnimate)
        return;
    ANIMATIONINFO ai{ sizeof(ai), on ? m_savedMinAnimate : 0 };
    SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, 0);
}

// Minimizes without the system's shrink-to-taskbar animation.
void Stage::MinimizeQuiet(HWND hwnd)
{
    ShowWindowAsync(hwnd, SW_MINIMIZE);
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
}

void Stage::Relayout(bool animate)
{
    m_visible.clear();
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
        // Pushed out of this desktop's sidebar: its snapshot would only hold memory.
        if (here && c->hasSnapshot)
        {
            c->snapshot.Surface(nullptr);
            c->hasSnapshot = false;
            if (c->side.holder)
                ApplySize(c->side, *c);
        }
    }
    for (size_t i = 0; i < m_visible.size(); ++i)
    {
        auto& c = *m_visible[i];
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

std::shared_ptr<Card> Stage::TakeCard(HWND hwnd)
{
    auto it = std::find_if(m_cards.begin(), m_cards.end(), [&](auto& c) { return c->hwnd == hwnd; });
    if (it == m_cards.end())
        return MakeCard(hwnd);
    auto card = *it;
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

    m_cards.erase(std::find(m_cards.begin(), m_cards.end(), next));
    if (next->side.holder)
        m_sideContent.Children().Remove(next->side.holder);
    next->side = {};

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
    HWND prev = m_active;
    m_active = next;

    m_flyOut = {};
    m_flyIn = {};
    m_outCard = nullptr;
    m_inCard = nextCard;
    m_pending = 0;

    if (prev && prev != next && IsWindow(prev) && IsWindowVisible(prev) && !IsIconic(prev) && OnCurrentDesktop(prev))
    {
        m_outCard = TakeCard(prev);
        m_cards.insert(m_cards.begin(), m_outCard);
    }
    Relayout(true);

    if (m_outCard)
    {
        // The sidebar copy stays hidden until the flying copy lands on it. The outgoing window stays
        // visible until its fresh snapshot is ready, so the incoming one can start moving right away.
        ++m_pending;
        m_outCard->side.holder.Opacity(0.f);
        RECT frame = wt::FrameRect(prev);
        bool fresh = m_prefetch && m_prefetchHwnd == prev && GetTickCount64() - m_prefetchAt < kPrefetchMaxAge &&
                     EqualRect(&frame, &m_prefetchFrame);
        if (fresh)
        {
            auto surface = m_prefetch;
            m_prefetch = nullptr;
            OnOutCaptured(m_outCard, frame, surface);
        }
        else if (m_prefetching && m_prefetchHwnd == prev)
            m_outWaitsForPrefetch = true;
        else
            m_snap.CaptureAsync(prev, frame, [this, card = m_outCard, frame](auto const& surface) {
                OnOutCaptured(card, frame, surface);
            });
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
        if (m_outWaitsForPrefetch && m_outCard && m_outCard->hwnd == h)
        {
            m_outWaitsForPrefetch = false;
            m_prefetch = nullptr;
            OnOutCaptured(m_outCard, m_prefetchFrame, surface);
        }
    });
}

void Stage::OnOutCaptured(std::shared_ptr<Card> card, RECT const& frame, wuc::CompositionDrawingSurface const& surface)
{
    if (card != m_outCard)
        return;
    SetSnapshot(*card, frame, surface);

    // Below the incoming copy, so the new window visibly lands on top.
    m_flyOut = MakeVis(*card, false);
    m_animStage.Children().InsertAtBottom(m_flyOut.holder);
    Pose from = PoseForRect(frame, m_monitor, card->w);
    Pose to = SlotPose(*card, 0);
    to.center = SideToAnim(to.center);
    ApplyPose(m_flyOut, *card, from);
    m_outBatch = m_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    AnimatePose(m_flyOut, *card, from, to, kFlyMs);
    m_outBatch.End();
    m_outBatch.Completed([this](auto&&, auto&&) {
        m_outBatch = nullptr;
        if (m_flyOut.holder)
            m_animStage.Children().Remove(m_flyOut.holder);
        m_flyOut = {};
        if (m_outCard && m_outCard->side.holder)
            m_outCard->side.holder.Opacity(1.f);
        StepDone();
    });
    // Hide the real window only once the flying copy is on screen above it.
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
    m_outBatch = nullptr;
    if (m_flyOut.holder)
        m_animStage.Children().Remove(m_flyOut.holder);
    if (m_flyIn.holder)
        m_animStage.Children().Remove(m_flyIn.holder);
    m_flyOut = {};
    m_flyIn = {};
    m_outCard = nullptr;
    m_inCard = nullptr;
    m_pending = 0;
    m_outWaitsForPrefetch = false;
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
        if (!m_busy)
            SetHover(HitTest(ViewToSide(lp)));
        return 0;
    case WM_LBUTTONUP:
        if (!m_busy)
        {
            int index = HitTest(ViewToSide(lp));
            if (index >= 0)
                SwitchTo(static_cast<size_t>(index));
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
        else if (wp == kTimerMinimizeOut && m_outCard)
            ShowWindowAsync(m_outCard->hwnd, SW_MINIMIZE);
        else if (wp == kTimerFade)
            FadeOutFlyIn();
        else if (wp == kTimerShrink && !m_busy)
            ShrinkView();
        return 0;
    case WM_DISPLAYCHANGE:
        Dock();
        Relayout(false);
        return 0;
    }
    return DefWindowProcW(m_sidebar, msg, wp, lp);
}
