#pragma once
#include "pch.h"

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

    std::shared_ptr<Card> NewPlaceholder(float w, float h);

    HINSTANCE m_inst{};
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
    wuc::ContainerVisual m_animRoot{ nullptr };
    wuc::CompositionColorBrush m_placeholderBrush{ nullptr };
    wuc::CompositionEasingFunction m_ease{ nullptr };

    std::vector<std::shared_ptr<Card>> m_cards;     // sidebar order, top first
    int m_hover = -1;
    bool m_tracking = false;
};
