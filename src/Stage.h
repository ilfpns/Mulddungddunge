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
};

struct Card
{
    HWND hwnd{};
    RECT frame{};           // last known on-screen frame (physical px)
    bool hasSnapshot = false;
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
};

class Stage
{
public:
    bool Init(HINSTANCE inst);
    void Shutdown();

private:
    static LRESULT CALLBACK SidebarProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK AnimProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnSidebarMessage(UINT msg, WPARAM wp, LPARAM lp);

    void Dock();
    void AddTrayIcon();
    void ShowTrayMenu();

    float S(float v) const { return v * m_scale; }
    float2 SlotCenter(size_t i) const;          // sidebar coords
    float ThumbScale(Card const& c) const;
    Pose SlotPose(Card const& c, size_t i) const;

    CardVis MakeVis(Card const& c, bool withBadge);
    void ApplyPose(CardVis const& v, Card const& c, Pose const& p);
    void AnimatePose(CardVis const& v, Card const& c, Pose const& from, Pose const& to, int ms);
    void Relayout(bool animate);
    int HitTest(POINT pt) const;
    void SetHover(int index);

    void Populate();
    std::shared_ptr<Card> MakeCard(HWND hwnd);
    void Refresh(Card& c);                       // blocking re-capture while the window is visible
    void SetSnapshot(Card& c, RECT const& frame, wuc::CompositionDrawingSurface const& surface);
    void ApplySize(CardVis const& v, Card const& c);
    void SetMinAnimate(bool on);
    void MinimizeQuiet(HWND hwnd);

    // Transitions: `next` comes on stage, the current window flies into the sidebar.
    void SwitchTo(size_t index);
    void BeginTransition(HWND next, std::shared_ptr<Card> nextCard, Pose const& nextFrom);
    void OnOutCaptured(std::shared_ptr<Card> card, RECT const& frame, wuc::CompositionDrawingSurface const& surface);
    void OnFlyInDone();
    void FadeOutFlyIn();
    void StepDone();
    void FinishTransition();
    std::shared_ptr<Card> TakeCard(HWND hwnd);
    float2 SideToAnim(float2 p) const;
    Pose FramePose(Card const& c) const;
    Pose TargetPose(Card const& c) const;
    static void ForceForeground(HWND hwnd);
    void ShowAnimLayer();
    void HideAnimLayer();

    HINSTANCE m_inst{};
    HMONITOR m_mon{};
    HWND m_active{};                // the window on stage (not in the sidebar)
    int m_savedMinAnimate = 0;
    Snapshot m_snap;
    HWND m_sidebar{};
    HWND m_anim{};
    RECT m_monitor{};
    RECT m_bar{};
    float m_scale = 1.f;

    winrt::Windows::System::DispatcherQueueController m_queue{ nullptr };
    wuc::Compositor m_compositor{ nullptr };
    wuc::Desktop::DesktopWindowTarget m_sideTarget{ nullptr };
    wuc::Desktop::DesktopWindowTarget m_animTarget{ nullptr };
    wuc::ContainerVisual m_sideRoot{ nullptr };
    // All sidebar cards. Lives under m_sideRoot, but moves into the animation layer during a
    // transition: a layered window on top hides the composition content of windows below it.
    wuc::ContainerVisual m_sideContent{ nullptr };
    wuc::ContainerVisual m_animRoot{ nullptr };
    wuc::CompositionColorBrush m_placeholderBrush{ nullptr };
    wuc::CompositionEasingFunction m_ease{ nullptr };

    std::vector<std::shared_ptr<Card>> m_cards;     // sidebar order, top first
    bool m_busy = false;
    CardVis m_flyOut, m_flyIn;
    std::shared_ptr<Card> m_outCard, m_inCard;
    int m_pending = 0;                              // fly-out and fly-in steps still running
    wuc::CompositionScopedBatch m_inBatch{ nullptr };
    wuc::CompositionScopedBatch m_outBatch{ nullptr };
    int m_hover = -1;
    bool m_tracking = false;
};
