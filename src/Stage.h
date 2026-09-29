#pragma once
#include "pch.h"
#include "Snapshot.h"
#include "Config.h"

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
    std::wstring app;       // executable path; pins are remembered per app
    GUID desktop{};         // virtual desktop it was last seen on (GUID_NULL: unknown, treated as everywhere)
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

// Hands unused memory back when the private working set grows past ~8MB (Stage.cpp).
void TrimMemory();

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
    void OnWinEvent(DWORD event, HWND hwnd);

    // Changes made outside the sidebar: Alt+Tab, taskbar, new windows, minimize, close.
    void OnForeground(HWND hwnd);
    void OnMinimizeStart(HWND hwnd);
    void OnGone(HWND hwnd);
    void RemoveCard(HWND hwnd, bool evenIfPinned = false);
    void Park(std::shared_ptr<Card> card);
    void SetPinned(Card& c, bool pinned);
    wuc::CompositionSurfaceBrush PinBrush();
    void LoadPins();
    void SavePins() const;
    void ShowMenu(HMENU menu, UINT* command);           // plain Win32 menu (tray icon)

    // Card context menu, drawn with composition so it matches the sidebar.
    void OpenCardMenu(int index, int anchorY);      // index < 0: right-click on empty sidebar space
    // Settings: a modal panel over a dimmed screen (Settings.cpp).
    void OpenSettings();
    void CloseSettings();
    bool InSettingsPanel(POINT viewPt) const;
    void PaintSettings();
    void SettingsMove(POINT viewPt);
    void SettingsClick(POINT viewPt);
    void SettingsWheel(int delta);
    void LoadSettingsLists();
    void ApplySetting(int what);
    void SaveSettings();
    void Rebuild();                                 // start over on another monitor
    void UnpinEntry(std::wstring entry);
    void CloseCardMenu();
    int MenuItemAt(POINT viewPt) const;
    void SetMenuHover(int item);
    void RunMenuItem(int item);
    // Virtual desktops: the sidebar only shows windows of the desktop being looked at.
    GUID DesktopOf(HWND hwnd) const;                // asks Explorer (a cross-process call)
    bool Here(Card const& c) const;                 // uses the cached desktop: no call
    static GUID CurrentDesktopId();
    void SyncDesktop();
    void Adopt(HWND hwnd);
    void AdoptPinnedElsewhere();
    static std::wstring PinKey(Card const& c);
    static bool PinMatches(std::wstring const& entry, Card const& c);
    LRESULT OnSidebarMessage(UINT msg, WPARAM wp, LPARAM lp);

    void Dock();
    void AddTrayIcon();
    void ShowTrayMenu();
    static bool StartsWithWindows();
    static void SetStartsWithWindows(bool on);

    float S(float v) const { return v * m_scale; }
    float BarW() const;
    float ThumbW() const;
    float ThumbH() const;
    float TiltAngle() const;
    float PlaceholderScale() const;
    bool Right() const { return m_cfg.right; }
    HMONITOR ChosenMonitor() const;
    bool FitsBeside(HWND hwnd) const;
    void FitBeside();
    bool SideBySide(HWND hwnd) const;
    void PlaceView(LONG left, LONG right, LONG height);     // monitor coordinates
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
    Pose HoverPose(Card const& c, size_t i, bool hovered) const;

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
    static void BringBack(HWND hwnd);
    static void Minimize(HWND hwnd);
    void OnHidden(HWND hwnd);
    void GrowView();                                // whole monitor, for a transition
    void ShrinkView();                              // back to just the bar
    void Prefetch();
    void SnapActiveSoon();                          // keep a recent picture of the stage window for minimize
    // Where the sidebar is. Shown normally; Tucked (slid out, a thin strip at the screen edge brings it
    // back) while a stage window covers it; Hidden (window gone, hotkeys released) for fullscreen apps.
    enum class DockState { Shown, Tucked, Hidden };
    void SetDock(DockState state);
    void UpdateDock();                              // re-evaluate after anything that moves stage windows
    void StageChanged();                            // m_stage changed: follow its windows, then UpdateDock
    bool CoversBar(HWND hwnd) const;
    void SetHotkeys(bool on);                       // Alt+1..4 jump to sidebar cards                                // snapshot the stage window before a click needs it

    Config m_cfg;
    LONG m_viewX = 0;               // view's left edge, relative to the monitor
    HMONITOR m_mon{};
    HWND m_active{};                // the focused window on stage
    std::vector<HWND> m_stage;      // every window on stage, most recently focused last
    int m_savedMinAnimate = 0;
    Snapshot m_snap;
    // m_sidebar is a hidden message window (tray icon, hotkeys, timers, shell hook).
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
    wuc::CompositionSurfaceBrush m_pinBrush{ nullptr };
    wuc::CompositionEasingFunction m_ease{ nullptr };

    std::vector<std::shared_ptr<Card>> m_cards;     // all known windows, most recent first
    std::vector<std::shared_ptr<Card>> m_visible;   // the ones in the sidebar right now (current desktop, max 4)
    // Pinned apps, one entry per pinned window, saved in the registry. Survives restarts and apps that
    // destroy and recreate their window (close-to-tray apps).
    std::vector<std::wstring> m_pinnedApps;
    // Cards of windows that went on stage. Kept (without sidebar visuals) so a window minimized before
    // it could be photographed again still comes back with its last picture instead of a placeholder.
    std::vector<std::shared_ptr<Card>> m_offstage;
    winrt::com_ptr<IVirtualDesktopManager> m_desktops;
    GUID m_desktopId{};
    bool m_busy = false;
    bool m_ready = false;                           // initial population finished
    ULONGLONG m_quietUntil = 0;                     // ignore focus churn caused by our own minimize/restore
    std::vector<HWINEVENTHOOK> m_hooks;
    UINT m_shellMsg = 0;
    DockState m_dock = DockState::Shown;
    bool m_revealed = false;                        // pulled out from the edge by the pointer
    bool m_hotkeysOn = false;
    std::vector<std::pair<DWORD, HWINEVENTHOOK>> m_moveHooks;   // location-change hooks, per stage window thread
    wuc::CompositionScopedBatch m_slideBatch{ nullptr };                            // shell hook: fires only for app windows, unlike object WinEvents
    CardVis m_flyIn;
    std::shared_ptr<Card> m_inCard;
    std::vector<std::shared_ptr<OutFlight>> m_outs;
    std::vector<HWND> m_hidden;                     // stage windows just hidden, see OnHidden
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
    ULONGLONG m_activeSnapAt = 0;
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
    struct Hit
    {
        D2D1_RECT_F rect;                           // panel coordinates
        int id;
        int arg;
    };
    struct SettingsApp
    {
        std::wstring id, label;
        HWND hwnd{};                                // one of its windows, for the icon (null: none open)
    };
    struct SettingsModal
    {
        bool open = false;
        wuc::ContainerVisual root{ nullptr };
        wuc::CompositionDrawingSurface surface{ nullptr };
        float2 origin{}, size{};                    // panel, monitor coordinates
        int tab = 0;
        int hover = -1, hoverArg = 0;               // the control under the pointer (Hit id, arg)
        float scroll = 0.f;                         // list pages (apps, pins)
        float scrollMax = 0.f;
        std::vector<Hit> hits;
        std::vector<SettingsApp> apps;
        std::vector<std::wstring> monitors;         // device names, in the order shown
        UINT64 ram = 0, gpu = 0;
        winrt::com_ptr<IDWriteTextFormat> title, body, caption, icon, value;
    } m_settings;

    // Pointer state on the sidebar: a press becomes a drag once it moves far enough.
    int m_pressIndex = -1;
    POINT m_pressPt{};
    bool m_dragging = false;
    std::shared_ptr<Card> m_dragCard;
    CardVis m_dragVis;
    Pose m_dragPose{};
};
