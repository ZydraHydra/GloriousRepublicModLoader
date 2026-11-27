#include "launcher/webview_host.hpp"
#include "launcher/resource.h"
#include "common/ini_util.hpp"

#include <dwmapi.h>
#include <objbase.h>
#include <WebView2.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>

namespace grml::webview_host {

namespace {

ICoreWebView2Controller* g_controller = nullptr;
ICoreWebView2*           g_webview    = nullptr;
HWND                     g_hwnd       = nullptr;
std::wstring             g_initialUrl;

template <typename Interface>
class ComCallbackBase : public Interface {
public:
    virtual ~ComCallbackBase() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(Interface)) {
            *ppv = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG c = --m_ref;
        if (c == 0) delete this;
        return c;
    }
private:
    std::atomic<ULONG> m_ref{1};
};

class ControllerCompletedHandler final
    : public ComCallbackBase<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override
    {
        if (FAILED(result) || !controller) return result;
        g_controller = controller;
        g_controller->AddRef();
        g_controller->get_CoreWebView2(&g_webview);

        if (g_webview) {
            ICoreWebView2Settings* settings = nullptr;
            if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
                settings->put_AreDefaultContextMenusEnabled(FALSE);
                settings->put_IsStatusBarEnabled(FALSE);
                settings->Release();
            }
            g_webview->Navigate(g_initialUrl.c_str());
        }

        RECT bounds{};
        GetClientRect(g_hwnd, &bounds);
        g_controller->put_Bounds(bounds);
        g_controller->put_IsVisible(TRUE);
        return S_OK;
    }
};

class EnvironmentCompletedHandler final
    : public ComCallbackBase<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override
    {
        if (FAILED(result) || !env) {
            MessageBoxW(g_hwnd,
                        L"Microsoft Edge WebView2 Runtime is required to run Glorious Republic Mod Loader.",
                        L"GRML — WebView2 Error",
                        MB_OK | MB_ICONERROR);
            PostQuitMessage(1);
            return result;
        }
        auto* ctrlHandler = new ControllerCompletedHandler();
        env->CreateCoreWebView2Controller(g_hwnd, ctrlHandler);
        ctrlHandler->Release();
        return S_OK;
    }
};

void applyDarkTitlebar(HWND hwnd)
{
    const BOOL darkMode = TRUE;
    const COLORREF captionColor = 0x001b1715; // #15171b (0x00BBGGRR)
    const COLORREF textColor    = 0x00dce7ec; // #ece7dc (0x00BBGGRR)
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &captionColor, sizeof(captionColor));
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &textColor, sizeof(textColor));
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (g_controller) {
            RECT bounds{};
            GetClientRect(hwnd, &bounds);
            g_controller->put_Bounds(bounds);
        }
        return 0;
    case WM_DESTROY:
        if (g_webview) {
            g_webview->Release();
            g_webview = nullptr;
        }
        if (g_controller) {
            g_controller->Close();
            g_controller->Release();
            g_controller = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

int runWindow(HINSTANCE hInstance, uint16_t serverPort)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wchar_t urlBuf[64]{};
    std::swprintf(urlBuf, _countof(urlBuf), L"http://127.0.0.1:%u/", static_cast<unsigned>(serverPort));
    g_initialUrl = urlBuf;

    constexpr wchar_t kClassName[] = L"GRML_LauncherWindowClass";
    HICON hIconBig = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR));
    HICON hIconSmall = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = hIconBig;
    wc.hIconSm       = hIconSmall;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(19, 21, 24));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    const int width  = 1240;
    const int height = 780;
    const int sw     = GetSystemMetrics(SM_CXSCREEN);
    const int sh     = GetSystemMetrics(SM_CYSCREEN);
    const int x      = (sw > width) ? (sw - width) / 2 : CW_USEDEFAULT;
    const int y      = (sh > height) ? (sh - height) / 2 : CW_USEDEFAULT;

    g_hwnd = CreateWindowExW(
        0,
        kClassName,
        L"Glorious Republic Mod Loader (GRML)",
        WS_OVERLAPPEDWINDOW,
        x, y, width, height,
        nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) {
        CoUninitialize();
        return 1;
    }

    if (hIconBig) SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIconBig));
    if (hIconSmall) SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIconSmall));
    applyDarkTitlebar(g_hwnd);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    const std::filesystem::path wv2UserData = grml::getAppDataDir() / L"WebView2";
    auto* envHandler = new EnvironmentCompletedHandler();
    CreateCoreWebView2EnvironmentWithOptions(nullptr, wv2UserData.c_str(), nullptr, envHandler);
    envHandler->Release();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}

} // namespace grml::webview_host
