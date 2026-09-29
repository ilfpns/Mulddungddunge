#include "Stage.h"
#include "AppBar.h"
#include "WindowTracker.h"

namespace
{
    constexpr UINT WM_APPBAR_CB = WM_APP + 1;
    constexpr UINT WM_TRAY = WM_APP + 2;
    constexpr UINT ID_EXIT = 1;
    constexpr UINT_PTR kTimerPopulate = 1;
    constexpr UINT_PTR kTimerMinAnimate = 2;
    constexpr UINT_PTR kTimerMinimizeOut = 3;
    constexpr UINT_PTR kTimerFade = 4;

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
    constexpr int kFlyMs = 380;
    constexpr int kFadeMs = 120;

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

    m_mon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(m_mon, &mi);
    m_monitor = mi.rcMonitor;
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(m_mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);

    ANIMATIONINFO ai{ sizeof(ai) };
    SystemParametersInfoW(SPI_GETANIMATION, sizeof(ai), &ai, 0);
    m_savedMinAnimate = ai.iMinAnimate;
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

    m_sidebar = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        L"StageManagerSidebar", L"Stage Manager", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    m_anim = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP |
        WS_EX_LAYERED | WS_EX_TRANSPARENT,
        L"StageManagerAnim", L"Stage Manager Animation", WS_POPUP,
        m_monitor.left, m_monitor.top, m_monitor.right - m_monitor.left, m_monitor.bottom - m_monitor.top,
        nullptr, nullptr, inst, nullptr);
    if (!m_sidebar || !m_anim)
        return false;
    // The animation layer must let clicks through; layered + transparent does that.
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
    m_sideContent = m_compositor.CreateContainerVisual();
    m_sideRoot.Children().InsertAtTop(m_sideContent);
    m_animTarget.Root(m_animRoot);
    m_placeholderBrush = m_compositor.CreateColorBrush({ 255, 58, 58, 64 });
    m_ease = MakeEase(m_compositor);
    m_snap.Init(m_compositor);

    appbar::Register(m_sidebar, WM_APPBAR_CB);
    Dock();
    ShowWindow(m_sidebar, SW_SHOWNOACTIVATE);
    AddTrayIcon();

    // Give maximized windows a moment to shrink out of the newly reserved strip before capturing them.
    SetTimer(m_sidebar, kTimerPopulate, 400, nullptr);
    return true;
}

void Stage::Shutdown()
{
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = m_sidebar;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    appbar::Remove(m_sidebar);
    SetMinAnimate(true);
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
    v.sprite.Offset({ -c.w / 2.f, -c.h / 2.f, 0.f });
    v.sprite.CenterPoint({ c.w / 2.f, c.h / 2.f, 0.f });
    v.sprite.Brush(c.hasSnapshot ? wuc::CompositionBrush(c.snapshot) : wuc::CompositionBrush(m_placeholderBrush));
    v.clip.Size({ c.w, c.h });
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
    SetMinAnimate(false);
    ShowWindowAsync(hwnd, SW_MINIMIZE);
    SetTimer(m_sidebar, kTimerMinAnimate, 600, nullptr);
}

CardVis Stage::MakeVis(Card const& c, bool withBadge)
{
    CardVis v;
    v.holder = m_compositor.CreateContainerVisual();
    auto persp = float4x4::identity();
    persp.m34 = -1.f / S(kDepth);
    v.holder.TransformMatrix(persp);

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
            m_sideContent.Children().InsertAtTop(c.side.holder);
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

// ---- transitions ----------------------------------------------------------

float2 Stage::SideToAnim(float2 p) const
{
    return { p.x + (m_bar.left - m_monitor.left), p.y + (m_bar.top - m_monitor.top) };
}

Pose PoseForRect(RECT const& r, RECT const& monitor, float spriteW)
{
    return { { (r.left + r.right) / 2.f - monitor.left, (r.top + r.bottom) / 2.f - monitor.top },
             (r.right - r.left) / spriteW, 0.f };
}

Pose Stage::FramePose(Card const& c) const
{
    return PoseForRect(c.frame, m_monitor, c.w);
}

Pose Stage::TargetPose(Card const& c) const
{
    RECT r = IsIconic(c.hwnd) ? wt::RestoreRect(c.hwnd) : wt::FrameRect(c.hwnd);
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
    auto next = m_cards[index];
    Pose from = SlotPose(*next, index);
    from.center = SideToAnim(from.center);
    if (m_hover == static_cast<int>(index))
        from.scale *= kHoverGrow;
    m_hover = -1;

    m_cards.erase(m_cards.begin() + index);
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
    KillTimer(m_sidebar, kTimerMinAnimate);
    SetMinAnimate(false);

    m_flyOut = {};
    m_flyIn = {};
    m_outCard = nullptr;
    m_inCard = nextCard;
    m_pending = 0;

    if (prev && prev != next && IsWindow(prev) && IsWindowVisible(prev) && !IsIconic(prev))
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
        m_snap.CaptureAsync(prev, frame, [this, card = m_outCard, frame](auto const& surface) {
            OnOutCaptured(card, frame, surface);
        });
    }
    if (m_inCard)
    {
        ++m_pending;
        m_flyIn = MakeVis(*m_inCard, false);
        m_animRoot.Children().InsertAtTop(m_flyIn.holder);
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
    ShowAnimLayer();
}

void Stage::ShowAnimLayer()
{
    m_sideRoot.Children().Remove(m_sideContent);
    m_sideContent.Offset({ static_cast<float>(m_bar.left - m_monitor.left), static_cast<float>(m_bar.top - m_monitor.top), 0.f });
    m_animRoot.Children().InsertAtBottom(m_sideContent);
    SetWindowPos(m_anim, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Stage::HideAnimLayer()
{
    m_animRoot.Children().RemoveAll();
    m_sideContent.Offset({ 0.f, 0.f, 0.f });
    m_sideRoot.Children().InsertAtTop(m_sideContent);
    ShowWindow(m_anim, SW_HIDE);
}

void Stage::OnOutCaptured(std::shared_ptr<Card> card, RECT const& frame, wuc::CompositionDrawingSurface const& surface)
{
    if (card != m_outCard)
        return;
    SetSnapshot(*card, frame, surface);

    // Below the incoming copy, so the new window visibly lands on top.
    m_flyOut = MakeVis(*card, false);
    m_animRoot.Children().InsertAtBottom(m_flyOut.holder);
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
            m_animRoot.Children().Remove(m_flyOut.holder);
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
    SetTimer(m_sidebar, kTimerFade, 90, nullptr);
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
    bool shown = IsWindowVisible(m_anim);
    m_flyOut = {};
    m_flyIn = {};
    m_outCard = nullptr;
    m_inCard = nullptr;
    m_pending = 0;
    if (shown)
        HideAnimLayer();
    SetMinAnimate(true);
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
        if (!m_busy)
            SetHover(HitTest({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }));
        return 0;
    case WM_LBUTTONUP:
        if (!m_busy)
        {
            int index = HitTest({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (index >= 0)
                SwitchTo(static_cast<size_t>(index));
        }
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
    case WM_TIMER:
        KillTimer(m_sidebar, wp);
        if (wp == kTimerPopulate)
            Populate();
        else if (wp == kTimerMinAnimate)
            SetMinAnimate(true);
        else if (wp == kTimerMinimizeOut && m_outCard)
            ShowWindowAsync(m_outCard->hwnd, SW_MINIMIZE);
        else if (wp == kTimerFade)
            FadeOutFlyIn();
        return 0;
    case WM_DISPLAYCHANGE:
        Dock();
        Relayout(false);
        return 0;
    }
    return DefWindowProcW(m_sidebar, msg, wp, lp);
}
