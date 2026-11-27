#include "runtime/overlay_hub.hpp"
#include "runtime/ring_logger.hpp"
#include "runtime/save_events.hpp"
#include "runtime/vfs_redirect.hpp"
#include "common/ini_util.hpp"
#include "common/vfs_index.hpp"

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif

#include <windows.h>
#include <d3d11.h>
#include <dinput.h>
#include <dxgi.h>
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace grml::overlay {

namespace {

// ── Legacy Local\WRSR_ModOverlay_<PID> Pure-C IPC Compatibility ───────────────
constexpr uint32_t kLegacyHubMagic = 0x57524D4Fu; // 'WRMO'
constexpr uint32_t kLegacyAbiVer   = 1u;

#define LEGACY_CALL __cdecl
using LegacySaveFn   = void(LEGACY_CALL*)(void*);
using LegacyButtonFn = void(LEGACY_CALL*)(void*);
using LegacyChangeFn = void(LEGACY_CALL*)(void*);

struct LegacyTabDesc {
    uint32_t     struct_size;
    const char*  tab_id;
    const char*  title;
    LegacySaveFn on_save;
    void*        user_data;
};
struct LegacyFloatSliderDesc {
    uint32_t       struct_size;
    const char*    label;
    float*         value;
    float          min_value;
    float          max_value;
    const char*    format;
    const char*    tooltip;
    LegacyChangeFn on_change;
    void*          user_data;
};
struct LegacyCheckboxDesc {
    uint32_t       struct_size;
    const char*    label;
    uint8_t*       value;
    const char*    tooltip;
    LegacyChangeFn on_change;
    void*          user_data;
};
struct LegacyButtonDesc {
    uint32_t       struct_size;
    const char*    label;
    uint8_t        same_line;
    const char*    tooltip;
    LegacyButtonFn on_click;
    void*          user_data;
};
struct LegacyLabelDesc {
    uint32_t    struct_size;
    const char* text;
    uint8_t     separator_before;
    uint32_t    rgba_color;
};
struct LegacyCollapsibleLabelDesc {
    uint32_t    struct_size;
    const char* header;
    const char* text;
    uint8_t     default_open;
    uint8_t     separator_before;
    uint32_t    rgba_color;
};

struct SharedOverlayHub {
    uint32_t      magic;
    uint32_t      abiVersion;
    uint32_t      structSize;
    volatile LONG ownerInitialized;
    DWORD         ownerPid;
    HMODULE       ownerModule;

    int32_t(LEGACY_CALL* registerTab)(const LegacyTabDesc*);
    int32_t(LEGACY_CALL* addFloatSlider)(int32_t, const LegacyFloatSliderDesc*);
    int32_t(LEGACY_CALL* addCheckbox)(int32_t, const LegacyCheckboxDesc*);
    int32_t(LEGACY_CALL* addButton)(int32_t, const LegacyButtonDesc*);
    int32_t(LEGACY_CALL* addLabel)(int32_t, const LegacyLabelDesc*);
    int32_t(LEGACY_CALL* isVisible)(void);
    void(LEGACY_CALL* setVisible)(int32_t);
    int32_t(LEGACY_CALL* addCollapsibleLabel)(int32_t, const LegacyCollapsibleLabelDesc*);
    int32_t(LEGACY_CALL* setTabFrameCallback)(int32_t, LegacyButtonFn, void*);
    int32_t(LEGACY_CALL* setTabMainMenuAvailable)(int32_t, uint8_t);
};

// ── Tab & Telemetry Storage ──────────────────────────────────────────────────
enum class LegacyWidgetKind { FloatSlider, Checkbox, Button, Label, CollapsibleLabel };

struct LegacyStoredWidget {
    LegacyWidgetKind kind{LegacyWidgetKind::Label};
    std::string      label;
    std::string      format;
    std::string      tooltip;
    float*           floatPtr{nullptr};
    float            minVal{0.f};
    float            maxVal{1.f};
    uint8_t*         boolPtr{nullptr};
    const char*      persistentText{nullptr};
    uint8_t          sameLine{0};
    uint8_t          separatorBefore{0};
    uint8_t          defaultOpen{0};
    uint32_t         rgbaColor{0};
    LegacyChangeFn   onChange{nullptr};
    LegacyButtonFn   onClick{nullptr};
    void*            userData{nullptr};
};

struct DeclarativeFieldRuntime {
    std::string              section;
    std::string              key;
    std::string              label;
    std::string              tooltip;
    std::string              format;
    UiControlType            type{UiControlType::Bool};
    double                   minVal{0.0};
    double                   maxVal{1.0};
    bool                     hasExplicitRange{true};
    std::string              defaultStr;
    double                   defaultVal{0.0};
    void*                    livePtr{nullptr};

    uint8_t                  boolVal{0};
    int32_t                  intVal{0};
    float                    floatVal{0.f};
    char                     strVal[256]{};
    std::vector<std::string> options;
    int32_t                  selectedIdx{0};
};

enum class TabKind { CustomGrml, DeclarativeIni, LegacyRetained };

struct RegisteredTab {
    int32_t     tabId{0};
    TabKind     kind{TabKind::CustomGrml};
    std::string pluginId;
    std::string title;
    uint8_t     mainMenuAvailable{0};

    // Fault isolation state
    bool        uiFaulted{false};
    DWORD       faultCode{0};

    // Custom GRML tab
    GrmlRenderTabFn renderCb{nullptr};
    void*           userData{nullptr};

    // Declarative .ini tab
    bool                                 autoGeneratedIni{false};
    std::filesystem::path                iniPath;
    std::vector<DeclarativeFieldRuntime> iniFields;
    GrmlIniAppliedFn                     onIniApplied{nullptr};
    void*                                iniUserData{nullptr};
    std::string                          iniStatusMsg;

    // Legacy retained-mode tab (backward compatibility)
    LegacySaveFn                    legacySave{nullptr};
    void*                           legacyUserData{nullptr};
    LegacyButtonFn                  legacyFrame{nullptr};
    void*                           legacyFrameUserData{nullptr};
    std::vector<LegacyStoredWidget> legacyWidgets;
};

struct HookTelemetryEntry {
    std::string pluginId;
    std::string hookName;
    uint64_t    callsThisFrame{0};
    double      msThisFrame{0.0};
    uint64_t    lastFrameCalls{0};
    double      lastFrameMs{0.0};
};

struct RegisteredWindow {
    int32_t         winId{-1};
    std::string     pluginId;
    std::string     windowId;
    std::string     title;
    uint32_t        defaultVk{0};
    bool            visible{false};
    bool            uiFaulted{false};
    DWORD           faultCode{0};
    GrmlRenderTabFn renderCb{nullptr};
    void*           userData{nullptr};
};

struct RegisteredHotkey {
    int32_t      hotkeyId{-1};
    std::string  pluginId;
    std::string  actionId;
    std::string  label;
    uint32_t     vk{0};
    std::string  conflictWith;
    GrmlHotkeyFn onTrigger{nullptr};
    void*        userData{nullptr};
    int32_t      linkedWinId{-1};
};

std::mutex                      g_tabsMutex;
std::vector<RegisteredTab>      g_tabs;
std::vector<LoadedModSummary>   g_loadedMods;
std::mutex                      g_winHotkeyMutex;
std::vector<RegisteredWindow>   g_windows;
std::vector<RegisteredHotkey>   g_hotkeys;
std::atomic<bool>               g_anyExtWindowVisible{false};
std::mutex                      g_telemetryMutex;
std::vector<HookTelemetryEntry> g_telemetry;
std::filesystem::path           g_pluginsDir;

HANDLE            g_ipcMapHandle = nullptr;
SharedOverlayHub* g_ipcHub       = nullptr;

// ── DX11 / WndProc / Input Lock State ────────────────────────────────────────
std::atomic<bool>       g_installed{false};
std::atomic<bool>       g_dxInitialized{false};
std::atomic<bool>       g_visible{false};
std::atomic<bool>       g_wantCaptureMouse{false};
std::atomic<bool>       g_wantCaptureKeyboard{false};
std::atomic<bool>       g_rectValid{false};
std::atomic<int32_t>    g_rectL{0};
std::atomic<int32_t>    g_rectT{0};
std::atomic<int32_t>    g_rectR{0};
std::atomic<int32_t>    g_rectB{0};
std::atomic<uint8_t>    g_externalMouseButtons{0};
std::atomic<uint8_t>    g_imguiMouseButtons{0};
std::atomic<uint16_t>   g_toggleVk{VK_F9};
std::atomic<const volatile uint8_t*> g_inGameFlagPtr{nullptr};
DirectSaveLoadTargets                  g_directLoadTargets{};
std::atomic<bool>                      g_directLoadConfigured{false};
std::atomic<bool>                      g_directLoadScanCompleted{false};
std::filesystem::path                  g_gameDir;
std::filesystem::path                  g_saveRootOverride;

std::mutex              g_d3dMutex;
ID3D11Device*           g_device      = nullptr;
ID3D11DeviceContext*    g_ctx         = nullptr;
ID3D11RenderTargetView* g_rtv         = nullptr;
HWND                    g_hwnd        = nullptr;
WNDPROC                 g_origWndProc = nullptr;

std::string             g_activeNavKey = "__builtin_loaded_mods";
bool                    g_contentCollapsed = false;
bool                    g_maximized        = false;
bool                    g_pendingGeometry  = false;
ImVec2                  g_targetPos{0.f, 0.f};
ImVec2                  g_targetSize{840.f, 540.f};
ImVec2                  g_expandedSize{840.f, 540.f};
ImVec2                  g_restorePos{60.f, 60.f};
ImVec2                  g_restoreSize{840.f, 540.f};
ImFont*                 g_fontBody     = nullptr;
ImFont*                 g_fontBold     = nullptr;
ImFont*                 g_fontMono     = nullptr;

// Guard depth for ImGui::BeginDisabled inside active sandboxed tab
thread_local int g_activeDisabledDepth = 0;

struct C3dVec3 { float x{0.f}, y{0.f}, z{0.f}; };
using PFN_Present          = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using PFN_ResizeBuffers    = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_DiGetDeviceState = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using PFN_C3dMouseBool     = bool(__fastcall*)(void*);
using PFN_C3dKeyBool       = bool(__fastcall*)(void*, int);
using PFN_C3dMouseRel      = C3dVec3*(__fastcall*)(void*, C3dVec3*);

PFN_Present          g_origPresent                = nullptr;
PFN_ResizeBuffers    g_origResizeBuffers          = nullptr;
PFN_DiGetDeviceState g_origDiGetDeviceState       = nullptr;
PFN_C3dMouseBool     g_origGetMouseLeftPress      = nullptr;
PFN_C3dMouseBool     g_origGetMouseLeftPressOff   = nullptr;
PFN_C3dMouseBool     g_origGetMouseLeftDown       = nullptr;
PFN_C3dMouseBool     g_origGetMouseRightPress     = nullptr;
PFN_C3dMouseBool     g_origGetMouseRightPressOff  = nullptr;
PFN_C3dMouseBool     g_origGetMouseRightDown      = nullptr;
PFN_C3dMouseBool     g_origGetMouseMiddlePress    = nullptr;
PFN_C3dMouseBool     g_origGetMouseMiddlePressOff = nullptr;
PFN_C3dMouseBool     g_origGetMouseMiddleDown     = nullptr;
PFN_C3dMouseRel      g_origGetMouseRelative       = nullptr;
PFN_C3dKeyBool       g_origGetKeyDown             = nullptr;
PFN_C3dKeyBool       g_origGetKeyPress            = nullptr;

void* g_presentTarget       = nullptr;
void* g_resizeBuffersTarget = nullptr;
void* g_diTarget            = nullptr;

bool isGameRunning() noexcept
{
    const volatile uint8_t* ptr = g_inGameFlagPtr.load(std::memory_order_acquire);
    const bool inGame = ptr ? ((*ptr) != 0) : true;
    save_events::notifyInGameState(inGame);
    return inGame;
}

ImVec4 unpackRgba(uint32_t rgba) noexcept
{
    return ImVec4(
        static_cast<float>((rgba >> 24) & 0xFF) / 255.f,
        static_cast<float>((rgba >> 16) & 0xFF) / 255.f,
        static_cast<float>((rgba >> 8) & 0xFF) / 255.f,
        static_cast<float>((rgba >> 0) & 0xFF) / 255.f);
}

// ── SEH-Isolated Callback Invokers (no C++ destructors inside __try) ─────────
bool invokeRenderTabSeh(GrmlRenderTabFn fn, const GrmlUiApi* api, void* userData, DWORD* outCode) noexcept
{
    if (!fn) return true;
    __try {
        fn(api, userData);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outCode) *outCode = GetExceptionCode();
        return false;
    }
}

bool invokeLegacyButtonSeh(LegacyButtonFn fn, void* userData, DWORD* outCode) noexcept
{
    if (!fn) return true;
    __try {
        fn(userData);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outCode) *outCode = GetExceptionCode();
        return false;
    }
}

// ── GrmlUiApi Implementation ─────────────────────────────────────────────────
extern const GrmlUiApi g_grmlUiApi;

int32_t GRML_CALL uiRegisterTab(const char* pluginId, const char* tabTitle, GrmlRenderTabFn renderCb, void* userData)
{
    if (!pluginId || !*pluginId || !tabTitle || !*tabTitle || !renderCb) {
        return -1;
    }
    std::lock_guard lock(g_tabsMutex);
    for (auto& t : g_tabs) {
        if (t.pluginId == pluginId && t.title == tabTitle) {
            t.kind      = TabKind::CustomGrml;
            t.renderCb  = renderCb;
            t.userData  = userData;
            t.uiFaulted = false;
            return t.tabId;
        }
    }
    RegisteredTab tab;
    tab.tabId    = static_cast<int32_t>(g_tabs.size());
    tab.kind     = TabKind::CustomGrml;
    tab.pluginId = pluginId;
    tab.title    = tabTitle;
    tab.renderCb = renderCb;
    tab.userData = userData;
    g_tabs.push_back(std::move(tab));
    return g_tabs.back().tabId;
}

void syncDeclarativeLivePtrs(RegisteredTab& tab) noexcept
{
    for (auto& f : tab.iniFields) {
        if (!f.livePtr) continue;
        switch (f.type) {
        case UiControlType::Bool:   *static_cast<uint8_t*>(f.livePtr) = f.boolVal; break;
        case UiControlType::Int:    *static_cast<int32_t*>(f.livePtr) = f.intVal; break;
        case UiControlType::Float:  *static_cast<float*>(f.livePtr)   = f.floatVal; break;
        default: break;
        }
    }
}

void loadDeclarativeIni(RegisteredTab& tab)
{
    IniFile ini;
    ini.loadFromFile(tab.iniPath);
    for (auto& f : tab.iniFields) {
        switch (f.type) {
        case UiControlType::Bool:
            f.boolVal = ini.getBool(f.section, f.key, f.defaultVal != 0.0) ? 1u : 0u;
            break;
        case UiControlType::Int: {
            int v = ini.getInt(f.section, f.key, static_cast<int>(f.defaultVal));
            if (f.hasExplicitRange) {
                v = (std::clamp)(v, static_cast<int>(f.minVal), static_cast<int>(f.maxVal));
            } else {
                if (static_cast<double>(v) < f.minVal) f.minVal = static_cast<double>(v);
                if (static_cast<double>(v) > f.maxVal) f.maxVal = static_cast<double>(v);
            }
            f.intVal = v;
            break;
        }
        case UiControlType::Float: {
            double v = ini.getDouble(f.section, f.key, f.defaultVal);
            if (f.hasExplicitRange) {
                v = (std::clamp)(v, f.minVal, f.maxVal);
            } else {
                if (v < f.minVal) f.minVal = v;
                if (v > f.maxVal) f.maxVal = v;
            }
            f.floatVal = static_cast<float>(v);
            break;
        }
        case UiControlType::String: {
            const std::string val = ini.getString(f.section, f.key, f.defaultStr);
            std::snprintf(f.strVal, sizeof(f.strVal), "%s", val.c_str());
            break;
        }
        case UiControlType::Select: {
            const std::string val = ini.getString(f.section, f.key, f.defaultStr);
            f.selectedIdx = 0;
            for (size_t i = 0; i < f.options.size(); ++i) {
                if (f.options[i] == val) {
                    f.selectedIdx = static_cast<int32_t>(i);
                    break;
                }
            }
            break;
        }
        case UiControlType::Header:
            break;
        }
    }
    syncDeclarativeLivePtrs(tab);
    if (tab.onIniApplied) {
        tab.onIniApplied(tab.iniUserData);
    }
}

bool saveDeclarativeIni(RegisteredTab& tab)
{
    std::vector<IniKeyUpdate> updates;
    updates.reserve(tab.iniFields.size());
    for (const auto& f : tab.iniFields) {
        if (f.type == UiControlType::Header || f.key.empty()) continue;
        IniKeyUpdate u;
        u.section = f.section;
        u.key     = f.key;
        switch (f.type) {
        case UiControlType::Bool:
            u.value = (f.boolVal != 0) ? "1" : "0";
            break;
        case UiControlType::Int: {
            char buf[32]{};
            std::snprintf(buf, sizeof(buf), "%d", f.intVal);
            u.value = buf;
            break;
        }
        case UiControlType::Float: {
            char buf[64]{};
            std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(f.floatVal));
            u.value = buf;
            break;
        }
        case UiControlType::String:
            u.value = f.strVal;
            break;
        case UiControlType::Select:
            if (f.selectedIdx >= 0 && static_cast<size_t>(f.selectedIdx) < f.options.size()) {
                u.value = f.options[static_cast<size_t>(f.selectedIdx)];
            } else {
                continue;
            }
            break;
        case UiControlType::Header:
            continue;
        }
        updates.push_back(std::move(u));
    }
    syncDeclarativeLivePtrs(tab);
    const bool ok = updateIniFilePreservingComments(tab.iniPath, updates);
    if (ok && tab.onIniApplied) {
        tab.onIniApplied(tab.iniUserData);
    }
    return ok;
}

int32_t GRML_CALL uiRegisterDeclarativeIniTab(const GrmlDeclarativeIniTabDesc* desc)
{
    if (!desc || desc->struct_size < sizeof(GrmlDeclarativeIniTabDesc) ||
        !desc->plugin_id || !desc->tab_title || !desc->fields || desc->field_count == 0) {
        return -1;
    }

    RegisteredTab tab;
    tab.kind              = TabKind::DeclarativeIni;
    tab.pluginId          = desc->plugin_id;
    tab.title             = desc->tab_title;
    tab.mainMenuAvailable = desc->main_menu_available;
    tab.onIniApplied      = desc->on_applied;
    tab.iniUserData       = desc->user_data;

    std::string fname = (desc->ini_filename && *desc->ini_filename)
                            ? std::string(desc->ini_filename)
                            : (std::string(desc->plugin_id) + ".ini");
    std::filesystem::path p = utf8ToWide(fname);
    if (p.is_relative() && !g_pluginsDir.empty()) {
        p = g_pluginsDir / p;
    }
    tab.iniPath = p;

    for (uint32_t i = 0; i < desc->field_count; ++i) {
        const auto& src = desc->fields[i];
        if (src.struct_size < sizeof(GrmlIniFieldDesc) || !src.section || !src.key || !src.label) {
            continue;
        }
        DeclarativeFieldRuntime rf;
        rf.section    = src.section;
        rf.key        = src.key;
        rf.label      = src.label;
        rf.tooltip    = src.tooltip ? src.tooltip : "";
        rf.format     = src.format ? src.format : "";
        rf.type       = (src.type == GRML_INI_INT)
                            ? UiControlType::Int
                            : ((src.type == GRML_INI_FLOAT) ? UiControlType::Float : UiControlType::Bool);
        rf.minVal     = src.min_value;
        rf.maxVal     = src.max_value;
        rf.defaultVal = src.default_value;
        rf.livePtr    = src.live_ptr;
        tab.iniFields.push_back(std::move(rf));
    }
    loadDeclarativeIni(tab);
    tab.iniStatusMsg = "Loaded from " + wideToUtf8(tab.iniPath.filename().wstring());

    std::lock_guard lock(g_tabsMutex);
    tab.tabId = static_cast<int32_t>(g_tabs.size());
    g_tabs.push_back(std::move(tab));
    return g_tabs.back().tabId;
}

void GRML_CALL uiSetTabMainMenuAvailable(int32_t tabId, uint8_t available)
{
    std::lock_guard lock(g_tabsMutex);
    if (tabId >= 0 && static_cast<size_t>(tabId) < g_tabs.size()) {
        g_tabs[static_cast<size_t>(tabId)].mainMenuAvailable = available ? 1u : 0u;
    }
}

int32_t GRML_CALL uiCheckbox(const char* label, uint8_t* value)
{
    if (!label || !value) return 0;
    bool b = (*value != 0);
    if (ImGui::Checkbox(label, &b)) {
        *value = b ? 1u : 0u;
        return 1;
    }
    return 0;
}

int32_t GRML_CALL uiSliderFloat(const char* label, float* value, float minVal, float maxVal, const char* fmt)
{
    if (!label || !value) return 0;
    return ImGui::SliderFloat(label, value, minVal, maxVal, (fmt && *fmt) ? fmt : "%.2f",
                              ImGuiSliderFlags_AlwaysClamp)
               ? 1
               : 0;
}

int32_t GRML_CALL uiSliderInt(const char* label, int32_t* value, int32_t minVal, int32_t maxVal, const char* fmt)
{
    if (!label || !value) return 0;
    return ImGui::SliderInt(label, value, minVal, maxVal, (fmt && *fmt) ? fmt : "%d",
                            ImGuiSliderFlags_AlwaysClamp)
               ? 1
               : 0;
}

int32_t GRML_CALL uiButton(const char* label)
{
    if (!label) return 0;
    return ImGui::Button(label) ? 1 : 0;
}

void GRML_CALL uiText(const char* fmt, ...)
{
    if (!fmt) return;
    char buf[1024]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ImGui::TextUnformatted(buf);
}

void GRML_CALL uiTextColored(uint32_t rgba, const char* fmt, ...)
{
    if (!fmt) return;
    char buf[1024]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ImGui::TextColored(unpackRgba(rgba), "%s", buf);
}

void GRML_CALL uiTextWrapped(const char* fmt, ...)
{
    if (!fmt) return;
    char buf[2048]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ImGui::TextWrapped("%s", buf);
}

void GRML_CALL uiSeparator(void) { ImGui::Separator(); }
void GRML_CALL uiSameLine(void)  { ImGui::SameLine(); }

void GRML_CALL uiBeginDisabled(uint8_t disabled)
{
    ImGui::BeginDisabled(disabled != 0);
    ++g_activeDisabledDepth;
}

void GRML_CALL uiEndDisabled(void)
{
    if (g_activeDisabledDepth > 0) {
        ImGui::EndDisabled();
        --g_activeDisabledDepth;
    }
}

int32_t GRML_CALL uiCollapsingHeader(const char* label, uint8_t defaultOpen)
{
    if (!label) return 0;
    const ImGuiTreeNodeFlags flags = defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
    return ImGui::CollapsingHeader(label, flags) ? 1 : 0;
}

void GRML_CALL uiSetItemTooltip(const char* fmt, ...)
{
    if (!fmt || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) return;
    char buf[1024]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ImGui::SetTooltip("%s", buf);
}

void GRML_CALL uiReportHookTiming(const char* pluginId, const char* hookName, uint64_t callsDelta, double elapsedMs)
{
    if (!pluginId || !hookName) return;
    std::lock_guard lock(g_telemetryMutex);
    for (auto& e : g_telemetry) {
        if (e.pluginId == pluginId && e.hookName == hookName) {
            e.callsThisFrame += callsDelta;
            e.msThisFrame += elapsedMs;
            return;
        }
    }
    HookTelemetryEntry e;
    e.pluginId       = pluginId;
    e.hookName       = hookName;
    e.callsThisFrame = callsDelta;
    e.msThisFrame    = elapsedMs;
    g_telemetry.push_back(std::move(e));
}

int32_t GRML_CALL uiInputFloat(const char* label, float* value, float step, float stepFast, const char* fmt)
{
    if (!label || !value) return 0;
    return ImGui::InputFloat(label, value, step, stepFast, fmt ? fmt : "%.3f") ? 1 : 0;
}

int32_t GRML_CALL uiInputInt(const char* label, int32_t* value, int32_t step, int32_t stepFast)
{
    if (!label || !value) return 0;
    return ImGui::InputInt(label, value, step, stepFast) ? 1 : 0;
}

int32_t GRML_CALL uiInputText(const char* label, char* buf, uint32_t bufSize)
{
    if (!label || !buf || bufSize == 0) return 0;
    return ImGui::InputText(label, buf, bufSize) ? 1 : 0;
}

int32_t GRML_CALL uiCombo(const char* label, int32_t* currentItem, const char* const* items, int32_t itemCount)
{
    if (!label || !currentItem || !items || itemCount <= 0) return 0;
    return ImGui::Combo(label, currentItem, items, itemCount) ? 1 : 0;
}

int32_t GRML_CALL uiBeginTable(const char* id, int32_t columns, uint32_t flags)
{
    if (!id || columns <= 0) return 0;
    ImGuiTableFlags imFlags = ImGuiTableFlags_None;
    if (flags & GRML_TABLE_BORDERS)      imFlags |= ImGuiTableFlags_Borders;
    if (flags & GRML_TABLE_ROW_BG)       imFlags |= ImGuiTableFlags_RowBg;
    if (flags & GRML_TABLE_RESIZABLE)    imFlags |= ImGuiTableFlags_Resizable;
    if (flags & GRML_TABLE_SCROLL_Y)     imFlags |= ImGuiTableFlags_ScrollY;
    if (flags & GRML_TABLE_SIZING_FIXED) imFlags |= ImGuiTableFlags_SizingFixedFit;
    return ImGui::BeginTable(id, columns, imFlags) ? 1 : 0;
}

void GRML_CALL uiEndTable(void)                      { ImGui::EndTable(); }
void GRML_CALL uiTableNextRow(void)                  { ImGui::TableNextRow(); }
int32_t GRML_CALL uiTableNextColumn(void)            { return ImGui::TableNextColumn() ? 1 : 0; }
void GRML_CALL uiTableSetupColumn(const char* label, float width)
{
    if (!label) return;
    if (width > 0.0f) ImGui::TableSetupColumn(label, ImGuiTableColumnFlags_WidthFixed, width);
    else ImGui::TableSetupColumn(label);
}
void GRML_CALL uiTableHeadersRow(void)               { ImGui::TableHeadersRow(); }
void GRML_CALL uiPushItemWidth(float width)          { ImGui::PushItemWidth(width); }
void GRML_CALL uiPopItemWidth(void)                  { ImGui::PopItemWidth(); }
void GRML_CALL uiIndent(float width)                 { ImGui::Indent(width); }
void GRML_CALL uiUnindent(float width)               { ImGui::Unindent(width); }

void refreshAnyExtWindowVisibleLocked()
{
    bool any = false;
    for (const auto& w : g_windows) {
        if (w.visible) { any = true; break; }
    }
    g_anyExtWindowVisible.store(any, std::memory_order_relaxed);
}

int32_t registerHotkeyInternal(
    const char*  pluginId,
    const char*  actionId,
    const char*  label,
    uint32_t     defaultVk,
    GrmlHotkeyFn onTrigger,
    void*        userData,
    int32_t      linkedWinId)
{
    if (!pluginId || !*pluginId || !actionId || !*actionId || defaultVk == 0) return -1;

    std::string conflict;
    if (defaultVk == g_toggleVk.load(std::memory_order_relaxed)) {
        conflict = "GRML Mod Settings overlay";
    } else {
        for (const auto& hk : g_hotkeys) {
            if (hk.vk == defaultVk) {
                conflict = hk.pluginId + " (" + hk.label + ")";
                break;
            }
        }
    }
    if (!conflict.empty()) {
        logger::logMessage(GRML_LOG_WARN, "Hotkeys",
                           "[HOTKEY CONFLICT] Plugin '%s' (%s) VK=0x%X conflicts with %s",
                           pluginId, label ? label : actionId, defaultVk, conflict.c_str());
    }

    RegisteredHotkey hk;
    hk.hotkeyId     = static_cast<int32_t>(g_hotkeys.size());
    hk.pluginId     = pluginId;
    hk.actionId     = actionId;
    hk.label        = label ? label : actionId;
    hk.vk           = defaultVk;
    hk.conflictWith = std::move(conflict);
    hk.onTrigger    = onTrigger;
    hk.userData     = userData;
    hk.linkedWinId  = linkedWinId;
    g_hotkeys.push_back(std::move(hk));
    return g_hotkeys.back().hotkeyId;
}

int32_t GRML_CALL uiRegisterWindow(
    const char*     pluginId,
    const char*     windowId,
    const char*     title,
    uint32_t        defaultVk,
    GrmlRenderTabFn renderCb,
    void*           userData)
{
    if (!pluginId || !*pluginId || !windowId || !*windowId || !title || !renderCb) return -1;
    std::lock_guard lock(g_winHotkeyMutex);
    for (const auto& w : g_windows) {
        if (w.pluginId == pluginId && w.windowId == windowId) return w.winId;
    }
    RegisteredWindow w;
    w.winId     = static_cast<int32_t>(g_windows.size());
    w.pluginId  = pluginId;
    w.windowId  = windowId;
    w.title     = title;
    w.defaultVk = defaultVk;
    w.renderCb  = renderCb;
    w.userData  = userData;
    g_windows.push_back(std::move(w));
    const int32_t id = g_windows.back().winId;
    if (defaultVk != 0) {
        registerHotkeyInternal(pluginId, windowId, title, defaultVk, nullptr, nullptr, id);
    }
    return id;
}

void GRML_CALL uiSetWindowVisible(int32_t winId, uint8_t visible)
{
    std::lock_guard lock(g_winHotkeyMutex);
    if (winId < 0 || static_cast<size_t>(winId) >= g_windows.size()) return;
    g_windows[static_cast<size_t>(winId)].visible = (visible != 0);
    refreshAnyExtWindowVisibleLocked();
}

int32_t GRML_CALL uiIsWindowVisible(int32_t winId)
{
    std::lock_guard lock(g_winHotkeyMutex);
    if (winId < 0 || static_cast<size_t>(winId) >= g_windows.size()) return 0;
    return g_windows[static_cast<size_t>(winId)].visible ? 1 : 0;
}

int32_t GRML_CALL uiRegisterHotkey(
    const char*  pluginId,
    const char*  actionId,
    const char*  label,
    uint32_t     defaultVk,
    GrmlHotkeyFn onTrigger,
    void*        userData)
{
    if (!onTrigger) return -1;
    std::lock_guard lock(g_winHotkeyMutex);
    return registerHotkeyInternal(pluginId, actionId, label, defaultVk, onTrigger, userData, -1);
}

const GrmlUiApi g_grmlUiApi = {
    GRML_API_VERSION,
    sizeof(GrmlUiApi),
    &uiRegisterTab,
    &uiRegisterDeclarativeIniTab,
    &uiSetTabMainMenuAvailable,
    &uiCheckbox,
    &uiSliderFloat,
    &uiSliderInt,
    &uiButton,
    &uiText,
    &uiTextColored,
    &uiTextWrapped,
    &uiSeparator,
    &uiSameLine,
    &uiBeginDisabled,
    &uiEndDisabled,
    &uiCollapsingHeader,
    &uiSetItemTooltip,
    &uiReportHookTiming,
    &uiInputFloat,
    &uiInputInt,
    &uiInputText,
    &uiCombo,
    &uiBeginTable,
    &uiEndTable,
    &uiTableNextRow,
    &uiTableNextColumn,
    &uiTableSetupColumn,
    &uiTableHeadersRow,
    &uiPushItemWidth,
    &uiPopItemWidth,
    &uiIndent,
    &uiUnindent,
    &uiRegisterWindow,
    &uiSetWindowVisible,
    &uiIsWindowVisible,
    &uiRegisterHotkey,
};

// ── Legacy WRSR_ModOverlay_<PID> Host Callbacks ──────────────────────────────
int32_t LEGACY_CALL legacyRegisterTab(const LegacyTabDesc* desc)
{
    if (!desc || desc->struct_size < sizeof(LegacyTabDesc) || !desc->tab_id || !desc->title) return -1;
    std::lock_guard lock(g_tabsMutex);
    for (auto& t : g_tabs) {
        if (t.pluginId == desc->tab_id) return t.tabId;
    }
    RegisteredTab t;
    t.tabId          = static_cast<int32_t>(g_tabs.size());
    t.kind           = TabKind::LegacyRetained;
    t.pluginId       = desc->tab_id;
    t.title          = desc->title;
    t.legacySave     = desc->on_save;
    t.legacyUserData = desc->user_data;
    g_tabs.push_back(std::move(t));
    return g_tabs.back().tabId;
}

int32_t LEGACY_CALL legacyAddFloatSlider(int32_t tabId, const LegacyFloatSliderDesc* desc)
{
    if (!desc || desc->struct_size < sizeof(LegacyFloatSliderDesc) || !desc->label || !desc->value) return 0;
    std::lock_guard lock(g_tabsMutex);
    if (tabId < 0 || static_cast<size_t>(tabId) >= g_tabs.size()) return 0;
    LegacyStoredWidget w;
    w.kind     = LegacyWidgetKind::FloatSlider;
    w.label    = desc->label;
    w.format   = desc->format ? desc->format : "%.2f";
    w.tooltip  = desc->tooltip ? desc->tooltip : "";
    w.floatPtr = desc->value;
    w.minVal   = desc->min_value;
    w.maxVal   = desc->max_value;
    w.onChange = desc->on_change;
    w.userData = desc->user_data;
    g_tabs[static_cast<size_t>(tabId)].legacyWidgets.push_back(std::move(w));
    return 1;
}

int32_t LEGACY_CALL legacyAddCheckbox(int32_t tabId, const LegacyCheckboxDesc* desc)
{
    if (!desc || desc->struct_size < sizeof(LegacyCheckboxDesc) || !desc->label || !desc->value) return 0;
    std::lock_guard lock(g_tabsMutex);
    if (tabId < 0 || static_cast<size_t>(tabId) >= g_tabs.size()) return 0;
    LegacyStoredWidget w;
    w.kind     = LegacyWidgetKind::Checkbox;
    w.label    = desc->label;
    w.tooltip  = desc->tooltip ? desc->tooltip : "";
    w.boolPtr  = desc->value;
    w.onChange = desc->on_change;
    w.userData = desc->user_data;
    g_tabs[static_cast<size_t>(tabId)].legacyWidgets.push_back(std::move(w));
    return 1;
}

int32_t LEGACY_CALL legacyAddButton(int32_t tabId, const LegacyButtonDesc* desc)
{
    if (!desc || desc->struct_size < sizeof(LegacyButtonDesc) || !desc->label) return 0;
    std::lock_guard lock(g_tabsMutex);
    if (tabId < 0 || static_cast<size_t>(tabId) >= g_tabs.size()) return 0;
    LegacyStoredWidget w;
    w.kind     = LegacyWidgetKind::Button;
    w.label    = desc->label;
    w.sameLine = desc->same_line;
    w.tooltip  = desc->tooltip ? desc->tooltip : "";
    w.onClick  = desc->on_click;
    w.userData = desc->user_data;
    g_tabs[static_cast<size_t>(tabId)].legacyWidgets.push_back(std::move(w));
    return 1;
}

int32_t LEGACY_CALL legacyAddLabel(int32_t tabId, const LegacyLabelDesc* desc)
{
    if (!desc || desc->struct_size < sizeof(LegacyLabelDesc) || !desc->text) return 0;
    std::lock_guard lock(g_tabsMutex);
    if (tabId < 0 || static_cast<size_t>(tabId) >= g_tabs.size()) return 0;
    LegacyStoredWidget w;
    w.kind            = LegacyWidgetKind::Label;
    w.persistentText  = desc->text;
    w.separatorBefore = desc->separator_before;
    w.rgbaColor       = desc->rgba_color;
    g_tabs[static_cast<size_t>(tabId)].legacyWidgets.push_back(std::move(w));
    return 1;
}

int32_t LEGACY_CALL legacyAddCollapsibleLabel(int32_t tabId, const LegacyCollapsibleLabelDesc* desc)
{
    if (!desc || desc->struct_size < sizeof(LegacyCollapsibleLabelDesc) || !desc->header || !desc->text) return 0;
    std::lock_guard lock(g_tabsMutex);
    if (tabId < 0 || static_cast<size_t>(tabId) >= g_tabs.size()) return 0;
    LegacyStoredWidget w;
    w.kind            = LegacyWidgetKind::CollapsibleLabel;
    w.label           = desc->header;
    w.persistentText  = desc->text;
    w.defaultOpen     = desc->default_open;
    w.separatorBefore = desc->separator_before;
    w.rgbaColor       = desc->rgba_color;
    g_tabs[static_cast<size_t>(tabId)].legacyWidgets.push_back(std::move(w));
    return 1;
}

int32_t LEGACY_CALL legacySetTabFrameCallback(int32_t tabId, LegacyButtonFn onFrame, void* userData)
{
    std::lock_guard lock(g_tabsMutex);
    if (tabId < 0 || static_cast<size_t>(tabId) >= g_tabs.size()) return 0;
    auto& t = g_tabs[static_cast<size_t>(tabId)];
    t.legacyFrame         = onFrame;
    t.legacyFrameUserData = userData;
    return 1;
}

int32_t LEGACY_CALL legacySetTabMainMenuAvailable(int32_t tabId, uint8_t available)
{
    uiSetTabMainMenuAvailable(tabId, available);
    return 1;
}

int32_t LEGACY_CALL legacyIsVisible(void)
{
    return g_visible.load(std::memory_order_relaxed) ? 1 : 0;
}

void toggleOverlay(bool show) noexcept
{
    g_visible.store(show, std::memory_order_relaxed);
    if (show) {
        ClipCursor(nullptr);
    } else {
        g_wantCaptureMouse.store(false, std::memory_order_relaxed);
        g_wantCaptureKeyboard.store(false, std::memory_order_relaxed);
        g_imguiMouseButtons.store(0, std::memory_order_relaxed);
        g_rectValid.store(false, std::memory_order_relaxed);
    }
}

void LEGACY_CALL legacySetVisible(int32_t visible)
{
    toggleOverlay(visible != 0);
}

void publishLegacyOverlayIpcHub()
{
    const DWORD pid = GetCurrentProcessId();
    wchar_t mutexName[96]{};
    wchar_t mapName[96]{};
    std::swprintf(mutexName, _countof(mutexName), L"Local\\WRSR_ModOverlay_Mutex_%lu", pid);
    std::swprintf(mapName, _countof(mapName), L"Local\\WRSR_ModOverlay_%lu", pid);

    HANDLE hSync = CreateMutexW(nullptr, FALSE, mutexName);
    if (hSync) WaitForSingleObject(hSync, INFINITE);

    g_ipcMapHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, sizeof(SharedOverlayHub), mapName);
    if (g_ipcMapHandle) {
        g_ipcHub = static_cast<SharedOverlayHub*>(
            MapViewOfFile(g_ipcMapHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedOverlayHub)));
    }
    if (g_ipcHub && g_ipcHub->ownerInitialized == 0) {
        HMODULE selfMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&publishLegacyOverlayIpcHub), &selfMod);
        g_ipcHub->magic                   = kLegacyHubMagic;
        g_ipcHub->abiVersion              = kLegacyAbiVer;
        g_ipcHub->structSize              = sizeof(SharedOverlayHub);
        g_ipcHub->ownerPid                = pid;
        g_ipcHub->ownerModule             = selfMod;
        g_ipcHub->registerTab             = &legacyRegisterTab;
        g_ipcHub->addFloatSlider          = &legacyAddFloatSlider;
        g_ipcHub->addCheckbox             = &legacyAddCheckbox;
        g_ipcHub->addButton               = &legacyAddButton;
        g_ipcHub->addLabel                = &legacyAddLabel;
        g_ipcHub->isVisible               = &legacyIsVisible;
        g_ipcHub->setVisible              = &legacySetVisible;
        g_ipcHub->addCollapsibleLabel     = &legacyAddCollapsibleLabel;
        g_ipcHub->setTabFrameCallback     = &legacySetTabFrameCallback;
        g_ipcHub->setTabMainMenuAvailable = &legacySetTabMainMenuAvailable;
        InterlockedExchange(&g_ipcHub->ownerInitialized, 1);
        logger::logMessage(GRML_LOG_INFO, "OverlayHub", "Published legacy Local\\WRSR_ModOverlay IPC hub");
    }

    if (hSync) {
        ReleaseMutex(hSync);
        CloseHandle(hSync);
    }
}

// ── Input Isolation & DirectInput / C3D_INPUT Guards ─────────────────────────
bool isCursorOverOverlayRect() noexcept
{
    if (!g_visible.load(std::memory_order_relaxed) ||
        !g_rectValid.load(std::memory_order_acquire) || !g_hwnd) {
        return false;
    }
    POINT pt{};
    if (!GetCursorPos(&pt) || !ScreenToClient(g_hwnd, &pt)) return false;
    return pt.x >= g_rectL.load(std::memory_order_relaxed) &&
           pt.x <  g_rectR.load(std::memory_order_relaxed) &&
           pt.y >= g_rectT.load(std::memory_order_relaxed) &&
           pt.y <  g_rectB.load(std::memory_order_relaxed);
}

bool isMouseLockedForGame() noexcept
{
    if (!g_visible.load(std::memory_order_relaxed)) return false;
    if (g_externalMouseButtons.load(std::memory_order_relaxed) != 0) return false;
    if (g_imguiMouseButtons.load(std::memory_order_relaxed) != 0) return true;
    return g_wantCaptureMouse.load(std::memory_order_relaxed) || isCursorOverOverlayRect();
}

bool isKeyboardLockedForGame() noexcept
{
    return g_visible.load(std::memory_order_relaxed) &&
           g_wantCaptureKeyboard.load(std::memory_order_relaxed);
}

HRESULT STDMETHODCALLTYPE detourDiGetDeviceState(IDirectInputDevice8W* self, DWORD cbData, LPVOID lpvData)
{
    const HRESULT hr = g_origDiGetDeviceState(self, cbData, lpvData);
    if (FAILED(hr) || !lpvData) return hr;

    if (cbData == sizeof(DIMOUSESTATE) || cbData == sizeof(DIMOUSESTATE2)) {
        const size_t btnCount = (cbData == sizeof(DIMOUSESTATE2)) ? 8 : 4;
        const auto*  btns     = static_cast<const BYTE*>(lpvData) + offsetof(DIMOUSESTATE, rgbButtons);
        uint8_t curMask = 0;
        for (size_t i = 0; i < btnCount; ++i) {
            if (btns[i] & 0x80) curMask |= static_cast<uint8_t>(1u << i);
        }
        if (!g_visible.load(std::memory_order_relaxed)) {
            g_externalMouseButtons.store(curMask, std::memory_order_relaxed);
            g_imguiMouseButtons.store(0, std::memory_order_relaxed);
            return hr;
        }
        uint8_t extMask = g_externalMouseButtons.load(std::memory_order_relaxed) & curMask;
        uint8_t imgMask = g_imguiMouseButtons.load(std::memory_order_relaxed) & curMask;
        const uint8_t newly = curMask & static_cast<uint8_t>(~(extMask | imgMask));
        if (newly != 0) {
            const bool overUi = (extMask == 0) && (imgMask != 0 ||
                                                   g_wantCaptureMouse.load(std::memory_order_relaxed) ||
                                                   isCursorOverOverlayRect());
            if (overUi) imgMask |= newly;
            else        extMask |= newly;
        }
        g_externalMouseButtons.store(extMask, std::memory_order_relaxed);
        g_imguiMouseButtons.store(imgMask, std::memory_order_relaxed);
        if (isMouseLockedForGame()) std::memset(lpvData, 0, cbData);
    } else if (cbData == 256 && isKeyboardLockedForGame()) {
        std::memset(lpvData, 0, cbData);
    }
    return hr;
}

bool __fastcall detourMouseLeftPress(void* s)      { return isMouseLockedForGame() ? false : g_origGetMouseLeftPress(s); }
bool __fastcall detourMouseLeftPressOff(void* s)   { return isMouseLockedForGame() ? false : g_origGetMouseLeftPressOff(s); }
bool __fastcall detourMouseLeftDown(void* s)       { return isMouseLockedForGame() ? false : g_origGetMouseLeftDown(s); }
bool __fastcall detourMouseRightPress(void* s)     { return isMouseLockedForGame() ? false : g_origGetMouseRightPress(s); }
bool __fastcall detourMouseRightPressOff(void* s)  { return isMouseLockedForGame() ? false : g_origGetMouseRightPressOff(s); }
bool __fastcall detourMouseRightDown(void* s)      { return isMouseLockedForGame() ? false : g_origGetMouseRightDown(s); }
bool __fastcall detourMouseMiddlePress(void* s)    { return isMouseLockedForGame() ? false : g_origGetMouseMiddlePress(s); }
bool __fastcall detourMouseMiddlePressOff(void* s) { return isMouseLockedForGame() ? false : g_origGetMouseMiddlePressOff(s); }
bool __fastcall detourMouseMiddleDown(void* s)     { return isMouseLockedForGame() ? false : g_origGetMouseMiddleDown(s); }
C3dVec3* __fastcall detourMouseRelative(void* s, C3dVec3* out)
{
    C3dVec3* r = g_origGetMouseRelative(s, out);
    if (r && isMouseLockedForGame()) { r->x = 0.f; r->y = 0.f; r->z = 0.f; }
    return r;
}
bool __fastcall detourKeyDown(void* s, int k)  { return isKeyboardLockedForGame() ? false : g_origGetKeyDown(s, k); }
bool __fastcall detourKeyPress(void* s, int k) { return isKeyboardLockedForGame() ? false : g_origGetKeyPress(s, k); }

bool patchModuleIat(HMODULE hMod, const char* dllName, const char* funcName, void* detour, void** outOrig) noexcept
{
    if (!hMod || !dllName || !funcName || !detour) return false;
    auto* base = reinterpret_cast<uint8_t*>(hMod);
    auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return false;

    for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dllName) != 0) continue;
        const DWORD oft = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        auto* thunkOrig = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + oft);
        auto* thunkIat  = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
        for (; thunkOrig->u1.AddressOfData; ++thunkOrig, ++thunkIat) {
            if (IMAGE_SNAP_BY_ORDINAL64(thunkOrig->u1.Ordinal)) continue;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + thunkOrig->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), funcName) != 0) continue;
            DWORD oldProt = 0;
            if (!VirtualProtect(&thunkIat->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &oldProt)) return false;
            if (outOrig) *outOrig = reinterpret_cast<void*>(thunkIat->u1.Function);
            thunkIat->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
            VirtualProtect(&thunkIat->u1.Function, sizeof(ULONG_PTR), oldProt, &oldProt);
            return true;
        }
    }
    return false;
}

void installDirectInputGuard()
{
    using PFN_DiCreate = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    HMODULE hDi = GetModuleHandleW(L"dinput8.dll");
    if (!hDi) hDi = LoadLibraryW(L"dinput8.dll");
    if (!hDi) return;
    auto pfnCreate = reinterpret_cast<PFN_DiCreate>(GetProcAddress(hDi, "DirectInput8Create"));
    if (!pfnCreate) return;

    static constexpr GUID kIidDi8 = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    static constexpr GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    IDirectInput8W* di = nullptr;
    IDirectInputDevice8W* dev = nullptr;
    if (FAILED(pfnCreate(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, kIidDi8, reinterpret_cast<void**>(&di), nullptr)) || !di) return;
    if (FAILED(di->CreateDevice(kSysMouse, &dev, nullptr)) || !dev) { di->Release(); return; }
    void** vtbl = *reinterpret_cast<void***>(dev);
    void* target = vtbl[9];
    dev->Release();
    di->Release();
    if (MH_CreateHook(target, reinterpret_cast<void*>(detourDiGetDeviceState), reinterpret_cast<void**>(&g_origDiGetDeviceState)) == MH_OK) {
        MH_EnableHook(target);
        g_diTarget = target;
    }
}

void installC3dInputGuards()
{
    HMODULE hExe = GetModuleHandleW(nullptr);
    if (!hExe) return;
    struct Entry { const char* sym; void* detour; void** orig; } entries[] = {
        {"?GetMouseLeftPress@C3D_INPUT@@QEAA_NXZ", reinterpret_cast<void*>(detourMouseLeftPress), reinterpret_cast<void**>(&g_origGetMouseLeftPress)},
        {"?GetMouseLeftPressAOff@C3D_INPUT@@QEAA_NXZ", reinterpret_cast<void*>(detourMouseLeftPressOff), reinterpret_cast<void**>(&g_origGetMouseLeftPressOff)},
        {"?GetMouseLeftDown@C3D_INPUT@@QEAA_NXZ", reinterpret_cast<void*>(detourMouseLeftDown), reinterpret_cast<void**>(&g_origGetMouseLeftDown)},
        {"?GetMouseRightPress@C3D_INPUT@@QEAA_NXZ", reinterpret_cast<void*>(detourMouseRightPress), reinterpret_cast<void**>(&g_origGetMouseRightPress)},
        {"?GetMouseRightPressAOff@C3D_INPUT@@QEAA_NXZ", reinterpret_cast<void*>(detourMouseRightPressOff), reinterpret_cast<void**>(&g_origGetMouseRightPressOff)},
        {"?GetMouseRightDown@C3D_INPUT@@QEAA_NXZ", reinterpret_cast<void*>(detourMouseRightDown), reinterpret_cast<void**>(&g_origGetMouseRightDown)},
        {"?GetMouseMiddlePress@C3D_INPUT@@QEAA_NXZ", reinterpret_cast<void*>(detourMouseMiddlePress), reinterpret_cast<void**>(&g_origGetMouseMiddlePress)},
        {"?GetMouseMiddlePressAOff@C3D_INPUT@@QEAA_NXZ", reinterpret_cast<void*>(detourMouseMiddlePressOff), reinterpret_cast<void**>(&g_origGetMouseMiddlePressOff)},
        {"?GetMouseMiddleDown@C3D_INPUT@@QEAA_NXZ", reinterpret_cast<void*>(detourMouseMiddleDown), reinterpret_cast<void**>(&g_origGetMouseMiddleDown)},
        {"?GetMouseRelative@C3D_INPUT@@QEAA?AVC3DVECTOR3@@XZ", reinterpret_cast<void*>(detourMouseRelative), reinterpret_cast<void**>(&g_origGetMouseRelative)},
        {"?GetKeyDown@C3D_INPUT@@QEAA_NH@Z", reinterpret_cast<void*>(detourKeyDown), reinterpret_cast<void**>(&g_origGetKeyDown)},
        {"?GetKeyPress@C3D_INPUT@@QEAA_NH@Z", reinterpret_cast<void*>(detourKeyPress), reinterpret_cast<void**>(&g_origGetKeyPress)},
    };
    for (const auto& e : entries) {
        patchModuleIat(hExe, "C3DDLL64.dll", e.sym, e.detour, e.orig);
    }
}

void applyWrsrTheme()
{
    ImGuiStyle& style       = ImGui::GetStyle();
    style.WindowPadding     = ImVec2(14.f, 12.f);
    style.FramePadding      = ImVec2(9.f, 5.f);
    style.ItemSpacing       = ImVec2(8.f, 7.f);
    style.WindowRounding    = 6.f;
    style.ChildRounding     = 6.f;
    style.FrameRounding     = 4.f;
    style.PopupRounding     = 4.f;
    style.TabRounding       = 4.f;
    style.ScrollbarRounding = 4.f;
    style.GrabRounding      = 4.f;
    style.WindowBorderSize  = 1.f;
    style.ChildBorderSize   = 1.f;
    style.FrameBorderSize   = 1.f;
    style.ScrollbarSize     = 9.f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                 = ImVec4(0.925f, 0.906f, 0.863f, 1.00f); // #ece7dc
    c[ImGuiCol_TextDisabled]         = ImVec4(0.604f, 0.627f, 0.667f, 1.00f); // #9aa0aa
    c[ImGuiCol_WindowBg]             = ImVec4(0.075f, 0.082f, 0.094f, 0.98f); // #131518
    c[ImGuiCol_ChildBg]              = ImVec4(0.098f, 0.110f, 0.129f, 1.00f); // #191c21
    c[ImGuiCol_PopupBg]              = ImVec4(0.098f, 0.110f, 0.129f, 0.98f); // #191c21
    c[ImGuiCol_Border]               = ImVec4(0.165f, 0.180f, 0.208f, 1.00f); // #2a2e35
    c[ImGuiCol_TitleBg]              = ImVec4(0.082f, 0.090f, 0.106f, 1.00f); // #15171b
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.106f, 0.118f, 0.141f, 1.00f); // #1b1e24
    c[ImGuiCol_FrameBg]              = ImVec4(0.059f, 0.067f, 0.078f, 1.00f); // #0f1114
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.149f, 0.169f, 0.200f, 1.00f); // #262b33
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.180f, 0.204f, 0.239f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.129f, 0.145f, 0.173f, 1.00f); // #21252c
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.227f, 0.204f, 0.145f, 1.00f); // subtle gold tint
    c[ImGuiCol_ButtonActive]         = ImVec4(0.831f, 0.659f, 0.325f, 0.45f); // #d4a853
    c[ImGuiCol_Header]               = ImVec4(0.129f, 0.145f, 0.173f, 0.90f); // #21252c
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.149f, 0.169f, 0.200f, 1.00f); // #262b33
    c[ImGuiCol_HeaderActive]         = ImVec4(0.831f, 0.659f, 0.325f, 0.35f); // #d4a853
    c[ImGuiCol_Tab]                  = ImVec4(0.082f, 0.090f, 0.106f, 1.00f); // #15171b
    c[ImGuiCol_TabHovered]           = ImVec4(0.149f, 0.169f, 0.200f, 1.00f); // #262b33
    c[ImGuiCol_TabSelected]          = ImVec4(0.129f, 0.145f, 0.173f, 1.00f); // #21252c
    c[ImGuiCol_TabSelectedOverline]  = ImVec4(0.831f, 0.659f, 0.325f, 1.00f); // #d4a853
    c[ImGuiCol_SliderGrab]           = ImVec4(0.831f, 0.659f, 0.325f, 1.00f); // #d4a853
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.910f, 0.769f, 0.463f, 1.00f); // #e8c476
    c[ImGuiCol_CheckMark]            = ImVec4(0.910f, 0.769f, 0.463f, 1.00f); // #e8c476
    c[ImGuiCol_Separator]            = ImVec4(0.165f, 0.180f, 0.208f, 1.00f); // #2a2e35
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.063f, 0.071f, 0.082f, 1.00f); // #101215
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.180f, 0.200f, 0.231f, 1.00f); // #2e333b
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.263f, 0.290f, 0.337f, 1.00f); // #434a56
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.831f, 0.659f, 0.325f, 0.85f); // #d4a853
    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.098f, 0.110f, 0.129f, 1.00f); // #191c21
    c[ImGuiCol_TableBorderStrong]    = ImVec4(0.165f, 0.180f, 0.208f, 1.00f); // #2a2e35
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.129f, 0.145f, 0.173f, 0.75f); // #21252c
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(0.098f, 0.110f, 0.129f, 0.45f);
}

void loadLauncherFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    wchar_t winDir[MAX_PATH]{};
    const UINT len = GetWindowsDirectoryW(winDir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        const std::filesystem::path fontsDir = std::filesystem::path(winDir) / L"Fonts";
        std::error_code ec;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        const auto segoe = fontsDir / L"segoeui.ttf";
        if (std::filesystem::exists(segoe, ec)) {
            g_fontBody = io.Fonts->AddFontFromFileTTF(wideToUtf8(segoe.wstring()).c_str(), 15.5f, &cfg);
        }
        const auto segoeBold = fontsDir / L"segoeuib.ttf";
        if (std::filesystem::exists(segoeBold, ec)) {
            g_fontBold = io.Fonts->AddFontFromFileTTF(wideToUtf8(segoeBold.wstring()).c_str(), 15.5f, &cfg);
        }
        const auto consola = fontsDir / L"consola.ttf";
        if (std::filesystem::exists(consola, ec)) {
            g_fontMono = io.Fonts->AddFontFromFileTTF(wideToUtf8(consola.wstring()).c_str(), 13.5f, &cfg);
        }
    }
    if (!g_fontBody) {
        g_fontBody = io.Fonts->AddFontDefault();
    }
}

void releaseRenderTarget()
{
    std::lock_guard lock(g_d3dMutex);
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

void createRenderTarget(IDXGISwapChain* sc)
{
    std::lock_guard lock(g_d3dMutex);
    ID3D11Texture2D* backBuf = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuf))) || !backBuf) return;
    if (g_device) g_device->CreateRenderTargetView(backBuf, nullptr, &g_rtv);
    backBuf->Release();
}

LRESULT CALLBACK hookedWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const uint16_t vk = g_toggleVk.load(std::memory_order_relaxed);
    if (msg == WM_KEYDOWN && ((lp & (1LL << 30)) == 0)) {
        if (wp == vk) {
            toggleOverlay(!g_visible.load(std::memory_order_relaxed));
            return 0;
        }
        if (!g_wantCaptureKeyboard.load(std::memory_order_relaxed)) {
            bool handledHotkey = false;
            std::vector<std::pair<GrmlHotkeyFn, void*>> toCall;
            {
                std::lock_guard lock(g_winHotkeyMutex);
                for (const auto& hk : g_hotkeys) {
                    if (hk.vk == static_cast<uint32_t>(wp)) {
                        if (hk.linkedWinId >= 0 && static_cast<size_t>(hk.linkedWinId) < g_windows.size()) {
                            auto& w = g_windows[static_cast<size_t>(hk.linkedWinId)];
                            w.visible = !w.visible;
                            refreshAnyExtWindowVisibleLocked();
                            handledHotkey = true;
                        } else if (hk.onTrigger) {
                            toCall.emplace_back(hk.onTrigger, hk.userData);
                            handledHotkey = true;
                        }
                    }
                }
            }
            for (const auto& [fn, ud] : toCall) {
                fn(ud);
            }
            if (handledHotkey) return 0;
        }
    }

    const bool anyVisible = g_visible.load(std::memory_order_relaxed) ||
                            g_anyExtWindowVisible.load(std::memory_order_relaxed);
    if (anyVisible) {
        if (msg == WM_KEYDOWN && wp == VK_ESCAPE && ((lp & (1LL << 30)) == 0)) {
            toggleOverlay(false);
            {
                std::lock_guard lock(g_winHotkeyMutex);
                for (auto& w : g_windows) w.visible = false;
                refreshAnyExtWindowVisibleLocked();
            }
            return 0;
        }
        const bool isMouseMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT;
        const bool isKeyMsg   = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST);
        const bool extDrag    = (g_externalMouseButtons.load(std::memory_order_relaxed) != 0);
        if (!isMouseMsg || !extDrag) {
            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wp, lp);
        }
        const bool lockMouse = isMouseLockedForGame();
        if (msg == WM_SETCURSOR && lockMouse) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return 1;
        }
        if (isMouseMsg && lockMouse) return 0;
        if (isKeyMsg && isKeyboardLockedForGame()) return 0;
    }
    return CallWindowProcW(g_origWndProc, hWnd, msg, wp, lp);
}

// ── Launcher-Matched UI Helpers ──────────────────────────────────────────────
bool beginPanelCard(const char* id, const char* title = nullptr)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.098f, 0.110f, 0.129f, 1.00f)); // #191c21
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f, 12.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.f);
    const bool ok = ImGui::BeginChild(
        id, ImVec2(0.f, 0.f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    if (ok && title && *title) {
        if (g_fontBold) ImGui::PushFont(g_fontBold);
        ImGui::TextColored(ImVec4(0.910f, 0.769f, 0.463f, 1.00f), "%s", title); // #e8c476
        if (g_fontBold) ImGui::PopFont();
        ImGui::Spacing();
    }
    return ok;
}

void endPanelCard()
{
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

bool drawToggleSwitch(const char* id, bool* v)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = 36.f;
    const float h = 19.f;
    const float r = h * 0.5f;

    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool clicked = ImGui::IsItemClicked();
    if (clicked && v) *v = !(*v);

    const bool hovered = ImGui::IsItemHovered();
    const bool on = v ? *v : false;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImU32 trackCol = on
        ? (hovered ? IM_COL32(232, 196, 118, 255) : IM_COL32(212, 168, 83, 255))
        : (hovered ? IM_COL32(46, 52, 62, 255) : IM_COL32(34, 38, 46, 255));
    const ImU32 borderCol = on ? IM_COL32(232, 196, 118, 255) : IM_COL32(58, 64, 74, 255);

    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), trackCol, r);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), borderCol, r, 0, 1.f);

    const float knobX = on ? (p.x + w - r) : (p.x + r);
    const float knobY = p.y + r;
    const ImU32 knobCol = on ? IM_COL32(22, 20, 16, 255) : IM_COL32(236, 231, 220, 255);
    dl->AddCircleFilled(ImVec2(knobX, knobY), r - 3.f, knobCol);
    return clicked;
}

bool drawPrimaryButton(const char* label)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.831f, 0.659f, 0.325f, 1.00f));        // #d4a853
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.910f, 0.769f, 0.463f, 1.00f)); // #e8c476
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.769f, 0.584f, 0.239f, 1.00f));  // #c4953d
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.086f, 0.078f, 0.063f, 1.00f));          // #161410
    if (g_fontBold) ImGui::PushFont(g_fontBold);
    const bool pressed = ImGui::Button(label);
    if (g_fontBold) ImGui::PopFont();
    ImGui::PopStyleColor(4);
    return pressed;
}

bool renderSidebarNavItem(const char* id,
                          const char* label,
                          bool        selected,
                          const char* badgeText   = nullptr,
                          bool        badgeDanger = false)
{
    const float availW = ImGui::GetContentRegionAvail().x;
    const float itemH  = 32.f;
    const ImVec2 p     = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(availW, itemH));
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) {
        dl->AddRectFilled(p, ImVec2(p.x + availW, p.y + itemH), IM_COL32(33, 37, 44, 255), 4.f); // #21252c
        dl->AddRect(p, ImVec2(p.x + availW, p.y + itemH), IM_COL32(42, 46, 53, 255), 4.f);       // #2a2e35
        dl->AddRectFilled(ImVec2(p.x, p.y + 2.f), ImVec2(p.x + 3.f, p.y + itemH - 2.f),
                          IM_COL32(212, 168, 83, 255), 1.f); // #d4a853 left bar
    } else if (hovered) {
        dl->AddRectFilled(p, ImVec2(p.x + availW, p.y + itemH), IM_COL32(25, 28, 33, 255), 4.f); // #191c21
    }

    float badgeReserve = 8.f;
    if (badgeText && *badgeText) {
        const ImVec2 bSz = ImGui::CalcTextSize(badgeText);
        const float pillW = bSz.x + 14.f;
        const float pillH = 18.f;
        const float bx    = p.x + availW - pillW - 7.f;
        const float by    = p.y + (itemH - pillH) * 0.5f;
        const ImU32 pillBg = badgeDanger ? IM_COL32(95, 32, 28, 255) : IM_COL32(40, 44, 52, 255);
        const ImU32 pillFg = badgeDanger ? IM_COL32(242, 97, 87, 255) : IM_COL32(232, 196, 118, 255);
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + pillW, by + pillH), pillBg, 9.f);
        dl->AddText(ImVec2(bx + 7.f, by + (pillH - bSz.y) * 0.5f), pillFg, badgeText);
        badgeReserve = pillW + 14.f;
    }

    const ImU32 textCol = selected
        ? IM_COL32(232, 196, 118, 255) // #e8c476
        : (hovered ? IM_COL32(236, 231, 220, 255) : IM_COL32(154, 160, 170, 255));
    const ImVec2 tSz = ImGui::CalcTextSize(label);
    const float tx   = p.x + 12.f;
    const float ty   = p.y + (itemH - tSz.y) * 0.5f;
    dl->PushClipRect(ImVec2(tx, p.y), ImVec2(p.x + availW - badgeReserve, p.y + itemH), true);
    dl->AddText(ImVec2(tx, ty), textCol, label);
    dl->PopClipRect();

    return clicked;
}

void renderTopbar(const char* hotkeyStr, size_t hiddenInGameCount, bool* pOpen)
{
    const float barH     = 44.f;
    const ImVec2 winPos  = ImGui::GetWindowPos();
    const ImVec2 winSize = ImGui::GetWindowSize();
    const ImGuiIO& io    = ImGui::GetIO();
    ImDrawList* dl       = ImGui::GetWindowDrawList();

    dl->AddRectFilled(winPos, ImVec2(winPos.x + winSize.x, winPos.y + barH), IM_COL32(24, 27, 32, 255), 6.f,
                      ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(winPos.x, winPos.y + barH - 1.f),
                ImVec2(winPos.x + winSize.x, winPos.y + barH - 1.f),
                IM_COL32(42, 46, 53, 255), 1.f);

    // Window control buttons on far right: [Collapse/Expand Content] [Maximize/Restore] [Close]
    const float btnSz  = 24.f;
    const float btnGap = 4.f;
    const float btnY   = winPos.y + (barH - btnSz) * 0.5f;
    const float closeX = winPos.x + winSize.x - 10.f - btnSz;
    const float maxX   = closeX - btnGap - btnSz;
    const float panelX = maxX - btnGap - btnSz;

    // Draggable area covering the topbar (excluding the right-side control buttons)
    ImGui::SetCursorPos(ImVec2(0.f, 0.f));
    const float dragW = (std::max)(1.f, (panelX - winPos.x) - 6.f);
    ImGui::InvisibleButton("##grml_topbar_drag", ImVec2(dragW, barH));
    if (!g_maximized && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 delta = io.MouseDelta;
        ImGui::SetWindowPos(ImVec2(winPos.x + delta.x, winPos.y + delta.y));
    }

    // 1) Collapse / Expand Content Panel button
    ImGui::SetCursorScreenPos(ImVec2(panelX, btnY));
    if (ImGui::InvisibleButton("##grml_topbar_panel", ImVec2(btnSz, btnSz))) {
        if (g_maximized) {
            g_maximized = false;
        }
        if (!g_contentCollapsed) {
            if (winSize.x >= 400.f) g_expandedSize = winSize;
            g_contentCollapsed = true;
            g_pendingGeometry  = true;
            g_targetPos        = winPos;
            g_targetSize       = ImVec2(210.f, winSize.y);
        } else {
            g_contentCollapsed = false;
            g_pendingGeometry  = true;
            g_targetPos        = winPos;
            g_targetSize       = ImVec2((std::max)(640.f, g_expandedSize.x), winSize.y);
        }
    }
    const bool panelHov = ImGui::IsItemHovered();
    if (panelHov) {
        dl->AddRectFilled(ImVec2(panelX, btnY), ImVec2(panelX + btnSz, btnY + btnSz),
                          IM_COL32(42, 46, 53, 255), 4.f);
        ImGui::SetTooltip("%s", g_contentCollapsed ? "Expand Content Panel" : "Sidebar Only (Collapse Content)");
    }
    {
        const ImU32 icCol = panelHov ? IM_COL32(232, 196, 118, 255) : IM_COL32(154, 160, 170, 255);
        const float rx0 = panelX + 5.f, ry0 = btnY + 6.f;
        const float rx1 = panelX + btnSz - 5.f, ry1 = btnY + btnSz - 6.f;
        dl->AddRect(ImVec2(rx0, ry0), ImVec2(rx1, ry1), icCol, 1.5f, 0, 1.3f);
        dl->AddLine(ImVec2(rx0 + 4.5f, ry0), ImVec2(rx0 + 4.5f, ry1), icCol, 1.2f);
        if (!g_contentCollapsed) {
            dl->AddRectFilled(ImVec2(rx0 + 6.f, ry0 + 2.f), ImVec2(rx1 - 2.f, ry1 - 2.f),
                              IM_COL32(212, 168, 83, 180));
        }
    }

    // 2) Maximize / Restore button
    ImGui::SetCursorScreenPos(ImVec2(maxX, btnY));
    if (ImGui::InvisibleButton("##grml_topbar_max", ImVec2(btnSz, btnSz))) {
        if (!g_maximized) {
            g_restorePos       = winPos;
            g_restoreSize      = g_contentCollapsed ? g_expandedSize : winSize;
            g_contentCollapsed = false;
            g_maximized        = true;
            g_pendingGeometry  = true;
            g_targetPos        = ImVec2(8.f, 8.f);
            g_targetSize       = ImVec2((std::max)(640.f, io.DisplaySize.x - 16.f),
                                        (std::max)(400.f, io.DisplaySize.y - 16.f));
        } else {
            g_maximized       = false;
            g_pendingGeometry = true;
            g_targetPos       = g_restorePos;
            g_targetSize      = ImVec2((std::max)(640.f, g_restoreSize.x),
                                       (std::max)(400.f, g_restoreSize.y));
        }
    }
    const bool maxHov = ImGui::IsItemHovered();
    if (maxHov) {
        dl->AddRectFilled(ImVec2(maxX, btnY), ImVec2(maxX + btnSz, btnY + btnSz),
                          IM_COL32(42, 46, 53, 255), 4.f);
        ImGui::SetTooltip("%s", g_maximized ? "Restore Window" : "Maximize Window");
    }
    {
        const ImU32 icCol = maxHov ? IM_COL32(236, 231, 220, 255) : IM_COL32(154, 160, 170, 255);
        if (!g_maximized) {
            dl->AddRect(ImVec2(maxX + 6.5f, btnY + 6.5f), ImVec2(maxX + btnSz - 6.5f, btnY + btnSz - 6.5f),
                        icCol, 0.f, 0, 1.3f);
        } else {
            dl->AddRect(ImVec2(maxX + 8.5f, btnY + 5.5f), ImVec2(maxX + btnSz - 5.5f, btnY + btnSz - 8.5f),
                        icCol, 0.f, 0, 1.2f);
            dl->AddRectFilled(ImVec2(maxX + 6.f, btnY + 8.f), ImVec2(maxX + btnSz - 8.f, btnY + btnSz - 6.f),
                              IM_COL32(24, 27, 32, 255));
            dl->AddRect(ImVec2(maxX + 6.f, btnY + 8.f), ImVec2(maxX + btnSz - 8.f, btnY + btnSz - 6.f),
                        icCol, 0.f, 0, 1.2f);
        }
    }

    // 3) Close button
    ImGui::SetCursorScreenPos(ImVec2(closeX, btnY));
    if (ImGui::InvisibleButton("##grml_topbar_close", ImVec2(btnSz, btnSz))) {
        if (pOpen) *pOpen = false;
    }
    const bool closeHov = ImGui::IsItemHovered();
    if (closeHov) {
        dl->AddRectFilled(ImVec2(closeX, btnY), ImVec2(closeX + btnSz, btnY + btnSz),
                          IM_COL32(179, 57, 43, 220), 4.f);
        ImGui::SetTooltip("Close [%s / Esc]", hotkeyStr ? hotkeyStr : "F9");
    }
    {
        const ImU32 xCol = closeHov ? IM_COL32(255, 255, 255, 255) : IM_COL32(154, 160, 170, 255);
        const float xPad = 7.f;
        dl->AddLine(ImVec2(closeX + xPad, btnY + xPad),
                    ImVec2(closeX + btnSz - xPad, btnY + btnSz - xPad), xCol, 1.5f);
        dl->AddLine(ImVec2(closeX + btnSz - xPad, btnY + xPad),
                    ImVec2(closeX + xPad, btnY + btnSz - xPad), xCol, 1.5f);
    }

    // Left: Brand title + version + hotkey (compact when sidebar-only)
    const bool compactBar = (winSize.x < 380.f);
    const char* brandText = compactBar ? "GRML" : "GLORIOUS REPUBLIC MOD LOADER";
    char metaBuf[48]{};
    if (compactBar) {
        std::snprintf(metaBuf, sizeof(metaBuf), "[%s]", hotkeyStr ? hotkeyStr : "F9");
    } else {
        std::snprintf(metaBuf, sizeof(metaBuf), "v%s [%s]", GRML_BUILD_VERSION, hotkeyStr ? hotkeyStr : "F9");
    }
    if (g_fontBold) ImGui::PushFont(g_fontBold);
    const ImVec2 bSz = ImGui::CalcTextSize(brandText);
    dl->AddText(ImVec2(winPos.x + 14.f, winPos.y + (barH - bSz.y) * 0.5f),
                IM_COL32(232, 196, 118, 255), brandText);
    if (g_fontBold) ImGui::PopFont();
    const ImVec2 metaSz = ImGui::CalcTextSize(metaBuf);
    dl->AddText(ImVec2(winPos.x + 14.f + bSz.x + 8.f, winPos.y + (barH - metaSz.y) * 0.5f),
                IM_COL32(154, 160, 170, 255), metaBuf);

    // Optional hidden-on-menu status pill (only when expanded)
    if (!compactBar && hiddenInGameCount > 0) {
        char hiddenPill[64]{};
        std::snprintf(hiddenPill, sizeof(hiddenPill), "%zu in-game tab%s hidden on Menu",
                      hiddenInGameCount, hiddenInGameCount == 1 ? "" : "s");
        const ImVec2 hSz  = ImGui::CalcTextSize(hiddenPill);
        const float pillH = 24.f;
        const float py    = winPos.y + (barH - pillH) * 0.5f;
        const float hW    = hSz.x + 18.f;
        const float hX    = panelX - 10.f - hW;
        dl->AddRectFilled(ImVec2(hX, py), ImVec2(hX + hW, py + pillH), IM_COL32(35, 32, 24, 255), 4.f);
        dl->AddRect(ImVec2(hX, py), ImVec2(hX + hW, py + pillH), IM_COL32(212, 168, 83, 255), 4.f);
        dl->AddText(ImVec2(hX + 9.f, py + (pillH - hSz.y) * 0.5f), IM_COL32(232, 196, 118, 255), hiddenPill);
    }

    ImGui::SetCursorPos(ImVec2(0.f, barH));
}

void renderBuiltInLoadedModsView()
{
    ImGui::TextDisabled("In-game Mod Settings & Diagnostics. Enable or disable mods in the GRML Launcher.");
    ImGui::Spacing();

    std::vector<LoadedModSummary> modsCopy;
    {
        std::lock_guard lock(g_tabsMutex);
        modsCopy = g_loadedMods;
    }

    if (beginPanelCard("##card_loaded_mods", "Loaded Mods")) {
        if (modsCopy.empty()) {
            ImGui::TextDisabled("No external plugin DLLs loaded.");
        } else if (ImGui::BeginTable("##grml_mods_tbl", 6,
                                     ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.f);
            ImGui::TableSetupColumn("Mod");
            ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, 72.f);
            ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthFixed, 95.f);
            ImGui::TableSetupColumn("ABI", ImGuiTableColumnFlags_WidthFixed, 75.f);
            ImGui::TableSetupColumn("Status");
            ImGui::TableHeadersRow();

            for (const auto& m : modsCopy) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%u", m.loadOrder);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(m.name.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(m.version.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(m.author.empty() ? "-" : m.author.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(m.loaderApi.c_str());
                ImGui::TableNextColumn();
                if (m.failed) {
                    ImGui::TextColored(ImVec4(0.95f, 0.38f, 0.35f, 1.f), "%s", m.status.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.65f, 0.85f, 0.45f, 1.f), "%s", m.status.c_str());
                }
            }
            ImGui::EndTable();
        }
    }
    endPanelCard();

    ImGui::Spacing();

    std::vector<HookTelemetryEntry> teleCopy;
    {
        std::lock_guard lock(g_telemetryMutex);
        teleCopy = g_telemetry;
    }

    if (beginPanelCard("##card_hook_tele", "Hook Telemetry (Calls/Frame & ms/Frame)")) {
        if (teleCopy.empty()) {
            ImGui::TextDisabled("No hook telemetry reported yet.");
        } else if (ImGui::BeginTable("##grml_tele_tbl", 4,
                                     ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Plugin");
            ImGui::TableSetupColumn("Hook");
            ImGui::TableSetupColumn("Calls / Frame", ImGuiTableColumnFlags_WidthFixed, 100.f);
            ImGui::TableSetupColumn("ms / Frame", ImGuiTableColumnFlags_WidthFixed, 95.f);
            ImGui::TableHeadersRow();

            for (const auto& t : teleCopy) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(t.pluginId.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(t.hookName.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(t.lastFrameCalls));
                ImGui::TableNextColumn(); ImGui::Text("%.4f ms", t.lastFrameMs);
            }
            ImGui::EndTable();
        }
    }
    endPanelCard();
}

void renderBuiltInOverridesView()
{
    auto& vfs = vfs::VfsIndex::get();
    if (beginPanelCard("##card_vfs_overrides", "Asset Overrides (Media VFS)")) {
        if (!vfs.isEnabled()) {
            ImGui::TextDisabled("Asset redirection is disabled in the GRML Launcher.");
        } else {
            ImGui::Text("Indexed Files: %zu  |  Conflicts: %zu  |  Redirected Opens: %llu",
                        vfs.indexedFileCount(),
                        vfs.conflictCount(),
                        static_cast<unsigned long long>(vfs_redirect::redirectHits()));
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            size_t activeCount = 0;
            for (size_t pi = 0; pi < vfs.packs().size(); ++pi) {
                const auto& pk = vfs.packs()[pi];
                if (!pk.enabled || pk.indexedFiles == 0) continue;
                ++activeCount;
                ImGui::BulletText("#%zu [%s] %s (%zu/%zu files, %zu conflicts)",
                                  activeCount,
                                  pk.source == vfs::PackSource::Workshop ? "Workshop" : "Local",
                                  pk.displayName.c_str(),
                                  pk.indexedFiles,
                                  pk.totalFiles,
                                  pk.conflictCount);
            }
            if (activeCount == 0) {
                ImGui::TextDisabled("No active asset overrides.");
            }
        }
    }
    endPanelCard();
}

void renderBuiltInLogView()
{
    if (g_fontBold) ImGui::PushFont(g_fontBold);
    ImGui::TextColored(ImVec4(0.910f, 0.769f, 0.463f, 1.00f), "Runtime Log (grml.log)");
    if (g_fontBold) ImGui::PopFont();
    ImGui::Spacing();

    const auto entries = logger::snapshotRecent(200);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.059f, 0.067f, 0.078f, 1.00f)); // #0f1114
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f, 10.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.f);
    if (ImGui::BeginChild("##grml_log_scroll", ImVec2(0.f, 0.f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding)) {
        if (g_fontMono) ImGui::PushFont(g_fontMono);
        if (entries.empty()) {
            ImGui::TextDisabled("No log entries recorded.");
        } else {
            for (const auto& e : entries) {
                ImVec4 col(0.85f, 0.83f, 0.76f, 1.f);
                if (e.level == GRML_LOG_WARN) col = ImVec4(0.95f, 0.76f, 0.30f, 1.f);
                else if (e.level == GRML_LOG_ERROR) col = ImVec4(0.95f, 0.38f, 0.35f, 1.f);
                ImGui::TextColored(col, "[%s] [%s] %s", e.timestamp, e.component.c_str(), e.message.c_str());
            }
        }
        if (g_fontMono) ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void renderDeclarativeIniTab(RegisteredTab& tab)
{
    ImGui::PushID(tab.pluginId.c_str());
    if (tab.autoGeneratedIni) {
        ImGui::TextDisabled("Auto-built from %s (Ctrl+Click sliders to type custom numbers)",
                            wideToUtf8(tab.iniPath.filename().wstring()).c_str());
        ImGui::Spacing();
    }
    for (size_t i = 0; i < tab.iniFields.size(); ++i) {
        auto& f = tab.iniFields[i];
        ImGui::PushID(static_cast<int>(i));
        switch (f.type) {
        case UiControlType::Header:
            if (i > 0) ImGui::Spacing();
            if (g_fontBold) ImGui::PushFont(g_fontBold);
            ImGui::TextColored(ImVec4(0.831f, 0.659f, 0.325f, 1.00f), "%s", f.label.c_str());
            if (g_fontBold) ImGui::PopFont();
            ImGui::Separator();
            break;
        case UiControlType::Bool: {
            bool b = (f.boolVal != 0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(f.label.c_str());
            if (!f.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", f.tooltip.c_str());
            }
            const float avail = ImGui::GetContentRegionAvail().x;
            ImGui::SameLine(avail > 48.f ? avail - 36.f : 0.f);
            if (drawToggleSwitch("##toggle", &b)) {
                f.boolVal = b ? 1u : 0u;
                syncDeclarativeLivePtrs(tab);
                if (tab.onIniApplied) tab.onIniApplied(tab.iniUserData);
            }
            break;
        }
        case UiControlType::Int: {
            const char* fmt = f.format.empty() ? "%d" : f.format.c_str();
            const ImGuiSliderFlags flags = f.hasExplicitRange ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
            if (ImGui::SliderInt(f.label.c_str(), &f.intVal,
                                 static_cast<int>(f.minVal), static_cast<int>(f.maxVal), fmt, flags)) {
                syncDeclarativeLivePtrs(tab);
                if (tab.onIniApplied) tab.onIniApplied(tab.iniUserData);
            }
            break;
        }
        case UiControlType::Float: {
            const char* fmt = f.format.empty() ? "%.2f" : f.format.c_str();
            const ImGuiSliderFlags flags = f.hasExplicitRange ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
            if (ImGui::SliderFloat(f.label.c_str(), &f.floatVal,
                                   static_cast<float>(f.minVal), static_cast<float>(f.maxVal), fmt, flags)) {
                syncDeclarativeLivePtrs(tab);
                if (tab.onIniApplied) tab.onIniApplied(tab.iniUserData);
            }
            break;
        }
        case UiControlType::String:
            ImGui::InputText(f.label.c_str(), f.strVal, sizeof(f.strVal));
            break;
        case UiControlType::Select:
            if (!f.options.empty()) {
                const char* preview = (f.selectedIdx >= 0 && static_cast<size_t>(f.selectedIdx) < f.options.size())
                                          ? f.options[static_cast<size_t>(f.selectedIdx)].c_str()
                                          : "";
                if (ImGui::BeginCombo(f.label.c_str(), preview)) {
                    for (size_t oi = 0; oi < f.options.size(); ++oi) {
                        const bool selected = (static_cast<int32_t>(oi) == f.selectedIdx);
                        if (ImGui::Selectable(f.options[oi].c_str(), selected)) {
                            f.selectedIdx = static_cast<int32_t>(oi);
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            break;
        }
        if (f.type != UiControlType::Bool && !f.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", f.tooltip.c_str());
        }
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (drawPrimaryButton("Apply & Save")) {
        tab.iniStatusMsg = saveDeclarativeIni(tab)
                               ? ("Saved " + wideToUtf8(tab.iniPath.filename().wstring()))
                               : "ERROR: Failed to save .ini file";
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Defaults")) {
        for (auto& f : tab.iniFields) {
            switch (f.type) {
            case UiControlType::Bool:   f.boolVal  = (f.defaultVal != 0.0) ? 1u : 0u; break;
            case UiControlType::Int:    f.intVal   = static_cast<int32_t>(f.defaultVal); break;
            case UiControlType::Float:  f.floatVal = static_cast<float>(f.defaultVal); break;
            case UiControlType::String: std::snprintf(f.strVal, sizeof(f.strVal), "%s", f.defaultStr.c_str()); break;
            case UiControlType::Select:
                f.selectedIdx = 0;
                for (size_t oi = 0; oi < f.options.size(); ++oi) {
                    if (f.options[oi] == f.defaultStr) { f.selectedIdx = static_cast<int32_t>(oi); break; }
                }
                break;
            case UiControlType::Header: break;
            }
        }
        syncDeclarativeLivePtrs(tab);
        if (tab.onIniApplied) tab.onIniApplied(tab.iniUserData);
        tab.iniStatusMsg = "Defaults restored (click Apply & Save to persist).";
    }
    if (!tab.iniStatusMsg.empty()) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tab.iniStatusMsg.c_str());
    }
    ImGui::PopID();
}

void renderLegacyWidget(const LegacyStoredWidget& w, size_t idx)
{
    ImGui::PushID(static_cast<int>(idx));
    switch (w.kind) {
    case LegacyWidgetKind::FloatSlider:
        if (w.floatPtr) {
            const char* fmt = w.format.empty() ? "%.2f" : w.format.c_str();
            if (ImGui::SliderFloat(w.label.c_str(), w.floatPtr, w.minVal, w.maxVal, fmt, ImGuiSliderFlags_AlwaysClamp)) {
                if (w.onChange) w.onChange(w.userData);
            }
            if (!w.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", w.tooltip.c_str());
            }
        }
        break;
    case LegacyWidgetKind::Checkbox:
        if (w.boolPtr) {
            bool checked = (*w.boolPtr != 0);
            if (ImGui::Checkbox(w.label.c_str(), &checked)) {
                *w.boolPtr = checked ? 1u : 0u;
                if (w.onChange) w.onChange(w.userData);
            }
            if (!w.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", w.tooltip.c_str());
            }
        }
        break;
    case LegacyWidgetKind::Button:
        if (w.sameLine) ImGui::SameLine();
        if (ImGui::Button(w.label.c_str())) {
            if (w.onClick) w.onClick(w.userData);
        }
        if (!w.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", w.tooltip.c_str());
        }
        break;
    case LegacyWidgetKind::Label:
        if (w.separatorBefore) ImGui::Separator();
        if (w.rgbaColor != 0) ImGui::TextColored(unpackRgba(w.rgbaColor), "%s", w.persistentText ? w.persistentText : "");
        else ImGui::TextWrapped("%s", w.persistentText ? w.persistentText : "");
        break;
    case LegacyWidgetKind::CollapsibleLabel:
        if (w.separatorBefore) ImGui::Separator();
        if (ImGui::CollapsingHeader(w.label.c_str(), w.defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None)) {
            if (w.rgbaColor != 0) ImGui::TextColored(unpackRgba(w.rgbaColor), "%s", w.persistentText ? w.persistentText : "");
            else ImGui::TextWrapped("%s", w.persistentText ? w.persistentText : "");
        }
        break;
    }
    ImGui::PopID();
}

void rolloverTelemetryFrame()
{
    std::lock_guard lock(g_telemetryMutex);
    for (auto& e : g_telemetry) {
        e.lastFrameCalls = e.callsThisFrame;
        e.lastFrameMs    = e.msThisFrame;
        e.callsThisFrame = 0;
        e.msThisFrame    = 0.0;
    }
}

void renderUI()
{
    rolloverTelemetryFrame();

    const bool mainVisible = g_visible.load(std::memory_order_relaxed);
    const bool extVisible  = g_anyExtWindowVisible.load(std::memory_order_relaxed);
    if (!mainVisible && !extVisible) {
        g_wantCaptureMouse.store(false, std::memory_order_relaxed);
        g_wantCaptureKeyboard.store(false, std::memory_order_relaxed);
        g_rectValid.store(false, std::memory_order_relaxed);
        ImGui::GetIO().MouseDrawCursor = false;
        return;
    }

    ImGuiIO& io        = ImGui::GetIO();
    io.MouseDrawCursor = isMouseLockedForGame();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    bool open = mainVisible;
    if (mainVisible) {
        if (g_pendingGeometry) {
            ImGui::SetNextWindowPos(g_targetPos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(g_targetSize, ImGuiCond_Always);
            g_pendingGeometry = false;
        } else {
            ImGui::SetNextWindowSize(ImVec2(840.f, 540.f), ImGuiCond_FirstUseEver);
        }
        const float minW = g_contentCollapsed ? 210.f : 640.f;
        ImGui::SetNextWindowSizeConstraints(ImVec2(minW, 400.f), ImVec2(FLT_MAX, FLT_MAX));
    }
    char hotkeyBuf[16]{};
    const uint16_t curVk = g_toggleVk.load(std::memory_order_relaxed);
    if (curVk >= VK_F1 && curVk <= VK_F12) {
        std::snprintf(hotkeyBuf, sizeof(hotkeyBuf), "F%u", static_cast<unsigned>(curVk - VK_F1 + 1));
    } else if (curVk == VK_INSERT) {
        std::snprintf(hotkeyBuf, sizeof(hotkeyBuf), "Insert");
    } else if (curVk == VK_HOME) {
        std::snprintf(hotkeyBuf, sizeof(hotkeyBuf), "Home");
    } else if (curVk == VK_END) {
        std::snprintf(hotkeyBuf, sizeof(hotkeyBuf), "End");
    } else if (curVk == VK_PRIOR) {
        std::snprintf(hotkeyBuf, sizeof(hotkeyBuf), "PageUp");
    } else if (curVk == VK_NEXT) {
        std::snprintf(hotkeyBuf, sizeof(hotkeyBuf), "PageDown");
    } else if (curVk == VK_PAUSE) {
        std::snprintf(hotkeyBuf, sizeof(hotkeyBuf), "Pause");
    } else if (curVk == VK_OEM_3) {
        std::snprintf(hotkeyBuf, sizeof(hotkeyBuf), "~");
    } else if ((curVk >= 'A' && curVk <= 'Z') || (curVk >= '0' && curVk <= '9')) {
        std::snprintf(hotkeyBuf, sizeof(hotkeyBuf), "%c", static_cast<char>(curVk));
    } else {
        std::snprintf(hotkeyBuf, sizeof(hotkeyBuf), "F9");
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    const ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                      (g_maximized ? ImGuiWindowFlags_NoResize : ImGuiWindowFlags_None);
    const bool windowOpen = mainVisible && ImGui::Begin("###GRML_ModSettings", &open, winFlags);
    ImGui::PopStyleVar();
    if (mainVisible) {
        const ImVec2 pos  = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        g_rectL.store(static_cast<int32_t>(pos.x), std::memory_order_relaxed);
        g_rectT.store(static_cast<int32_t>(pos.y), std::memory_order_relaxed);
        g_rectR.store(static_cast<int32_t>(pos.x + size.x), std::memory_order_relaxed);
        g_rectB.store(static_cast<int32_t>(pos.y + size.y), std::memory_order_relaxed);
        g_rectValid.store(true, std::memory_order_release);
    }

    if (windowOpen) {
        const bool gameRunning = isGameRunning();
        size_t modCount           = 0;
        size_t visibleModTabCount = 0;
        size_t hiddenInGameCount  = 0;
        {
            std::lock_guard lock(g_tabsMutex);
            modCount = g_loadedMods.size();
            for (const auto& tab : g_tabs) {
                if (!gameRunning && tab.mainMenuAvailable == 0) ++hiddenInGameCount;
                else ++visibleModTabCount;
            }
        }
        size_t activeOverridePacks = 0;
        for (const auto& pk : vfs::VfsIndex::get().packs()) {
            if (pk.enabled && pk.indexedFiles > 0) ++activeOverridePacks;
        }

        const ImVec2 mainWinPos  = ImGui::GetWindowPos();
        const ImVec2 mainWinSize = ImGui::GetWindowSize();
        auto selectNav = [&](const std::string& key) {
            g_activeNavKey = key;
            if (g_contentCollapsed) {
                g_contentCollapsed = false;
                g_pendingGeometry  = true;
                g_targetPos        = mainWinPos;
                g_targetSize       = ImVec2((std::max)(640.f, g_expandedSize.x), mainWinSize.y);
            }
        };

        renderTopbar(hotkeyBuf, hiddenInGameCount, &open);

        // ── Left Sidebar Navigation ──────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.082f, 0.090f, 0.106f, 1.00f)); // #15171b
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, 10.f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.f);
        if (ImGui::BeginChild("##grml_sidebar", ImVec2(210.f, 0.f),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            char modBadge[16]{};
            std::snprintf(modBadge, sizeof(modBadge), "%zu", modCount);
            if (renderSidebarNavItem("##nav_loaded_mods", "Loaded Mods",
                                     g_activeNavKey == "__builtin_loaded_mods", modBadge)) {
                selectNav("__builtin_loaded_mods");
            }

            char ovBadge[16]{};
            if (activeOverridePacks > 0) {
                std::snprintf(ovBadge, sizeof(ovBadge), "%zu", activeOverridePacks);
            }
            if (renderSidebarNavItem("##nav_overrides", "Asset Overrides",
                                     g_activeNavKey == "__builtin_overrides",
                                     activeOverridePacks > 0 ? ovBadge : nullptr)) {
                selectNav("__builtin_overrides");
            }

            if (renderSidebarNavItem("##nav_logs", "Runtime Log",
                                     g_activeNavKey == "__builtin_logs")) {
                selectNav("__builtin_logs");
            }

            // Fixed MOD SETTINGS header + scrollable list strictly underneath
            if (visibleModTabCount > 0) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextDisabled("MOD SETTINGS");
                ImGui::Spacing();

                if (ImGui::BeginChild("##grml_sidebar_mods", ImVec2(0.f, 0.f),
                                      ImGuiChildFlags_None)) {
                    std::lock_guard lock(g_tabsMutex);
                    for (const auto& tab : g_tabs) {
                        if (!gameRunning && tab.mainMenuAvailable == 0) continue;
                        const std::string key = "tab_" + tab.pluginId + "_" + std::to_string(tab.tabId);
                        const std::string navId = "##nav_" + key;
                        if (renderSidebarNavItem(navId.c_str(), tab.title.c_str(),
                                                 g_activeNavKey == key,
                                                 tab.uiFaulted ? "FAULT" : nullptr,
                                                 tab.uiFaulted)) {
                            selectNav(key);
                        }
                    }
                }
                ImGui::EndChild();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        // ── Right Main Content Pane ──────────────────────────────────────────
        if (!g_contentCollapsed) {
        ImGui::SameLine(0.f, 0.f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.075f, 0.082f, 0.094f, 1.00f)); // #131518
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.f, 14.f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.f);
        if (ImGui::BeginChild("##grml_content", ImVec2(0.f, 0.f),
                              ImGuiChildFlags_AlwaysUseWindowPadding)) {
            bool fallbackToLoadedMods = false;
            if (g_activeNavKey == "__builtin_loaded_mods") {
                renderBuiltInLoadedModsView();
            } else if (g_activeNavKey == "__builtin_overrides") {
                renderBuiltInOverridesView();
            } else if (g_activeNavKey == "__builtin_logs") {
                renderBuiltInLogView();
            } else {
                std::lock_guard lock(g_tabsMutex);
                RegisteredTab* selectedTab = nullptr;
                for (auto& tab : g_tabs) {
                    if (!gameRunning && tab.mainMenuAvailable == 0) continue;
                    const std::string key = "tab_" + tab.pluginId + "_" + std::to_string(tab.tabId);
                    if (key == g_activeNavKey) {
                        selectedTab = &tab;
                        break;
                    }
                }
                if (!selectedTab) {
                    g_activeNavKey = "__builtin_loaded_mods";
                    fallbackToLoadedMods = true;
                } else {
                    if (g_fontBold) ImGui::PushFont(g_fontBold);
                    ImGui::TextColored(ImVec4(0.910f, 0.769f, 0.463f, 1.00f), "%s", selectedTab->title.c_str());
                    if (g_fontBold) ImGui::PopFont();
                    ImGui::Spacing();

                    if (selectedTab->uiFaulted) {
                        ImGui::TextColored(ImVec4(0.95f, 0.38f, 0.35f, 1.f),
                                           "[UI Faulted - Disabled] Exception 0x%08lX caught inside plugin tab '%s'.",
                                           selectedTab->faultCode, selectedTab->title.c_str());
                    } else if (selectedTab->kind == TabKind::CustomGrml) {
                        ImGui::PushID(selectedTab->pluginId.c_str());
                        const bool childOk = ImGui::BeginChild("##grml_custom_sandbox", ImVec2(0.f, 0.f),
                                                               ImGuiChildFlags_None);
                        if (childOk) {
                            g_activeDisabledDepth = 0;
                            DWORD excCode = 0;
                            if (!invokeRenderTabSeh(selectedTab->renderCb, &g_grmlUiApi, selectedTab->userData, &excCode)) {
                                selectedTab->uiFaulted = true;
                                selectedTab->faultCode = excCode;
                                logger::logMessage(GRML_LOG_ERROR, "OverlayHub",
                                                   "Plugin tab '%s' (%s) faulted with exception 0x%08lX — marked [UI Faulted - Disabled]",
                                                   selectedTab->title.c_str(), selectedTab->pluginId.c_str(), excCode);
                            }
                            while (g_activeDisabledDepth > 0) {
                                ImGui::EndDisabled();
                                --g_activeDisabledDepth;
                            }
                        }
                        ImGui::EndChild();
                        ImGui::PopID();
                    } else if (selectedTab->kind == TabKind::DeclarativeIni) {
                        ImGui::PushID(selectedTab->pluginId.c_str());
                        if (beginPanelCard("##grml_ini_card")) {
                            renderDeclarativeIniTab(*selectedTab);
                        }
                        endPanelCard();
                        ImGui::PopID();
                    } else if (selectedTab->kind == TabKind::LegacyRetained) {
                        ImGui::PushID(selectedTab->pluginId.c_str());
                        if (ImGui::BeginChild("##grml_legacy_sandbox", ImVec2(0.f, 0.f),
                                              ImGuiChildFlags_None)) {
                            if (selectedTab->legacyFrame) {
                                DWORD excCode = 0;
                                if (!invokeLegacyButtonSeh(selectedTab->legacyFrame, selectedTab->legacyFrameUserData, &excCode)) {
                                    selectedTab->uiFaulted = true;
                                    selectedTab->faultCode = excCode;
                                    logger::logMessage(GRML_LOG_ERROR, "OverlayHub",
                                                       "Legacy tab frame callback '%s' faulted (0x%08lX) — marked [UI Faulted - Disabled]",
                                                       selectedTab->title.c_str(), excCode);
                                }
                            }
                            if (!selectedTab->uiFaulted) {
                                if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && selectedTab->legacySave) {
                                    DWORD excCode = 0;
                                    invokeLegacyButtonSeh(selectedTab->legacySave, selectedTab->legacyUserData, &excCode);
                                }
                                for (size_t i = 0; i < selectedTab->legacyWidgets.size(); ++i) {
                                    renderLegacyWidget(selectedTab->legacyWidgets[i], i);
                                }
                            }
                        }
                        ImGui::EndChild();
                        ImGui::PopID();
                    }
                }
            }
            if (fallbackToLoadedMods) {
                renderBuiltInLoadedModsView();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        }
    }
    if (mainVisible) {
        ImGui::End();
    }

    // Render standalone plugin windows (e.g. F8 pop-out tools)
    {
        std::lock_guard lock(g_winHotkeyMutex);
        bool visChanged = false;
        for (auto& w : g_windows) {
            if (!w.visible) continue;
            bool wOpen = true;
            ImGui::SetNextWindowSize(ImVec2(760.f, 520.f), ImGuiCond_FirstUseEver);
            std::string winTitle = w.title + "###GRML_Win_" + w.pluginId + "_" + w.windowId;
            if (ImGui::Begin(winTitle.c_str(), &wOpen, ImGuiWindowFlags_NoCollapse)) {
                ImGui::PushID(w.winId);
                if (w.uiFaulted) {
                    ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.f),
                                       "[UI Faulted - Disabled] Exception 0x%08lX", w.faultCode);
                } else if (w.renderCb) {
                    DWORD excCode = 0;
                    if (!invokeRenderTabSeh(w.renderCb, &g_grmlUiApi, w.userData, &excCode)) {
                        w.uiFaulted = true;
                        w.faultCode = excCode;
                        logger::logMessage(GRML_LOG_ERROR, "OverlayHub",
                                           "Window '%s' faulted (0x%08lX) — marked [UI Faulted - Disabled]",
                                           w.title.c_str(), excCode);
                    }
                }
                ImGui::PopID();
            }
            ImGui::End();
            if (!wOpen) {
                w.visible = false;
                visChanged = true;
            }
        }
        if (visChanged) refreshAnyExtWindowVisibleLocked();
    }

    const bool mouseCaptured =
        io.WantCaptureMouse ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow |
                               ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
        ImGui::IsAnyItemActive();
    g_wantCaptureMouse.store(mouseCaptured, std::memory_order_relaxed);
    g_wantCaptureKeyboard.store(io.WantTextInput || io.WantCaptureKeyboard, std::memory_order_relaxed);
    io.MouseDrawCursor = isMouseLockedForGame();

    if (mainVisible && !open) toggleOverlay(false);

    ImGui::Render();
    std::lock_guard lock(g_d3dMutex);
    if (g_rtv && g_ctx) {
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

struct WrsrSaveEntry0x20C {
    uint32_t flags{0};
    wchar_t  displayName[128]{};
    char     relPath[264]{};
};
static_assert(sizeof(WrsrSaveEntry0x20C) == 0x20C, "SaveEntry size must match WRSR 0x20C");

bool sehInvokeDirectSaveLoad(const DirectSaveLoadTargets* t,
                             const wchar_t*               saveFolderW,
                             const char*                  relPathUtf8,
                             DWORD*                       outExc) noexcept
{
    __try {
        WrsrSaveEntry0x20C entry{};
        wcsncpy_s(entry.displayName, _countof(entry.displayName), saveFolderW, _TRUNCATE);
        strncpy_s(entry.relPath, sizeof(entry.relPath), relPathUtf8, _TRUNCATE);

        auto* dstRel       = reinterpret_cast<char*>(t->mainMenuObj + 0xA20);
        auto* dstName      = reinterpret_cast<wchar_t*>(t->mainMenuObj + 0x720);
        auto* dstGlobalRel = const_cast<char*>(reinterpret_cast<volatile char*>(t->loadRequestedPtr + 6));
        strncpy_s(dstRel, 260, relPathUtf8, _TRUNCATE);
        wcsncpy_s(dstName, 128, saveFolderW, _TRUNCATE);
        strncpy_s(dstGlobalRel, 260, relPathUtf8, _TRUNCATE);

        using PFN_LoadSaveHeader = void(__fastcall*)(void* mainMenuObj, const void* saveEntry);
        auto fn = reinterpret_cast<PFN_LoadSaveHeader>(t->loadSaveHeaderFn);
        fn(t->mainMenuObj, &entry);

        *reinterpret_cast<uint8_t*>(t->mainMenuObj + 0x10C0) = 0;
        *t->newGameFlagPtr       = 0;
        *(t->newGameFlagPtr + 3) = 0;
        *t->loadRequestedPtr     = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outExc) *outExc = GetExceptionCode();
        return false;
    }
}

void processPendingDirectSaveLoad()
{
    if (!save_events::hasPendingAutoLoad()) return;
    if (!g_directLoadScanCompleted.load(std::memory_order_acquire)) return;

    if (!g_directLoadConfigured.load(std::memory_order_acquire)) {
        const std::wstring dropped = save_events::consumePendingAutoLoad();
        if (!dropped.empty()) {
            logger::logMessage(GRML_LOG_WARN, "DirectLoad",
                               "Direct-to-Save disabled (missing signature) — staying at Main Menu");
        }
        return;
    }

    const auto& t = g_directLoadTargets;
    if (!t.menuActivePtr || !t.mainMenuObj || !t.loadSaveHeaderFn || !t.loadRequestedPtr || !t.newGameFlagPtr) {
        save_events::consumePendingAutoLoad();
        return;
    }
    if (*t.menuActivePtr == 0 || isGameRunning()) return;
    const int32_t uiState = *reinterpret_cast<const int32_t*>(t.mainMenuObj + t.uiStateOff);
    if (uiState != 1 && uiState != 2) return;

    const std::wstring saveFolder = save_events::consumePendingAutoLoad();
    if (saveFolder.empty()) return;

    std::error_code ec;
    const std::filesystem::path saveBase = g_saveRootOverride.empty()
        ? (g_gameDir / L"media_soviet" / L"save")
        : g_saveRootOverride;
    const std::filesystem::path headerPath = saveBase / saveFolder / L"header.bin";
    if (!std::filesystem::exists(headerPath, ec)) {
        logger::logMessage(GRML_LOG_WARN, "DirectLoad",
                           "Save '%s' not found on disk (%s) — staying at Main Menu",
                           wideToUtf8(saveFolder).c_str(),
                           wideToUtf8(headerPath.wstring()).c_str());
        return;
    }

    const std::string relPathUtf8 = "save/" + wideToUtf8(saveFolder);
    DWORD exc = 0;
    if (sehInvokeDirectSaveLoad(&t, saveFolder.c_str(), relPathUtf8.c_str(), &exc)) {
        logger::logMessage(GRML_LOG_INFO, "DirectLoad",
                           "Triggered direct save load for '%s'", wideToUtf8(saveFolder).c_str());
    } else {
        logger::logMessage(GRML_LOG_ERROR, "DirectLoad",
                           "LoadSaveHeaderAndCheckItems faulted (0x%08lX) for '%s' — staying at Main Menu",
                           exc, wideToUtf8(saveFolder).c_str());
    }
}

HRESULT WINAPI hookedPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags)
{
    if (!g_dxInitialized.exchange(true)) {
        sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device));
        g_device->GetImmediateContext(&g_ctx);
        DXGI_SWAP_CHAIN_DESC desc{};
        sc->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;
        g_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hookedWndProc)));
        installDirectInputGuard();
        installC3dInputGuards();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io    = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;
        applyWrsrTheme();
        loadLauncherFonts();
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX11_Init(g_device, g_ctx);
        createRenderTarget(sc);
        logger::logMessage(GRML_LOG_INFO, "OverlayHub", "DX11 + ImGui F9 Mod Settings hub initialized");
    }
    renderUI();
    processPendingDirectSaveLoad();
    return g_origPresent(sc, syncInterval, flags);
}

HRESULT WINAPI hookedResizeBuffers(IDXGISwapChain* sc, UINT bufCount, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    releaseRenderTarget();
    ImGui_ImplDX11_InvalidateDeviceObjects();
    HRESULT hr = g_origResizeBuffers(sc, bufCount, w, h, fmt, flags);
    ImGui_ImplDX11_CreateDeviceObjects();
    createRenderTarget(sc);
    return hr;
}

bool installDx11Hooks()
{
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcW;
    wc.lpszClassName = L"GRML_DX11_Probe";
    wc.hInstance     = GetModuleHandleW(nullptr);
    RegisterClassExW(&wc);

    HWND hwndProbe = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                     0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwndProbe) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount       = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow      = hwndProbe;
    scd.SampleDesc.Count  = 1;
    scd.Windowed          = TRUE;
    scd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* probeDev = nullptr;
    ID3D11DeviceContext* probeCtx = nullptr;
    IDXGISwapChain* probeSc = nullptr;
    D3D_FEATURE_LEVEL fl{};
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &scd, &probeSc, &probeDev, &fl, &probeCtx);
    if (FAILED(hr) || !probeSc) {
        DestroyWindow(hwndProbe);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    void** vtbl = *reinterpret_cast<void***>(probeSc);
    const MH_STATUS st = MH_Initialize();
    bool ok = (st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED) &&
              MH_CreateHook(vtbl[8], reinterpret_cast<void*>(hookedPresent), reinterpret_cast<void**>(&g_origPresent)) == MH_OK &&
              MH_CreateHook(vtbl[13], reinterpret_cast<void*>(hookedResizeBuffers), reinterpret_cast<void**>(&g_origResizeBuffers)) == MH_OK &&
              MH_EnableHook(vtbl[8]) == MH_OK &&
              MH_EnableHook(vtbl[13]) == MH_OK;

    if (ok) {
        g_presentTarget       = vtbl[8];
        g_resizeBuffersTarget = vtbl[13];
    }

    probeSc->Release();
    probeDev->Release();
    probeCtx->Release();
    DestroyWindow(hwndProbe);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

} // namespace

void setInGameFlagPtr(const volatile uint8_t* ptr) noexcept
{
    g_inGameFlagPtr.store(ptr, std::memory_order_release);
}

void setDirectSaveLoadTargets(const DirectSaveLoadTargets& targets,
                              const std::filesystem::path& gameDir,
                              const std::filesystem::path& saveRootOverride)
{
    g_directLoadTargets = targets;
    g_gameDir           = gameDir;
    g_saveRootOverride  = saveRootOverride;
    const bool ok = targets.menuActivePtr && targets.mainMenuObj &&
                    targets.loadSaveHeaderFn && targets.loadRequestedPtr && targets.newGameFlagPtr;
    g_directLoadConfigured.store(ok, std::memory_order_release);
    g_directLoadScanCompleted.store(true, std::memory_order_release);
}

void setPluginsDirectory(const std::filesystem::path& pluginsDir)
{
    std::lock_guard lock(g_tabsMutex);
    g_pluginsDir = pluginsDir;
}

void publishLoadedMods(std::vector<LoadedModSummary> mods)
{
    std::lock_guard lock(g_tabsMutex);
    g_loadedMods = std::move(mods);
}

bool hasRegisteredTabForPlugin(const std::string& pluginId)
{
    std::lock_guard lock(g_tabsMutex);
    for (const auto& t : g_tabs) {
        if (t.pluginId == pluginId) return true;
    }
    return false;
}

int32_t registerExternalUiSchemaTab(const std::string& pluginId,
                                    const std::string& fallbackTitle,
                                    const std::filesystem::path& dllPath,
                                    const std::filesystem::path& manifestPath,
                                    const UiSchema& schema)
{
    if (!schema.valid) return -1;

    RegisteredTab tab;
    tab.kind              = TabKind::DeclarativeIni;
    tab.autoGeneratedIni  = schema.autoGenerated;
    tab.pluginId          = pluginId;
    tab.title             = schema.tabTitle.empty() ? fallbackTitle : schema.tabTitle;
    tab.mainMenuAvailable = schema.mainMenuAvailable ? 1u : 0u;
    tab.iniPath           = findModIniPath(dllPath, manifestPath, &schema);

    for (const auto& c : schema.controls) {
        DeclarativeFieldRuntime rf;
        rf.section          = c.section;
        rf.key              = c.key;
        rf.label            = c.label;
        rf.tooltip          = c.tooltip;
        rf.format           = c.format;
        rf.type             = c.type;
        rf.minVal           = c.minValue;
        rf.maxVal           = c.maxValue;
        rf.hasExplicitRange = c.hasExplicitRange;
        rf.defaultStr       = c.defaultValue;
        rf.options          = c.options;
        if (c.type == UiControlType::Bool) {
            rf.defaultVal = (c.defaultValue == "1" || c.defaultValue == "true") ? 1.0 : 0.0;
        } else if (c.type == UiControlType::Int || c.type == UiControlType::Float) {
            rf.defaultVal = std::strtod(c.defaultValue.c_str(), nullptr);
        }
        tab.iniFields.push_back(std::move(rf));
    }

    loadDeclarativeIni(tab);
    tab.iniStatusMsg = std::string(schema.autoGenerated ? "Auto INI UI -> " : "Schema UI -> ") +
                       wideToUtf8(tab.iniPath.filename().wstring());

    std::lock_guard lock(g_tabsMutex);
    tab.tabId = static_cast<int32_t>(g_tabs.size());
    g_tabs.push_back(std::move(tab));
    return g_tabs.back().tabId;
}

bool install(uint16_t toggleVk)
{
    if (g_installed.exchange(true)) return true;
    g_toggleVk.store(toggleVk, std::memory_order_relaxed);
    publishLegacyOverlayIpcHub();
    if (!installDx11Hooks()) {
        g_installed.store(false);
        logger::logMessage(GRML_LOG_ERROR, "OverlayHub", "Failed to install DX11 Present/ResizeBuffers hooks");
        return false;
    }
    return true;
}

void shutdown()
{
    if (!g_installed.exchange(false)) return;
    if (g_hwnd && g_origWndProc) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        g_origWndProc = nullptr;
    }
    if (g_presentTarget) { MH_DisableHook(g_presentTarget); MH_RemoveHook(g_presentTarget); g_presentTarget = nullptr; }
    if (g_resizeBuffersTarget) { MH_DisableHook(g_resizeBuffersTarget); MH_RemoveHook(g_resizeBuffersTarget); g_resizeBuffersTarget = nullptr; }
    if (g_diTarget) { MH_DisableHook(g_diTarget); MH_RemoveHook(g_diTarget); g_diTarget = nullptr; }

    if (g_dxInitialized.exchange(false)) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    releaseRenderTarget();
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    if (g_ipcHub) { UnmapViewOfFile(g_ipcHub); g_ipcHub = nullptr; }
    if (g_ipcMapHandle) { CloseHandle(g_ipcMapHandle); g_ipcMapHandle = nullptr; }
}

const GrmlUiApi* uiApi() noexcept
{
    return &g_grmlUiApi;
}

} // namespace grml::overlay
