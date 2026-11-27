#ifndef GRML_API_H
#define GRML_API_H

/* ============================================================================
 * grml_api.h — Glorious Republic Mod Loader (GRML) Pure-C Plugin ABI (v1)
 * ============================================================================
 * Canonical ABI header owned by GloriousRepublicModLoader (include/grml_api.h).
 * Downstream mod repositories mirror this file byte-for-byte.
 *
 * STRICT ABI RULES (/MT Static CRT Boundary Safety):
 *   1. Pure C only (extern "C"). Zero C++ STL types, templates, or exceptions.
 *   2. Zero ImGuiContext* pointers or cross-DLL heap allocations/frees.
 *   3. Every ABI struct starts with `abi_version` and `struct_size`.
 *   4. Append-only evolution: never reorder or remove existing fields.
 * ============================================================================ */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRML_API_VERSION 1u

#if defined(_WIN32)
  #define GRML_CALL __cdecl
  #ifdef __cplusplus
    #define GRML_EXPORT extern "C" __declspec(dllexport)
  #else
    #define GRML_EXPORT __declspec(dllexport)
  #endif
#else
  #define GRML_CALL
  #define GRML_EXPORT
#endif

/* Log severity levels for GrmlHostApi::Log */
typedef enum GrmlLogLevel {
    GRML_LOG_TRACE = 0,
    GRML_LOG_INFO  = 1,
    GRML_LOG_WARN  = 2,
    GRML_LOG_ERROR = 3
} GrmlLogLevel;

/* Field value types for zero-code declarative .ini UI tabs */
typedef enum GrmlIniFieldType {
    GRML_INI_BOOL  = 0, /* uint8_t (0 or 1) */
    GRML_INI_INT   = 1, /* int32_t          */
    GRML_INI_FLOAT = 2  /* float            */
} GrmlIniFieldType;

typedef struct GrmlUiApi GrmlUiApi;
typedef struct GrmlHostApi GrmlHostApi;

/* Callback invoked each frame while a plugin's F9 Mod Settings tab is active.
 * Always runs inside GRML's PushID + BeginChild + SEH fault-isolation sandbox. */
typedef void (GRML_CALL* GrmlRenderTabFn)(const GrmlUiApi* ui, void* user_data);

/* Callback invoked on centralized save lifecycle events (buildings.bin load /
 * header.bin write / return to main menu). `save_dir` is wide UTF-16 or NULL
 * on main-menu unload. */
typedef void (GRML_CALL* GrmlSaveEventFn)(const wchar_t* save_dir, void* user_data);

/* Optional callback invoked once right after GRML finishes its single-pass
 * .text AOB scan across SOVIET64.exe (all RegisterPatternScan targets ready). */
typedef void (GRML_CALL* GrmlPostScanFn)(void* user_data);

/* Optional callback invoked after declarative .ini settings are loaded/applied. */
typedef void (GRML_CALL* GrmlIniAppliedFn)(void* user_data);

/* Optional exported entrypoint queried by GRML after LoadLibraryW:
 *   extern "C" __declspec(dllexport) void __cdecl GrmlPluginInit(const GrmlHostApi* host);
 */
typedef void (GRML_CALL* GrmlPluginInitFn)(const GrmlHostApi* host);

typedef struct GrmlIniFieldDesc {
    uint32_t         abi_version;   /* Set to GRML_API_VERSION                    */
    uint32_t         struct_size;   /* Set to sizeof(GrmlIniFieldDesc)            */
    const char*      section;       /* INI [section] name                         */
    const char*      key;           /* INI key name                               */
    const char*      label;         /* Display label in F9 Mod Settings           */
    const char*      tooltip;       /* Optional hover tooltip (NULL for none)     */
    GrmlIniFieldType type;          /* GRML_INI_BOOL / INT / FLOAT                */
    double           min_value;     /* Minimum bound (INT / FLOAT)                */
    double           max_value;     /* Maximum bound (INT / FLOAT)                */
    double           default_value; /* Fallback value if key absent from .ini     */
    const char*      format;        /* Optional printf format (e.g. "%.2fx")      */
    void*            live_ptr;      /* Optional uint8_t* / int32_t* / float*      */
} GrmlIniFieldDesc;

typedef struct GrmlDeclarativeIniTabDesc {
    uint32_t                abi_version;         /* Set to GRML_API_VERSION                */
    uint32_t                struct_size;         /* Set to sizeof(GrmlDeclarativeIniTabDesc) */
    const char*             plugin_id;           /* Stable reverse-DNS plugin ID           */
    const char*             tab_title;           /* Tab label in F9 Mod Settings           */
    const char*             ini_filename;        /* UTF-8 filename/path (NULL -> auto)     */
    uint8_t                 main_menu_available; /* Non-zero shows tab on main menu        */
    const GrmlIniFieldDesc* fields;              /* Array of field descriptors             */
    uint32_t                field_count;         /* Number of entries in `fields`          */
    GrmlIniAppliedFn        on_applied;          /* Optional callback after load/apply     */
    void*                   user_data;           /* Opaque context passed to `on_applied`  */
} GrmlDeclarativeIniTabDesc;

typedef struct GrmlGameInfo {
    uint32_t    abi_version;     /* Set to GRML_API_VERSION */
    uint32_t    struct_size;     /* Set to sizeof(GrmlGameInfo) */
    uintptr_t   image_base;      /* SOVIET64.exe base address */
    uint32_t    image_size;      /* SizeOfImage */
    uint32_t    pe_timestamp;    /* PE FileHeader.TimeDateStamp */
    const char* game_version;    /* Detected WRSR version string (e.g. "1.2.0.9") */
    const char* sha256;          /* Lowercase hex SHA-256 of SOVIET64.exe */
    const char* loader_version;  /* GRML runtime version string */
} GrmlGameInfo;

/* ============================================================================
 * GrmlUiApi (v1) — Unified F9 Mod Settings & Telemetry Interface
 * ============================================================================ */
struct GrmlUiApi {
    uint32_t abi_version;
    uint32_t struct_size;

    /* Registers a custom immediate-mode Mod Settings tab. Returns tab_id >= 0, or -1. */
    int32_t (GRML_CALL* RegisterTab)(
        const char*     plugin_id,
        const char*     tab_title,
        GrmlRenderTabFn render_cb,
        void*           user_data);

    /* Zero-code declarative .ini tab auto-rendered & persisted by GRML. */
    int32_t (GRML_CALL* RegisterDeclarativeIniTab)(
        const GrmlDeclarativeIniTabDesc* desc);

    /* Controls whether a registered tab appears on the main menu (1) or in-game only (0). */
    void (GRML_CALL* SetTabMainMenuAvailable)(int32_t tab_id, uint8_t available);

    /* Safe primitive widgets (valid ONLY inside GrmlRenderTabFn). */
    int32_t (GRML_CALL* Checkbox)(const char* label, uint8_t* value);
    int32_t (GRML_CALL* SliderFloat)(const char* label, float* value, float min_val, float max_val, const char* fmt);
    int32_t (GRML_CALL* SliderInt)(const char* label, int32_t* value, int32_t min_val, int32_t max_val, const char* fmt);
    int32_t (GRML_CALL* Button)(const char* label);
    void    (GRML_CALL* Text)(const char* fmt, ...);
    void    (GRML_CALL* TextColored)(uint32_t rgba, const char* fmt, ...); /* 0xRRGGBBAA */
    void    (GRML_CALL* TextWrapped)(const char* fmt, ...);
    void    (GRML_CALL* Separator)(void);
    void    (GRML_CALL* SameLine)(void);
    void    (GRML_CALL* BeginDisabled)(uint8_t disabled);
    void    (GRML_CALL* EndDisabled)(void);
    int32_t (GRML_CALL* CollapsingHeader)(const char* label, uint8_t default_open);
    void    (GRML_CALL* SetItemTooltip)(const char* fmt, ...);

    /* Reports per-frame hook timing/call counts (compatible with WRSR_PROFILE_HOOK). */
    void (GRML_CALL* ReportHookTiming)(
        const char* plugin_id,
        const char* hook_name,
        uint64_t    calls_delta,
        double      elapsed_ms);
};

/* ============================================================================
 * GrmlHostApi (v1) — Centralized Loader Services Passed to GrmlPluginInit
 * ============================================================================ */
struct GrmlHostApi {
    uint32_t abi_version;
    uint32_t struct_size;

    const GrmlUiApi* ui;

    /* Queues an AOB pattern ("48 8B ?? ??") for GRML's single-pass .text scan
     * over SOVIET64.exe before gameplay hooks activate. Returns 1 if queued. */
    int32_t (GRML_CALL* RegisterPatternScan)(const char* pattern, void** out_addr);

    /* Optional callback invoked once immediately after the single-pass AOB scan. */
    int32_t (GRML_CALL* RegisterPostScanCallback)(GrmlPostScanFn cb, void* user_data);

    /* Registers callbacks on GRML's centralized header.bin/buildings.bin save hook. */
    int32_t (GRML_CALL* RegisterSaveEvents)(
        GrmlSaveEventFn on_save_load,
        GrmlSaveEventFn on_save_unload,
        void*           user_data);

    /* Optional callback specifically when WRSR writes header.bin (save/autosave). */
    int32_t (GRML_CALL* RegisterSaveWriteEvent)(
        GrmlSaveEventFn on_save_write,
        void*           user_data);

    /* Writes a formatted message into GRML's unified ring-buffer + grml.log. */
    void (GRML_CALL* Log)(int32_t level, const char* plugin_id, const char* fmt, ...);

    /* Populates GrmlGameInfo with SOVIET64.exe metadata. Returns 1 on success. */
    int32_t (GRML_CALL* GetGameInfo)(GrmlGameInfo* out_info);
};

#ifdef __cplusplus
}
#endif

#endif /* GRML_API_H */
