#pragma once
#include "pch.h"

// Owns the D3D/D2D devices and turns windows into composition surfaces.
class Snapshot
{
public:
    void Init(wuc::Compositor const& compositor);

    using Done = std::function<void(wuc::CompositionDrawingSurface)>;

    // One-shot Windows.Graphics.Capture of `hwnd`, cropped to `frame` and scaled down to <= kMaxWidth.
    // Starting a capture session takes ~100ms, so the blocking form is only for startup.
    wuc::CompositionDrawingSurface Capture(HWND hwnd, RECT const& frame);
    // Same, but returns immediately; `done` runs later on this thread (nullptr on failure or timeout).
    void CaptureAsync(HWND hwnd, RECT const& frame, Done done);
    // The window's app icon rendered at `px` x `px`.
    wuc::CompositionDrawingSurface Icon(HWND hwnd, int px);

private:
    static constexpr float kMaxWidth = 800.f;

    struct Job;
    std::shared_ptr<Job> Start(HWND hwnd, RECT const& frame);
    wuc::CompositionDrawingSurface Render(Job const& job,
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame const& captured);

    winrt::com_ptr<ID3D11Device> m_d3d;
    winrt::com_ptr<ID2D1Device> m_d2d;
    winrt::com_ptr<IWICImagingFactory> m_wic;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_device{ nullptr };
    wuc::CompositionGraphicsDevice m_graphics{ nullptr };
};
