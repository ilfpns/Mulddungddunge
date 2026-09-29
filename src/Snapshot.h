#pragma once
#include "pch.h"

// Owns the D3D/D2D devices and turns windows into composition surfaces.
class Snapshot
{
public:
    void Init(wuc::Compositor const& compositor);

    // One-shot Windows.Graphics.Capture of `hwnd`, cropped to `frame` and scaled down to <= kMaxWidth.
    wuc::CompositionDrawingSurface Capture(HWND hwnd, RECT const& frame);
    // The window's app icon rendered at `px` x `px`.
    wuc::CompositionDrawingSurface Icon(HWND hwnd, int px);

private:
    static constexpr float kMaxWidth = 800.f;

    winrt::com_ptr<ID3D11Device> m_d3d;
    winrt::com_ptr<ID2D1Device> m_d2d;
    winrt::com_ptr<IWICImagingFactory> m_wic;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_device{ nullptr };
    wuc::CompositionGraphicsDevice m_graphics{ nullptr };
};
