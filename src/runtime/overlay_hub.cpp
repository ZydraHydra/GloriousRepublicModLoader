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

std::mutex                      g_tabsMutex;
std::vector<RegisteredTab>      g_tabs;
std::vector<LoadedModSummary>   g_loadedMods;
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

std::mutex              g_d3dMutex;
ID3D11Device*           g_device      = nullptr;
ID3D11DeviceContext*    g_ctx         = nullptr;
ID3D11RenderTargetView* g_rtv         = nullptr;
HWND                    g_hwnd        = nullptr;
WNDPROC                 g_origWndProc = nullptr;

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
    style.WindowPadding     = ImVec2(12.f, 10.f);
    style.FramePadding      = ImVec2(8.f, 4.f);
    style.ItemSpacing       = ImVec2(8.f, 6.f);
    style.WindowRounding    = 2.f;
    style.ChildRounding     = 2.f;
    style.FrameRounding     = 2.f;
    style.TabRounding       = 2.f;
    style.ScrollbarRounding = 2.f;
    style.WindowBorderSize  = 1.f;
    style.FrameBorderSize   = 1.f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                 = ImVec4(0.91f, 0.88f, 0.80f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.55f, 0.52f, 0.45f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.12f, 0.12f, 0.10f, 0.96f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.14f, 0.13f, 0.11f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.14f, 0.13f, 0.11f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.32f, 0.29f, 0.24f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.16f, 0.15f, 0.12f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.27f, 0.22f, 0.14f, 1.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.09f, 0.09f, 0.07f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.20f, 0.18f, 0.13f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.28f, 0.23f, 0.15f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.24f, 0.21f, 0.16f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.42f, 0.34f, 0.20f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.58f, 0.45f, 0.22f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.28f, 0.24f, 0.16f, 0.90f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.42f, 0.34f, 0.20f, 0.95f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.55f, 0.43f, 0.22f, 1.00f);
    c[ImGuiCol_Tab]                  = ImVec4(0.18f, 0.16f, 0.12f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.42f, 0.34f, 0.20f, 1.00f);
    c[ImGuiCol_TabSelected]          = ImVec4(0.32f, 0.26f, 0.16f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.68f, 0.52f, 0.22f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.86f, 0.67f, 0.29f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.84f, 0.65f, 0.27f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.35f, 0.31f, 0.24f, 1.00f);
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
    if (msg == WM_KEYDOWN && wp == vk && ((lp & (1LL << 30)) == 0)) {
        toggleOverlay(!g_visible.load(std::memory_order_relaxed));
        return 0;
    }

    if (g_visible.load(std::memory_order_relaxed)) {
        if (msg == WM_KEYDOWN && wp == VK_ESCAPE && ((lp & (1LL << 30)) == 0)) {
            toggleOverlay(false);
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

void renderBuiltInLoadedModsTab()
{
    ImGui::TextColored(ImVec4(0.91f, 0.78f, 0.45f, 1.f),
                       "Glorious Republic Mod Loader (GRML v%s)", GRML_BUILD_VERSION);
    ImGui::TextDisabled("In-game Mod Settings & Diagnostics. Enable or disable mods in the GRML Launcher.");
    ImGui::Separator();

    // Read-only Loaded Mods table
    std::vector<LoadedModSummary> modsCopy;
    {
        std::lock_guard lock(g_tabsMutex);
        modsCopy = g_loadedMods;
    }

    if (ImGui::CollapsingHeader("Loaded Mods", ImGuiTreeNodeFlags_DefaultOpen)) {
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

    // Live Hook Telemetry table
    std::vector<HookTelemetryEntry> teleCopy;
    {
        std::lock_guard lock(g_telemetryMutex);
        teleCopy = g_telemetry;
    }

    if (ImGui::CollapsingHeader("Hook Telemetry (Calls/Frame & ms/Frame)", ImGuiTreeNodeFlags_DefaultOpen)) {
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

    // Built-in Asset Overrides (Media VFS) telemetry & live toggle
    {
        auto& vfs = vfs::VfsIndex::get();
        if (ImGui::CollapsingHeader("Asset Overrides (Media VFS)")) {
            bool vfsOn = vfs.isEnabled();
            if (ImGui::Checkbox("Enable Runtime Asset Redirection", &vfsOn)) {
                vfs.setEnabled(vfsOn);
            }
            ImGui::Text("Indexed Files: %zu  |  Conflicts: %zu  |  Redirected Opens: %llu",
                        vfs.indexedFileCount(),
                        vfs.conflictCount(),
                        static_cast<unsigned long long>(vfs_redirect::redirectHits()));
            if (vfs.packs().empty()) {
                ImGui::TextDisabled("No override packs found in media_overrides\\ or Workshop.");
            } else {
                for (size_t pi = 0; pi < vfs.packs().size(); ++pi) {
                    const auto& pk = vfs.packs()[pi];
                    ImGui::BulletText("#%zu [%s] %s (%zu/%zu files, %zu conflicts)%s",
                                      pi + 1,
                                      pk.source == vfs::PackSource::Workshop ? "Workshop" : "Local",
                                      pk.displayName.c_str(),
                                      pk.indexedFiles,
                                      pk.totalFiles,
                                      pk.conflictCount,
                                      pk.enabled ? "" : " [Disabled]");
                }
            }
        }
    }

    // Unified Ring-Buffer Log Viewer
    if (ImGui::CollapsingHeader("Runtime Log (grml.log)")) {
        const auto entries = logger::snapshotRecent(120);
        ImGui::BeginChild("##grml_log_scroll", ImVec2(0.f, 150.f), true);
        for (const auto& e : entries) {
            ImVec4 col(0.85f, 0.83f, 0.76f, 1.f);
            if (e.level == GRML_LOG_WARN) col = ImVec4(0.95f, 0.76f, 0.30f, 1.f);
            else if (e.level == GRML_LOG_ERROR) col = ImVec4(0.95f, 0.38f, 0.35f, 1.f);
            ImGui::TextColored(col, "[%s] [%s] %s", e.timestamp, e.component.c_str(), e.message.c_str());
        }
        ImGui::EndChild();
    }
}

void renderDeclarativeIniTab(RegisteredTab& tab)
{
    ImGui::PushID(tab.pluginId.c_str());
    if (tab.autoGeneratedIni) {
        ImGui::TextDisabled("Auto-built from %s (changes apply after restart; Ctrl+Click sliders to type custom numbers)",
                            wideToUtf8(tab.iniPath.filename().wstring()).c_str());
    }
    for (size_t i = 0; i < tab.iniFields.size(); ++i) {
        auto& f = tab.iniFields[i];
        ImGui::PushID(static_cast<int>(i));
        switch (f.type) {
        case UiControlType::Header:
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.91f, 0.78f, 0.45f, 1.f), "%s", f.label.c_str());
            break;
        case UiControlType::Bool: {
            bool b = (f.boolVal != 0);
            if (ImGui::Checkbox(f.label.c_str(), &b)) {
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
        if (!f.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", f.tooltip.c_str());
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::Button("Apply & Save")) {
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

    if (!g_visible.load(std::memory_order_relaxed)) {
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

    ImGui::SetNextWindowSize(ImVec2(660.f, 440.f), ImGuiCond_FirstUseEver);
    bool open = true;
    char titleBuf[64]{};
    const uint16_t curVk = g_toggleVk.load(std::memory_order_relaxed);
    if (curVk >= VK_F1 && curVk <= VK_F12) {
        std::snprintf(titleBuf, sizeof(titleBuf), "GRML Mod Settings [F%u]###GRML_ModSettings",
                      static_cast<unsigned>(curVk - VK_F1 + 1));
    } else if (curVk == VK_INSERT) {
        std::snprintf(titleBuf, sizeof(titleBuf), "GRML Mod Settings [Insert]###GRML_ModSettings");
    } else if (curVk == VK_HOME) {
        std::snprintf(titleBuf, sizeof(titleBuf), "GRML Mod Settings [Home]###GRML_ModSettings");
    } else if (curVk == VK_END) {
        std::snprintf(titleBuf, sizeof(titleBuf), "GRML Mod Settings [End]###GRML_ModSettings");
    } else if (curVk == VK_PRIOR) {
        std::snprintf(titleBuf, sizeof(titleBuf), "GRML Mod Settings [PageUp]###GRML_ModSettings");
    } else if (curVk == VK_NEXT) {
        std::snprintf(titleBuf, sizeof(titleBuf), "GRML Mod Settings [PageDown]###GRML_ModSettings");
    } else if (curVk == VK_PAUSE) {
        std::snprintf(titleBuf, sizeof(titleBuf), "GRML Mod Settings [Pause]###GRML_ModSettings");
    } else if (curVk == VK_OEM_3) {
        std::snprintf(titleBuf, sizeof(titleBuf), "GRML Mod Settings [~]###GRML_ModSettings");
    } else if ((curVk >= 'A' && curVk <= 'Z') || (curVk >= '0' && curVk <= '9')) {
        std::snprintf(titleBuf, sizeof(titleBuf), "GRML Mod Settings [%c]###GRML_ModSettings", static_cast<char>(curVk));
    } else {
        std::snprintf(titleBuf, sizeof(titleBuf), "GRML Mod Settings [F9]###GRML_ModSettings");
    }
    const bool windowOpen = ImGui::Begin(titleBuf, &open, ImGuiWindowFlags_NoCollapse);
    {
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
        if (ImGui::BeginTabBar("##GRML_ModSettings_Tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
            if (ImGui::BeginTabItem("Loaded Mods###grml_loaded_mods")) {
                ImGui::Spacing();
                renderBuiltInLoadedModsTab();
                ImGui::EndTabItem();
            }

            std::lock_guard lock(g_tabsMutex);
            size_t hiddenInGameCount = 0;
            for (auto& tab : g_tabs) {
                if (!gameRunning && tab.mainMenuAvailable == 0) {
                    ++hiddenInGameCount;
                    continue;
                }
                const std::string tabLabel = tab.title + "###grml_tab_" + tab.pluginId + "_" + std::to_string(tab.tabId);
                if (ImGui::BeginTabItem(tabLabel.c_str())) {
                    ImGui::Spacing();
                    if (tab.uiFaulted) {
                        ImGui::TextColored(ImVec4(0.95f, 0.38f, 0.35f, 1.f),
                                           "[UI Faulted - Disabled] Exception 0x%08lX caught inside plugin tab '%s'.",
                                           tab.faultCode, tab.title.c_str());
                    } else if (tab.kind == TabKind::CustomGrml) {
                        ImGui::PushID(tab.pluginId.c_str());
                        const bool childOk = ImGui::BeginChild("##grml_custom_sandbox", ImVec2(0.f, 0.f), false);
                        if (childOk) {
                            g_activeDisabledDepth = 0;
                            DWORD excCode = 0;
                            if (!invokeRenderTabSeh(tab.renderCb, &g_grmlUiApi, tab.userData, &excCode)) {
                                tab.uiFaulted = true;
                                tab.faultCode = excCode;
                                logger::logMessage(GRML_LOG_ERROR, "OverlayHub",
                                                   "Plugin tab '%s' (%s) faulted with exception 0x%08lX — marked [UI Faulted - Disabled]",
                                                   tab.title.c_str(), tab.pluginId.c_str(), excCode);
                            }
                            while (g_activeDisabledDepth > 0) {
                                ImGui::EndDisabled();
                                --g_activeDisabledDepth;
                            }
                        }
                        ImGui::EndChild();
                        ImGui::PopID();
                    } else if (tab.kind == TabKind::DeclarativeIni) {
                        ImGui::PushID(tab.pluginId.c_str());
                        if (ImGui::BeginChild("##grml_ini_sandbox", ImVec2(0.f, 0.f), false)) {
                            renderDeclarativeIniTab(tab);
                        }
                        ImGui::EndChild();
                        ImGui::PopID();
                    } else if (tab.kind == TabKind::LegacyRetained) {
                        ImGui::PushID(tab.pluginId.c_str());
                        if (ImGui::BeginChild("##grml_legacy_sandbox", ImVec2(0.f, 0.f), false)) {
                            if (tab.legacyFrame) {
                                DWORD excCode = 0;
                                if (!invokeLegacyButtonSeh(tab.legacyFrame, tab.legacyFrameUserData, &excCode)) {
                                    tab.uiFaulted = true;
                                    tab.faultCode = excCode;
                                    logger::logMessage(GRML_LOG_ERROR, "OverlayHub",
                                                       "Legacy tab frame callback '%s' faulted (0x%08lX) — marked [UI Faulted - Disabled]",
                                                       tab.title.c_str(), excCode);
                                }
                            }
                            if (!tab.uiFaulted) {
                                if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && tab.legacySave) {
                                    DWORD excCode = 0;
                                    invokeLegacyButtonSeh(tab.legacySave, tab.legacyUserData, &excCode);
                                }
                                for (size_t i = 0; i < tab.legacyWidgets.size(); ++i) {
                                    renderLegacyWidget(tab.legacyWidgets[i], i);
                                }
                            }
                        }
                        ImGui::EndChild();
                        ImGui::PopID();
                    }
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();

            if (hiddenInGameCount > 0) {
                ImGui::Separator();
                ImGui::TextDisabled("%zu in-game mod config tab(s) hidden on Main Menu — load a republic to access them.",
                                    hiddenInGameCount);
            }
        }
    }
    ImGui::End();

    const bool mouseCaptured =
        io.WantCaptureMouse ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow |
                               ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
        ImGui::IsAnyItemActive();
    g_wantCaptureMouse.store(mouseCaptured, std::memory_order_relaxed);
    g_wantCaptureKeyboard.store(io.WantTextInput || io.WantCaptureKeyboard, std::memory_order_relaxed);
    io.MouseDrawCursor = isMouseLockedForGame();

    if (!open) toggleOverlay(false);

    ImGui::Render();
    std::lock_guard lock(g_d3dMutex);
    if (g_rtv && g_ctx) {
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
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
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX11_Init(g_device, g_ctx);
        createRenderTarget(sc);
        logger::logMessage(GRML_LOG_INFO, "OverlayHub", "DX11 + ImGui F9 Mod Settings hub initialized");
    }
    renderUI();
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
