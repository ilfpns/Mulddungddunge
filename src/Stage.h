#pragma once
#include "pch.h"
#include "Snapshot.h"
#include "Audio.h"
#include "Config.h"

// Visual tree for one card: holder (perspective, position) -> sprite (image, tilt, scale) + badge.
struct CardVis
{
    wuc::ContainerVisual holder{ nullptr };
    wuc::SpriteVisual sprite{ nullptr };
    wuc::CompositionRoundedRectangleGeometry clip{ nullptr };
    wuc::SpriteVisual badge{ nullptr };
    wuc::SpriteVisual pin{ nullptr };
    wuc::SpriteVisual alert{ nullptr };     // orange dot: its app asked for attention
    wuc::SpriteVisual glass{ nullptr };     // blurred backdrop under a glass stand-in; follows the sprite
    wuc::SpriteVisual sound{ nullptr };     // speaker: its app is playing sound
    wuc::ContainerVisual grid{ nullptr };   // a folder card: its windows' pictures, 2 x 2
    // Where the holder was last sent (set or animated to). Reading Offset back gives the last value
    // set, not where an animation took it, so a card that had slid would jump back and slide again.
    mutable float2 at{ -1e9f, -1e9f };
};

struct Card;
struct CardFolder;

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
    bool placeholder = false;   // the loaded picture is a stand-in (icon and title), not the window
    uint32_t logo = 0;          // its app's logo color for the card line (Snapshot::LogoColor), once known
    bool logoKnown = false;
    bool pinned = false;    // keeps a fixed place at the top (out of the sidebar while its window is on stage)
    std::wstring pinKey;    // the saved pin entry it was pinned with (its desktop may change later)
    bool alert = false;     // flashed its taskbar button since it was last looked at
    ULONGLONG alertTraceAt = 0;
    int sound = 0;          // 0 silent, 1 playing, 2 playing but muted
    float w = 0, h = 0;     // sprite size = frame size
    wuc::CompositionSurfaceBrush snapshot{ nullptr };
    wuc::CompositionSurfaceBrush icon{ nullptr };
    CardVis side;           // visuals in the sidebar
    int homeIndex = -1;     // its place in the list when it went on stage (cards moved by hand keep it)
    bool adopting = false;  // just found on screen: photographed, then minimized (not "restored by its app")
    ULONGLONG pulledBackAt = 0;     // Reconcile took it back onto the stage then (no flip-flopping)
    ULONGLONG cardedAt = 0;         // it came into the sidebar then (its window may still be minimizing)
    // A folder's own card (no window): stands for the windows grouped in it, in one slot.
    bool isFolder = false;
    std::weak_ptr<CardFolder> folderOf;
    // What its sidebar grid shows now (window, size, icon): drawn again only when that changes, since
    // rebuilding it blanked the folder for a frame on every layout.
    std::vector<int64_t> gridKey;
    wuc::ContainerVisual gridOf{ nullptr };
};

// Windows grouped into one sidebar card by holding a dragged card over another. Their cards stay in
// m_cards like any other; the sidebar shows the folder's card in their place (or, opened, each of
// them). Kept by window, so a card made again for one of them (after it was on stage) still belongs.
struct CardFolder
{
    int id = 0;                     // names its pin entry ("folder#<id>") while it is pinned
    std::vector<HWND> members;      // in the order its grid shows them, at most kFolderMax
    std::shared_ptr<Card> card;     // what the sidebar shows for it
    bool shown = false;             // has a slot in the current layout
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
    // Something (files from Explorer...) is dragged over the sidebar: held over a card, the card opens
    // its window after a moment so the drop can land in the app. Called by the view's drop target.
    void DragHover(POINTL screen);
    void DragEnd();

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
    void SyncSettingsVisuals();                     // switches, choices, tab marker follow the last paint
    wuc::CompositionLinearGradientBrush GoldBrush();
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
    // The whole app, background included. fromHide: the window was closed to the tray (nothing else
    // is closed, and it gets its card back if the app is spared). False if nothing was started.
    bool QuitApp(HWND hwnd, bool fromHide = false);
    void FinishQuits();
    bool QuitsOnClose(HWND hwnd) const;             // its app is one the user chose to quit on X
    void PutAwayHidden(HWND hwnd);                  // a window hidden to the tray: into the sidebar
    bool InputPanelUp();
    // A thin line runs around a card, from the top edge's middle down both sides to the bottom edge's
    // middle, its trail fading behind it: white on hover, orange when its app asks for attention.
    void StartTrace(int index);                     // the hovered card
    // countdownMs > 0: a file is held over the card; the line fills the outline over that long.
    void RunTrace(Card& c, bool hover, int countdownMs = 0);
    void ClearTrace();                              // hover traces
    void StopAlertTrace(wuc::ContainerVisual const& sprite);
    void SweepTraces();                             // finished ones
    // Card badges beyond the pin: attention dot and speaker.
    wuc::CompositionSurfaceBrush SoundBrush(bool muted);
    wuc::CompositionSurfaceBrush AlertBrush();
    float2 AlertOffset(Card const& c, Pose const& p) const;
    float2 SoundOffset(Card const& c, Pose const& p) const;
    void UpdateBadges(Card& c);
    void OnFlash(HWND hwnd);
    void ClearAlert(HWND hwnd);
    void OnAudioChanged();
    int SoundOf(Card const& c) const;               // 0 silent, 1 playing, 2 playing but muted
    int SoundAt(POINT sidePt) const;                // index of the visible card whose speaker is there
    // An earlier version turned the system's three-finger swipe off while it ran; put it back if a
    // run of it ended without doing so.
    static void RestoreTouchpadGesture();
    void MovePin(std::wstring const& from, std::wstring const& to);
    // "크기 맞추기": the window takes one saved place and size (its visible frame lands there, whatever
    // invisible borders the app has).
    void FitToSaved(HWND hwnd);
    void FitAndOpen(HWND hwnd);                     // then brings it on stage
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
    void PlaceView(LONG left, LONG right, LONG height, bool grown = false);   // monitor coordinates
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
    void SetSnapshot(Card& c, RECT const& frame, wuc::CompositionDrawingSurface const& surface, bool placeholder = false);
    void ApplySize(CardVis const& v, Card const& c);
    void SetMinAnimate(bool on);

    // Transitions: `next` comes on stage, the current window flies into the sidebar.
    void SwitchTo(size_t index);
    void BeginTransition(HWND next, std::shared_ptr<Card> nextCard, Pose const& nextFrom);
    void OnOutCaptured(std::shared_ptr<OutFlight> flight, RECT const& frame, wuc::CompositionDrawingSurface const& surface, bool placeholder = false);
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
    std::shared_ptr<Card> TakeCard(HWND hwnd);     // a window leaving the stage: its card (stamped cardedAt)
    std::shared_ptr<Card> TakeCardImpl(HWND hwnd);
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
    // Folders, and the one opened (its windows listed in the sidebar to pick from).
    std::vector<std::shared_ptr<CardFolder>> m_folders;
    std::shared_ptr<CardFolder> m_openFolder;
    wuc::SpriteVisual m_folderPanel{ nullptr };     // behind an opened folder's cards
    bool m_manualOrder = false;                     // cards were moved by hand: they keep their places
    int m_folderIds = 0;
    int m_hotkeyN = 0;                              // Alt+1..N registered (cards and folders' windows)
    std::shared_ptr<Card> m_groupTarget;            // the card a dragged one is held over (kTimerGroup)
    bool m_groupDone = false;
    std::vector<std::pair<HWND, RECT>> m_borders;   // invisible borders of windows seen on stage (for 크기 맞추기)
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
    std::vector<HWND> m_putAway;                    // spared close-to-tray windows waiting for their card
    HWND m_newWindow{};                             // a foreground window not ready yet, see kTimerNewWindow
    bool m_syncing = false;                         // SyncDesktop running (it may pump messages)
    struct Quitting
    {
        HANDLE proc;
        HWND hwnd;                                  // the window it started from
        DWORD windowPid;                            // that window's process, to tell a reused handle
        ULONGLONG deadline;                         // ended then if it still runs with nothing on screen
        bool fromHide;
    };
    std::vector<Quitting> m_quitting;               // apps asked to quit, checked on kTimerQuit
    std::vector<HWND> m_inputPanels;                // emoji panel / touch keyboard windows shown
    int m_rechecks = 0;                             // looks left at a foreground window not ready yet
    unsigned m_gen = 0;                             // transition number: late callbacks of an older one do nothing
    bool m_shownEarly = false;                      // the incoming window was restored mid-flight (kTimerEarlyShow)
    // A click or right-click on a card while a transition was running: carried out once it ends,
    // rather than dropped (kTimerQueued).
    winrt::com_ptr<IDropTarget> m_dropTarget;       // the view, as a place things can be dragged over
    bool m_fileDrag = false;                        // a drag from another app is over the sidebar
    std::shared_ptr<Card> m_dropCard;               // ...held over this card (kTimerDropOpen)
    bool m_dropHold = false;                        // a card was opened: none opens again until the pointer leaves the cards
    std::shared_ptr<Card> m_queuedSwitch;
    std::shared_ptr<Card> m_queuedMenu;
    int m_queuedMenuY = 0;
    ULONGLONG m_queuedAt = 0;
    UINT m_taskbarMsg = 0;                          // "TaskbarCreated": Explorer restarted, re-add the tray icon
    HICON m_trayIcon = nullptr;
    struct TraceRun
    {
        wuc::ShapeVisual shape{ nullptr };
        wuc::ContainerVisual parent{ nullptr };     // the card's sprite: the line tilts and grows with it
        ULONGLONG until = 0;                        // ~0: runs until taken away (an attention loop)
        bool hover = false;                         // otherwise: the attention loop
    };
    std::vector<TraceRun> m_traces;
    // Head to tail, shared by every card line (hover, file held over, alert): recoloring them
    // recolors the lines already running.
    std::vector<wuc::CompositionColorBrush> m_traceBrushes;
    std::vector<std::pair<uint32_t, std::vector<wuc::CompositionColorBrush>>> m_logoBrushes;   // per logo color
    std::vector<wuc::CompositionColorBrush> const& TraceBrushes(Card& c);
    void TintTraces();
    void RedrawTraces();                            // after the line's thickness changed
    wuc::CompositionSurfaceBrush m_soundBrush[2]{ nullptr, nullptr };
    wuc::CompositionSurfaceBrush m_alertBrush{ nullptr };
    wuc::CompositionBackdropBrush m_backdrop{ nullptr };    // what is behind our window, blurred by DWM
    wuc::CompositionDrawingSurface MakePlaceholder(Card const& c);
    RECT FitFrame();
    void RunQueued();
    // Folders and moving cards by hand.
    std::shared_ptr<CardFolder> FolderOf(HWND hwnd) const;
    std::shared_ptr<Card> CardOf(HWND hwnd) const;  // in the sidebar's list, or parked while on stage
    int SlotOf(Card const& c) const;                // its slot, or its folder's; -1 if not in view
    void InsertReturning(std::shared_ptr<Card> card);   // a card back from the stage: where it belongs
    bool CanGroup(Card const& dragged, Card const& target) const;
    bool Group(std::shared_ptr<Card> dragged, std::shared_ptr<Card> target);
    void LeaveFolder(HWND hwnd);
    void Dissolve(std::shared_ptr<CardFolder> f);
    void PruneFolders();
    void OpenFolder(std::shared_ptr<CardFolder> f, bool byKey);
    void CloseFolder();
    void MoveCard(std::shared_ptr<Card> dragged, std::shared_ptr<Card> target);
    std::shared_ptr<Card> MakeFolderCard();
    wuc::CompositionDrawingSurface FolderBackground(Card const& c);
    void FillFolderGrid(CardVis const& v, Card& folderCard);
    void RefreshFolderBadges(CardFolder& f);
    // A folder's windows in the sidebar (not the one on stage), in its order: its grid, its numbers.
    std::vector<std::shared_ptr<Card>> InSidebar(CardFolder const& f) const;
    std::vector<std::shared_ptr<Card>> HotkeyOrder() const;    // Alt+1.. targets: a folder's windows each count
    int HotkeyCount() const;                        // how many Alt+number keys that needs (at least the card count)
    size_t HotkeyOrderOf(Card const& folderCard) const;
    void RegisterHotkeys();
    void SwitchToCard(std::shared_ptr<Card> next, Pose from);
    // Clicking the empty desktop (like macOS): every stage window flies into its card.
    bool DesktopClicked(HWND foreground);
    void PutStageAway();
    // A new action while a transition runs (Alt+Tab to another window, Alt+number, a card click, a
    // desktop swipe) is not held back until it ends: the transition is cut short and the new one starts.
    // keep: the window the user went to (left as it is). restoreIncoming: bring up the window that was
    // coming in (not when another switch follows at once: it would end up on screen next to that one).
    void AbortTransition(HWND keep, bool restoreIncoming = true);
    bool UserSwitchedMidTransition(HWND hwnd) const;
    std::vector<HWND> m_leaving;                    // windows put away by the current/last transition
    // A desktop switch is seen when it happens (windows uncloaking, the switching preview), not at
    // the next focus change: that one may never come until a click (kTimerDesktopCheck).
    int m_desktopChecks = 0;
    void CheckDesktop();
    // What the sidebar believes against what the windows are, after events that can change a window
    // without the focus change we otherwise go by: restored without activation, moved to another
    // desktop, minimized or hidden unseen, opened minimized. Event-driven (kTimerReconcile), no polling.
    void Reconcile();
    void ReconcileSoon();
    bool m_reconcilePending = false, m_reconcileDue = false;   // timer armed / to run when a transition ends
    ULONGLONG m_reconcileFirst = 0;                 // first event of the current burst (bounds the wait)
    ULONGLONG m_begunAt = 0, m_shownAt = 0;         // transition start, its window brought up
    bool m_fastShow = false;                        // the next switch is from a hotkey: window up sooner
    // At rest the view takes clicks only where the cards are (top card to bottom card); above and
    // below, clicks reach the desktop and windows underneath. settle: drop the old span too.
    void UpdateHitRegion(bool settle = false);
    LONG m_hitTop = 0, m_hitBottom = 0;             // the span the region allows now (window coords)
    bool m_hitRegion = false;                       // a region is set (else the whole view takes clicks)
    winrt::com_ptr<IUIAutomation> m_uia;            // tells a desktop icon from the empty desktop
    // Opening and closing animations: cards spread out of a folder's card, or gather into it.
    using Detached = std::vector<std::pair<std::shared_ptr<Card>, CardVis>>;
    Detached DetachOpened(Card const* except);
    void GatherInto(Detached leaving, std::shared_ptr<CardFolder> f);
    void SpreadFrom(float2 center, std::vector<std::shared_ptr<Card>> const& cards);
    void RestylePlaceholders();
    void RecoverDevice();
    Audio m_audio;
    std::vector<Audio::Playing> m_playing;
    std::vector<Audio::Media> m_media;
    int m_pressSound = -1;                          // speaker pressed, released over it = mute toggle
    std::vector<GUID> m_menuDesktops;               // desktops listed in the open card menu
    winrt::com_ptr<ID2D1Factory> m_d2d;             // for the trace's outline paths
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
        UINT command;                               // 0: a separator line
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
        std::vector<wuc::SpriteVisual> icons;       // per item (null for separators): they move on hover
        std::vector<wuc::CompositionColorBrush> tints;  // their color (the glyph is a mask over it)
        std::vector<std::wstring> labels;           // text of items made up on the spot (desktop names)
        std::vector<float3> iconAt;                 // their resting offsets
        float2 origin{};                            // view coordinates of the panel's top-left
        int hover = -1;
    } m_menu;
    float MenuTop(size_t item) const;               // panel y of an item's top
    float MenuItemH(size_t item) const;
    void AnimateMenuIcon(size_t item, bool hovered);
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
    // Switches, the selected option of a choice and the selected tab are composition visuals over the
    // drawn panel, so their movement is animated by DWM instead of redrawing frames ourselves.
    struct ToggleDesc
    {
        int key;                                    // what it sets (list rows: 1000 + row)
        D2D1_RECT_F rect;                           // track, panel coordinates
        bool on, hover, list;
    };
    struct ChoiceDesc
    {
        int what;
        D2D1_RECT_F rect;                           // the selected option
    };
    struct ToggleVis
    {
        int key = 0;
        bool on = false, hover = false, list = false;
        wuc::SpriteVisual track{ nullptr }, gold{ nullptr }, knob{ nullptr };
        wuc::CompositionColorBrush knobBrush{ nullptr };
    };
    struct ChoiceVis
    {
        int what = 0;
        D2D1_RECT_F rect{};
        wuc::SpriteVisual pill{ nullptr };
    };
    struct SettingsModal
    {
        bool open = false;
        wuc::ContainerVisual root{ nullptr };
        wuc::CompositionDrawingSurface surface{ nullptr };
        // Layers: under the drawn text (tracks, pills), over it (knobs); list rows get clipped copies.
        wuc::ContainerVisual under{ nullptr }, over{ nullptr }, listUnder{ nullptr }, listOver{ nullptr };
        wuc::SpriteVisual navPill{ nullptr }, navBar{ nullptr };
        D2D1_RECT_F navRect{}, listRect{};
        int builtTab = -1;                          // the tab the switch/choice visuals belong to
        std::vector<ToggleDesc> toggleDescs;        // what the last paint laid out
        std::vector<ChoiceDesc> choiceDescs;
        std::vector<ToggleVis> toggles;
        std::vector<ChoiceVis> choices;
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
