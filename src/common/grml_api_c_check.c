/* Minimal C11 translation unit verifying include/grml_api.h compiles as pure C */
#include "grml_api.h"

int grml_api_c_abi_smoke_check(void) {
    GrmlGameInfo info;
    GrmlWrsrOffsets offsets;
    GrmlHookApi hooks;
    GrmlWrsrApi wrsr;
    GrmlHostApi host;
    GrmlUiApi ui;
    info.abi_version = GRML_API_VERSION;
    info.struct_size = (uint32_t)sizeof(GrmlGameInfo);
    offsets.abi_version = GRML_API_VERSION;
    offsets.struct_size = (uint32_t)sizeof(GrmlWrsrOffsets);
    hooks.abi_version = GRML_API_VERSION;
    hooks.struct_size = (uint32_t)sizeof(GrmlHookApi);
    wrsr.abi_version = GRML_API_VERSION;
    wrsr.struct_size = (uint32_t)sizeof(GrmlWrsrApi);
    host.abi_version = GRML_API_VERSION;
    host.struct_size = (uint32_t)sizeof(GrmlHostApi);
    ui.abi_version = GRML_API_VERSION;
    ui.struct_size = (uint32_t)sizeof(GrmlUiApi);
    return (int)(info.struct_size + offsets.struct_size + hooks.struct_size +
                 wrsr.struct_size + host.struct_size + ui.struct_size);
}
