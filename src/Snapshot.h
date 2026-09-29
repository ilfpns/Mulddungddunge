#pragma once
#include "pch.h"

// Owns the D3D/D2D devices and turns windows into composition surfaces.
class Snapshot
{
public:
    void Init(wuc::Compositor const& compositor);

    using Done = std::function<void(wuc::CompositionDrawingSurface)>;

    // One-shot Windows.Graphics.Capture of `hwnd`, cropped to `frame` and scaled down to <= m_maxWidth.
    // Returns immediately (a capture session takes ~100ms to start); `done` runs later on this thread,
    // with nullptr on failure or timeout.
    void CaptureAsync(HWND hwnd, RECT const& frame, Done done);
    // A minimized window draws nothing, but DWM still has its last picture (what taskbar previews show).
    // Shows that picture in a hidden helper window through a DWM thumbnail and photographs the helper.
    void CaptureMinimized(HWND hwnd, Done done);
    // The window's app icon rendered at `px` x `px`.
    wuc::CompositionDrawingSurface Icon(HWND hwnd, int px);
    // Stand-in for a window we could not photograph (it was minimized): app icon and title on a dark
    // card, with the same aspect ratio as the window.
    // `displayWidth` is the width to draw at in pixels (the card's on-screen width, or a multiple of it).
    wuc::CompositionDrawingSurface Placeholder(HWND hwnd, float w, float h, float displayWidth);
    IDWriteFactory* Text();                         // created on first use
    // A transparent surface of the given size, drawn once by `draw`.
    wuc::CompositionDrawingSurface Paint(float w, float h, std::function<void(ID2D1DeviceContext*)> const& draw);
    // Clears and draws an existing surface again (panels that change, like the settings).
    void Repaint(wuc::CompositionDrawingSurface const& surface, std::function<void(ID2D1DeviceContext*)> const& draw);
    // Draws the window's app icon (cached per app) inside `dst`.
    void DrawAppIcon(ID2D1DeviceContext* dc, HWND hwnd, D2D1_RECT_F const& dst);
    // Detail of window pictures: 0 low, 1 normal, 2 high. Applies to pictures taken from now on.
    void SetQuality(int quality);
    // GPU memory this process holds (its textures and surfaces), in bytes.
    UINT64 GpuMemory() const;

private:
    // Width pictures are scaled down to. The sidebar shows ~260px; the fly-in copy fades into the real window.
    float m_maxWidth = 440.f;

    struct Job;
    std::shared_ptr<Job> Start(HWND hwnd, RECT const& frame);
    wuc::CompositionDrawingSurface Render(Job const& job,
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame const& captured);

    winrt::com_ptr<ID3D11Device> m_d3d;
    winrt::com_ptr<ID2D1Device> m_d2d;
    winrt::com_ptr<IWICImagingFactory> m_wic;
    winrt::com_ptr<IDWriteFactory> m_dwrite;
    // App icons, decoded once per executable (128px): extracting them through the shell is the slowest
    // part of making a card.
    winrt::com_ptr<IWICFormatConverter> CachedIcon(HWND hwnd);
    std::vector<std::pair<std::wstring, winrt::com_ptr<IWICFormatConverter>>> m_icons;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_device{ nullptr };
    wuc::CompositionGraphicsDevice m_graphics{ nullptr };
};
