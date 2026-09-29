#pragma once
#include "pch.h"
#include "Snapshot.h"

// Visual tree for one card: holder (perspective, position) -> sprite (image, tilt, scale) + badge.
struct CardVis
{
    wuc::ContainerVisual holder{ nullptr };
    wuc::SpriteVisual sprite{ nullptr };
    wuc::CompositionRoundedRectangleGeometry clip{ nullptr };
    wuc::SpriteVisual badge{ nullptr };
    wuc::SpriteVisual pin{ nullptr };
};

struct Card;

// One window leaving the stage for its sidebar slot.
struct OutFlight
{
    std::shared_ptr<Card> card;
    CardVis vis;
    wuc::CompositionScopedBatch batch{ nullptr };
    bool waitsForPrefetch = false;
};

struct Card
{
    HWND hwnd{};
    RECT frame{};           // last known on-screen frame (physical px)
    RECT border{};          // invisible resize borders: window rect minus visible frame, per side
    bool hasSnapshot = false;   // a real picture of the window (otherwise a placeholder, or nothing if released)
    bool hasPicture = false;    // snapshot or placeholder currently loaded
    bool pinned = false;    // stays in the sidebar, at the top, even while its window is on stage
    float w = 0, h = 0;     // sprite size = frame size
    wuc::CompositionSurfaceBrush snapshot{ nullptr };
    wuc::CompositionSurfaceBrush icon{ nullptr };
    CardVis side;           // visuals in the sidebar
};

// Where a card sits: center in target coords, uniform scale, Y rotation.
struct Pose
{
    float2 center;
    float scale;
    float angle;
    bool thumb = false;     // cropped to the uniform sidebar card shape
};

// Visible part of the sprite, in sprite-local pixels.
struct Crop
{
    float2 offset;
    float2 size;
};

class Stage
{
public:
    bool Init(HINSTANCE inst);
    void Shutdown();

private:
    static LRESULT CALLBACK SidebarProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ViewProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnViewMessage(UINT msg, WPARAM wp, LPARAM lp);
    POINT ViewToSide(LPARAM lp) const;
    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD);

    // Changes made outside the sidebar: Alt+Tab, taskbar, new windows, minimize, close.
    void OnForeground(HWND hwnd);
    void OnMinimizeStart(HWND hwnd);
    void OnGone(HWND hwnd);
    void RemoveCard(HWND hwnd, bool evenIfPinned = false);
    void SetPinned(Card& c, bool pinned);
    void ShowMenu(HMENU menu, UINT* command);           // plain Win32 menu (tray icon)

    // Card context menu, drawn with composition so it matches the sidebar.
    void OpenCardMenu(size_t index);
    void CloseCardMenu();
    int MenuItemAt(POINT viewPt) const;
    void SetMenuHover(int item);
    void RunMenuItem(int item);
    // Virtual desktops: the sidebar only shows windows of the desktop being looked at.
    bool OnCurrentDesktop(HWND hwnd) const;
    static GUID CurrentDesktopId();
    void SyncDesktop();
    void Adopt(HWND hwnd);
    LRESULT OnSidebarMessage(UINT msg, WPARAM wp, LPARAM lp);

    void Dock();
    void AddTrayIcon();
    void ShowTrayMenu();

    float S(float v) const { return v * m_scale; }
    float2 SlotCenter(size_t i) const;          // sidebar coords
    float ThumbScale(Card const& c) const;
    float4x4 Perspective(float2 eye) const;
    Crop CropFor(Card const& c, bool thumb) const;
    float2 Project(Pose const& p, float2 local, float z) const;   // card-local point -> where the camera shows it
    float2 BadgeOffset(Card const& c, Pose const& p) const;
    float2 PinOffset(Card const& c, Pose const& p) const;
    Pose SlotPose(Card const& c, size_t i) const;

    CardVis MakeVis(Card const& c, bool withBadge);
    void ApplyPose(CardVis const& v, Card const& c, Pose const& p);
    void AnimatePose(CardVis const& v, Card const& c, Pose const& from, Pose const& to, int ms);
    void Relayout(bool animate);
    int HitTest(POINT pt) const;
    void SetHover(int index);

    void Populate();
    std::shared_ptr<Card> MakeCard(HWND hwnd);
    void SetSnapshot(Card& c, RECT const& frame, wuc::CompositionDrawingSurface const& surface);
    void ApplySize(CardVis const& v, Card const& c);
    void SetMinAnimate(bool on);

    // Transitions: `next` comes on stage, the current window flies into the sidebar.
    void SwitchTo(size_t index);
    void BeginTransition(HWND next, std::shared_ptr<Card> nextCard, Pose const& nextFrom);
    void OnOutCaptured(std::shared_ptr<OutFlight> flight, RECT const& frame, wuc::CompositionDrawingSurface const& surface);
    bool OnStage(HWND hwnd) const;
    void LeaveStage(HWND hwnd);

    // Dragging a card onto the stage adds its window next to the ones already there.
    void BeginDrag(POINT viewPt);
    void MoveDrag(POINT viewPt);
    void EndDrag(POINT viewPt, bool cancel);
    void JoinStage(std::shared_ptr<Card> card, POINT viewPt);
    void OnFlyInDone();
    void FadeOutFlyIn();
    void StepDone();
    void FinishTransition();
    std::shared_ptr<Card> TakeCard(HWND hwnd);
    float2 SideToAnim(float2 p) const;
    Pose FramePose(Card const& c) const;
    Pose TargetPose(Card const& c) const;
    static void ForceForeground(HWND hwnd);
    void GrowView();                                // whole monitor, for a transition
    void ShrinkView();                              // back to just the bar
    void Prefetch();
    void SnapActiveSoon();                          // keep a recent picture of the stage window for minimize
    void SlideSidebar(bool out);                    // fullscreen apps: tuck the sidebar away to the left
    void SetHotkeys(bool on);                       // Alt+1..4 jump to sidebar cards                                // snapshot the stage window before a click needs it

    HMONITOR m_mon{};
    HWND m_active{};                // the focused window on stage
    std::vector<HWND> m_stage;      // every window on stage, most recently focused last
    int m_savedMinAnimate = 0;
    Snapshot m_snap;
    // m_sidebar is a hidden message window (AppBar, tray icon, hotkeys, timers, shell hook).
    // m_view draws everything and takes the clicks. It covers just the bar at rest and grows to the
    // whole monitor during a transition. It is deliberately not a layered window: DWM drops
    // composition frames of layered windows, which showed up as heavy flicker.
    HWND m_sidebar{};
    HWND m_view{};
    RECT m_monitor{};
    RECT m_bar{};
    float m_scale = 1.f;

    winrt::Windows::System::DispatcherQueueController m_queue{ nullptr };
    wuc::Compositor m_compositor{ nullptr };
    wuc::Desktop::DesktopWindowTarget m_target{ nullptr };
    wuc::ContainerVisual m_root{ nullptr };
    wuc::ContainerVisual m_sideContent{ nullptr };  // sidebar cards, positioned over the bar
    wuc::ContainerVisual m_animStage{ nullptr };    // flying cards; same camera as the sidebar
    wuc::CompositionColorBrush m_placeholderBrush{ nullptr };
    wuc::CompositionEasingFunction m_ease{ nullptr };

    std::vector<std::shared_ptr<Card>> m_cards;     // all known windows, most recent first
    std::vector<std::shared_ptr<Card>> m_visible;   // the ones in the sidebar right now (current desktop, max 4)
    winrt::com_ptr<IVirtualDesktopManager> m_desktops;
    GUID m_desktopId{};
    bool m_busy = false;
    bool m_ready = false;                           // initial population finished
    ULONGLONG m_quietUntil = 0;                     // ignore focus churn caused by our own minimize/restore
    std::vector<HWINEVENTHOOK> m_hooks;
    UINT m_shellMsg = 0;
    bool m_tucked = false;
    wuc::CompositionScopedBatch m_slideBatch{ nullptr };                            // shell hook: fires only for app windows, unlike object WinEvents
    CardVis m_flyIn;
    std::shared_ptr<Card> m_inCard;
    std::vector<std::shared_ptr<OutFlight>> m_outs;
    std::vector<HWND> m_toMinimize;                 // stage windows whose flying copy is now on screen
    int m_pending = 0;                              // fly-out and fly-in steps still running
    wuc::CompositionScopedBatch m_inBatch{ nullptr };
    // Snapshot of the stage window taken when the pointer enters the sidebar, so a click can send it
    // off immediately instead of waiting ~100ms for a capture session.
    HWND m_prefetchHwnd{};
    RECT m_prefetchFrame{};
    ULONGLONG m_prefetchAt = 0;
    bool m_prefetching = false;
    wuc::CompositionDrawingSurface m_prefetch{ nullptr };
    // Minimizing (button, Win+D, four-finger swipe) gives no chance to capture, so the stage window is
    // photographed once shortly after it takes the stage.
    HWND m_activeSnapHwnd{};
    RECT m_activeSnapFrame{};
    wuc::CompositionDrawingSurface m_activeSnap{ nullptr };
    int m_hover = -1;
    bool m_tracking = false;

    struct MenuItem
    {
        UINT command;
        wchar_t const* glyph;
        wchar_t const* label;
    };
    struct CardMenu
    {
        bool open = false;
        std::shared_ptr<Card> card;
        std::vector<MenuItem> items;
        wuc::ContainerVisual root{ nullptr };
        wuc::SpriteVisual highlight{ nullptr };
        float2 origin{};                            // view coordinates of the panel's top-left
        int hover = -1;
    } m_menu;

    // Pointer state on the sidebar: a press becomes a drag once it moves far enough.
    int m_pressIndex = -1;
    POINT m_pressPt{};
    bool m_dragging = false;
    std::shared_ptr<Card> m_dragCard;
    CardVis m_dragVis;
    Pose m_dragPose{};
};
