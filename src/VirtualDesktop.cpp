#include "VirtualDesktop.h"
#include <winstring.h>

namespace
{
    // Windows 11 24H2/25H2 (builds 26100/26200). A different build answers E_NOINTERFACE for these
    // ids, so a mismatched method layout is never called.
    constexpr CLSID kImmersiveShell = { 0xC2F03A33, 0x21F5, 0x47FA, { 0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39 } };
    constexpr GUID kManagerService = { 0xC5E0CDCA, 0x7B6E, 0x41B2, { 0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B } };
    constexpr IID kManagerIid = { 0x53F5CA0B, 0x158F, 0x4124, { 0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27 } };
    constexpr IID kViewCollectionIid = { 0x1841C6D7, 0x4F9D, 0x42C0, { 0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5 } };
    constexpr IID kDesktopIid = { 0x3F07F4BE, 0xB107, 0x441A, { 0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C } };

    struct IVirtualDesktop : IUnknown
    {
        virtual HRESULT __stdcall IsViewVisible(IUnknown* view, BOOL* visible) = 0;
        virtual HRESULT __stdcall GetId(GUID* id) = 0;
        virtual HRESULT __stdcall GetName(HSTRING* name) = 0;
    };

    struct IVirtualDesktopManagerInternal : IUnknown
    {
        virtual HRESULT __stdcall GetCount(UINT* count) = 0;
        virtual HRESULT __stdcall MoveViewToDesktop(IUnknown* view, IVirtualDesktop* desktop) = 0;
        virtual HRESULT __stdcall CanViewMoveDesktops(IUnknown* view, BOOL* can) = 0;
        virtual HRESULT __stdcall GetCurrentDesktop(IVirtualDesktop** desktop) = 0;
        virtual HRESULT __stdcall GetDesktops(IObjectArray** desktops) = 0;
    };

    struct IApplicationViewCollection : IUnknown
    {
        virtual HRESULT __stdcall GetViews(IObjectArray** views) = 0;
        virtual HRESULT __stdcall GetViewsByZOrder(IObjectArray** views) = 0;
        virtual HRESULT __stdcall GetViewsByAppUserModelId(PCWSTR id, IObjectArray** views) = 0;
        virtual HRESULT __stdcall GetViewForHwnd(HWND hwnd, IUnknown** view) = 0;
    };

    struct ExplorerLink
    {
        winrt::com_ptr<IVirtualDesktopManagerInternal> manager;
        winrt::com_ptr<IApplicationViewCollection> views;
    };

    // Asked for when needed (a menu opening, a move), not kept: Explorer may restart in between.
    ExplorerLink Connect()
    {
        ExplorerLink s;
        winrt::com_ptr<IServiceProvider> provider;
        if (FAILED(CoCreateInstance(kImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(provider.put()))))
            return s;
        if (FAILED(provider->QueryService(kManagerService, kManagerIid, s.manager.put_void())) ||
            FAILED(provider->QueryService(kViewCollectionIid, kViewCollectionIid, s.views.put_void())))
            return {};
        return s;
    }

    std::vector<winrt::com_ptr<IVirtualDesktop>> List(ExplorerLink const& s)
    {
        std::vector<winrt::com_ptr<IVirtualDesktop>> out;
        winrt::com_ptr<IObjectArray> array;
        UINT n = 0;
        if (!s.manager || FAILED(s.manager->GetDesktops(array.put())) || FAILED(array->GetCount(&n)))
            return out;
        for (UINT i = 0; i < n; ++i)
        {
            winrt::com_ptr<IVirtualDesktop> d;
            if (SUCCEEDED(array->GetAt(i, kDesktopIid, d.put_void())))
                out.push_back(d);
        }
        return out;
    }
}

namespace vd
{
    bool Current(GUID* id)
    {
        static ExplorerLink kept;
        static ULONGLONG retryAt = 0;
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            if (!kept.manager)
            {
                if (GetTickCount64() < retryAt)
                    return false;
                kept = Connect();
                if (!kept.manager)
                {
                    retryAt = GetTickCount64() + 60000;     // unsupported build: don't keep trying
                    return false;
                }
            }
            winrt::com_ptr<IVirtualDesktop> d;
            if (SUCCEEDED(kept.manager->GetCurrentDesktop(d.put())) && d && SUCCEEDED(d->GetId(id)))
                return true;
            kept = {};                              // Explorer restarted: connect again
        }
        return false;
    }

    bool Available()
    {
        return Connect().manager != nullptr;
    }

    std::vector<GUID> Desktops()
    {
        std::vector<GUID> ids;
        for (auto& d : List(Connect()))
        {
            GUID id{};
            if (SUCCEEDED(d->GetId(&id)))
                ids.push_back(id);
        }
        return ids;
    }

    std::vector<std::wstring> Names()
    {
        std::vector<std::wstring> names;
        for (auto& d : List(Connect()))
        {
            HSTRING raw = nullptr;
            std::wstring name;
            if (SUCCEEDED(d->GetName(&raw)) && raw)
            {
                UINT32 len = 0;
                auto text = WindowsGetStringRawBuffer(raw, &len);
                name.assign(text, len);
                WindowsDeleteString(raw);
            }
            names.push_back(name);
        }
        return names;
    }

    bool MoveWindow(HWND hwnd, GUID const& desktop)
    {
        auto s = Connect();
        if (!s.manager)
            return false;
        winrt::com_ptr<IUnknown> view;
        if (FAILED(s.views->GetViewForHwnd(hwnd, view.put())))
            return false;
        for (auto& d : List(s))
        {
            GUID id{};
            if (SUCCEEDED(d->GetId(&id)) && id == desktop)
                return SUCCEEDED(s.manager->MoveViewToDesktop(view.get(), d.get()));
        }
        return false;
    }
}
