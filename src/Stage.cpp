#include "Stage.h"
#include "AppBar.h"

namespace
{
    constexpr UINT WM_APPBAR_CB = WM_APP + 1;
    constexpr UINT WM_TRAY = WM_APP + 2;
    constexpr UINT ID_EXIT = 1;

    constexpr float kSidebarW = 190.f;
    constexpr float kThumbW = 150.f;
    constexpr float kThumbH = 100.f;
    constexpr float kPitch = 128.f;
    constexpr float kTop = 60.f;
    constexpr float kTilt = 28.f;        // degrees
    constexpr float kDepth = 520.f;      // perspective distance
    constexpr float kRadius = 8.f;
    constexpr float kBadge = 34.f;
    constexpr float kHoverGrow = 1.07f;
    constexpr size_t kMaxCards = 4;
    constexpr int kSlideMs = 260;

    Stage* g_stage = nullptr;

    wuc::CompositionEasingFunction MakeEase(wuc::Compositor const& c)
    {
        return c.CreateCubicBezierEasingFunction({ 0.2f, 0.9f }, { 0.25f, 1.f });
    }
}

bool Stage::Init(HINSTANCE inst)
{
    g_stage = this;
    m_inst = inst;

    HMONITOR mon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    m_monitor = mi.rcMonitor;
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    m_scale = dpiX / 96.f;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = SidebarProc;
    wc.lpszClassName = L"StageManagerSidebar";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = AnimProc;
    wc.lpszClassName = L"StageManagerAnim";
    RegisterClassExW(&wc);

    m_sidebar = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED,
        L"StageManagerSidebar", L"Stage Manager", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    m_anim = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP |
        WS_EX_LAYERED | WS_EX_TRANSPARENT,
        L"StageManagerAnim", L"Stage Manager Animation", WS_POPUP,
        m_monitor.left, m_monitor.top, m_monitor.right - m_monitor.left, m_monitor.bottom - m_monitor.top,
        nullptr, nullptr, inst, nullptr);
    if (!m_sidebar || !m_anim)
        return false;
    // Layered + no redirection bitmap: only composition content is drawn, the rest stays see-through.
    SetLayeredWindowAttributes(m_sidebar, 0, 255, LWA_ALPHA);
    SetLayeredWindowAttributes(m_anim, 0, 255, LWA_ALPHA);

    DispatcherQueueOptions opts{ sizeof(opts), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA };
    winrt::check_hresult(CreateDispatcherQueueController(opts,
        reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(m_queue))));

    m_compositor = wuc::Compositor();
    auto interop = m_compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(m_sidebar, TRUE,
        reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(m_sideTarget))));
    winrt::check_hresult(interop->CreateDesktopWindowTarget(m_anim, TRUE,
        reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(m_animTarget))));
    m_sideRoot = m_compositor.CreateContainerVisual();
    m_animRoot = m_compositor.CreateContainerVisual();
    m_sideTarget.Root(m_sideRoot);
    m_animTarget.Root(m_animRoot);
    m_placeholderBrush = m_compositor.CreateColorBrush({ 255, 58, 58, 64 });
    m_ease = MakeEase(m_compositor);

    appbar::Register(m_sidebar, WM_APPBAR_CB);
    Dock();
    ShowWindow(m_sidebar, SW_SHOWNOACTIVATE);
    AddTrayIcon();

    for (int i = 0; i < 4; ++i)
        m_cards.push_back(NewPlaceholder(S(1280), S(800)));
    Relayout(false);
    return true;
}

void Stage::Shutdown()
{
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = m_sidebar;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    appbar::Remove(m_sidebar);
    DestroyWindow(m_anim);
    DestroyWindow(m_sidebar);
}

void Stage::Dock()
{
    m_bar = appbar::Dock(m_sidebar, m_monitor, static_cast<int>(S(kSidebarW)));
    SetWindowPos(m_sidebar, HWND_TOPMOST, m_bar.left, m_bar.top,
        m_bar.right - m_bar.left, m_bar.bottom - m_bar.top, SWP_NOACTIVATE);
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
    return { S(kSidebarW) / 2.f, S(kTop) + i * S(kPitch) + S(kThumbH) / 2.f };
}

float Stage::ThumbScale(Card const& c) const
{
    return std::min(S(kThumbW) / c.w, S(kThumbH) / c.h);
}

Pose Stage::SlotPose(Card const& c, size_t i) const
{
    return { SlotCenter(i), ThumbScale(c), kTilt };
}

std::shared_ptr<Card> Stage::NewPlaceholder(float w, float h)
{
    auto c = std::make_shared<Card>();
    c->w = w;
    c->h = h;
    return c;
}

CardVis Stage::MakeVis(Card const& c, bool withBadge)
{
    CardVis v;
    v.holder = m_compositor.CreateContainerVisual();
    auto persp = float4x4::identity();
    persp.m34 = -1.f / S(kDepth);
    v.holder.TransformMatrix(persp);

    v.sprite = m_compositor.CreateSpriteVisual();
    v.sprite.Size({ c.w, c.h });
    v.sprite.Offset({ -c.w / 2.f, -c.h / 2.f, 0.f });
    v.sprite.CenterPoint({ c.w / 2.f, c.h / 2.f, 0.f });
    v.sprite.RotationAxis({ 0.f, 1.f, 0.f });
    if (c.hasSnapshot)
        v.sprite.Brush(c.snapshot);
    else
        v.sprite.Brush(m_placeholderBrush);
    v.clip = m_compositor.CreateRoundedRectangleGeometry();
    v.clip.Size({ c.w, c.h });
    v.sprite.Clip(m_compositor.CreateGeometricClip(v.clip));
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

void Stage::ApplyPose(CardVis const& v, Card const& c, Pose const& p)
{
    v.holder.Offset({ p.center.x, p.center.y, 0.f });
    v.sprite.Scale({ p.scale, p.scale, 1.f });
    v.sprite.RotationAngleInDegrees(p.angle);
    v.clip.CornerRadius({ S(kRadius) / p.scale, S(kRadius) / p.scale });
    if (v.badge)
    {
        float dw = c.w * p.scale, dh = c.h * p.scale;
        v.badge.Offset({ -dw / 2.f - S(6), dh / 2.f - S(kBadge) * 0.65f, 0.f });
    }
}

void Stage::AnimatePose(CardVis const& v, Card const& c, Pose const& from, Pose const& to, int ms)
{
    std::chrono::milliseconds dur(ms);

    auto off = m_compositor.CreateVector3KeyFrameAnimation();
    off.InsertKeyFrame(0.f, { from.center.x, from.center.y, 0.f });
    off.InsertKeyFrame(1.f, { to.center.x, to.center.y, 0.f }, m_ease);
    off.Duration(dur);
    v.holder.StartAnimation(L"Offset", off);

    auto scale = m_compositor.CreateVector3KeyFrameAnimation();
    scale.InsertKeyFrame(0.f, { from.scale, from.scale, 1.f });
    scale.InsertKeyFrame(1.f, { to.scale, to.scale, 1.f }, m_ease);
    scale.Duration(dur);
    v.sprite.StartAnimation(L"Scale", scale);

    auto angle = m_compositor.CreateScalarKeyFrameAnimation();
    angle.InsertKeyFrame(0.f, from.angle);
    angle.InsertKeyFrame(1.f, to.angle, m_ease);
    angle.Duration(dur);
    v.sprite.StartAnimation(L"RotationAngleInDegrees", angle);

    auto radius = m_compositor.CreateVector2KeyFrameAnimation();
    radius.InsertKeyFrame(0.f, { S(kRadius) / from.scale, S(kRadius) / from.scale });
    radius.InsertKeyFrame(1.f, { S(kRadius) / to.scale, S(kRadius) / to.scale }, m_ease);
    radius.Duration(dur);
    v.clip.StartAnimation(L"CornerRadius", radius);

    if (v.badge)
    {
        float dw = c.w * to.scale, dh = c.h * to.scale;
        auto badge = m_compositor.CreateVector3KeyFrameAnimation();
        badge.InsertKeyFrame(1.f, { -dw / 2.f - S(6), dh / 2.f - S(kBadge) * 0.65f, 0.f }, m_ease);
        badge.Duration(dur);
        v.badge.StartAnimation(L"Offset", badge);
    }
}

void Stage::Relayout(bool animate)
{
    for (size_t i = 0; i < m_cards.size(); ++i)
    {
        auto& c = *m_cards[i];
        if (i >= kMaxCards)
        {
            if (c.side.holder)
                c.side.holder.IsVisible(false);
            continue;
        }
        Pose target = SlotPose(c, i);
        if (!c.side.holder)
        {
            c.side = MakeVis(c, true);
            m_sideRoot.Children().InsertAtTop(c.side.holder);
            ApplyPose(c.side, c, target);
            continue;
        }
        c.side.holder.IsVisible(true);
        float2 now{ c.side.holder.Offset().x, c.side.holder.Offset().y };
        if (animate && (now.x != target.center.x || now.y != target.center.y))
            AnimatePose(c.side, c, { now, target.scale, target.angle }, target, kSlideMs);
        else
            ApplyPose(c.side, c, target);
    }
}

int Stage::HitTest(POINT pt) const
{
    size_t n = std::min(m_cards.size(), kMaxCards);
    for (size_t i = 0; i < n; ++i)
    {
        auto& c = *m_cards[i];
        float2 ctr = SlotCenter(i);
        float s = ThumbScale(c);
        if (std::abs(pt.x - ctr.x) <= c.w * s / 2.f && std::abs(pt.y - ctr.y) <= c.h * s / 2.f)
            return static_cast<int>(i);
    }
    return -1;
}

void Stage::SetHover(int index)
{
    if (index == m_hover)
        return;
    auto grow = [&](int i, float factor) {
        if (i < 0 || i >= static_cast<int>(std::min(m_cards.size(), kMaxCards)))
            return;
        auto& c = *m_cards[i];
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

// ---- window procs ---------------------------------------------------------

LRESULT CALLBACK Stage::SidebarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_stage && g_stage->m_sidebar == hwnd)
        return g_stage->OnSidebarMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK Stage::AnimProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST)
        return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Stage::OnSidebarMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        if (!m_tracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_sidebar, 0 };
            m_tracking = TrackMouseEvent(&tme);
        }
        SetHover(HitTest({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }));
        return 0;
    case WM_MOUSELEAVE:
        m_tracking = false;
        SetHover(-1);
        return 0;
    case WM_APPBAR_CB:
        if (wp == ABN_POSCHANGED)
            Dock();
        else if (wp == ABN_FULLSCREENAPP)
            ShowWindow(m_sidebar, lp ? SW_HIDE : SW_SHOWNOACTIVATE);
        return 0;
    case WM_ACTIVATE:
        appbar::NotifyActivate(m_sidebar);
        break;
    case WM_WINDOWPOSCHANGED:
        appbar::NotifyPosChanged(m_sidebar);
        break;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP)
            ShowTrayMenu();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_EXIT)
            PostQuitMessage(0);
        return 0;
    case WM_DISPLAYCHANGE:
        Dock();
        Relayout(false);
        return 0;
    }
    return DefWindowProcW(m_sidebar, msg, wp, lp);
}
