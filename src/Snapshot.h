#pragma once
#include "pch.h"

// Owns the D3D/D2D devices and turns windows into composition surfaces.
class Snapshot
{
public:
    void Init(wuc::Compositor const& compositor);

    using Done = std::function<void(wuc::CompositionDrawingSurface)>;

    // One-shot Windows.Graphics.Capture of `hwnd`, cropped to `frame` and scaled down to <= kMaxWidth.
    // Returns immediately (a capture session takes ~100ms to start); `done` runs later on this thread,
    // with nullptr on failure or timeout.
    void CaptureAsync(HWND hwnd, RECT const& frame, Done done);
    // The window's app icon rendered at `px` x `px`.
    wuc::CompositionDrawingSurface Icon(HWND hwnd, int px);
    // Stand-in for a window we could not photograph (it was minimized): app icon and title on a dark
    // card, with the same aspect ratio as the window.
    // `displayWidth` is the card's on-screen width in pixels, so it is drawn 1:1 and stays crisp.
    wuc::CompositionDrawingSurface Placeholder(HWND hwnd, float w, float h, float displayWidth);
    IDWriteFactory* Text();                         // created on first use
    // A transparent surface of the given size, drawn once by `draw`.
    wuc::CompositionDrawingSurface Paint(float w, float h, std::function<void(ID2D1DeviceContext*)> const& draw);

private:
    static constexpr float kMaxWidth = 440.f;   // sidebar shows ~260px; the fly-in copy fades into the real window

    struct Job;
    std::shared_ptr<Job> Start(HWND hwnd, RECT const& frame);
    wuc::CompositionDrawingSurface Render(Job const& job,
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame const& captured);

    winrt::com_ptr<ID3D11Device> m_d3d;
    winrt::com_ptr<ID2D1Device> m_d2d;
    winrt::com_ptr<IWICImagingFactory> m_wic;
    winrt::com_ptr<IDWriteFactory> m_dwrite;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_device{ nullptr };
    wuc::CompositionGraphicsDevice m_graphics{ nullptr };
};
