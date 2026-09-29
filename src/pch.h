#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <psapi.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <d2d1_3.h>
#include <dwrite.h>
#include <wincodec.h>
#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <share.h>
#include <string>
#include <thread>
#include <vector>

namespace wuc = winrt::Windows::UI::Composition;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;
using winrt::Windows::Foundation::Numerics::float4x4;
