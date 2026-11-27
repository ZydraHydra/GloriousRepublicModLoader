/* Minimal C11 translation unit verifying include/grml_api.h compiles as pure C */
#include "grml_api.h"

int grml_api_c_abi_smoke_check(void) {
    GrmlGameInfo info;
    info.abi_version = GRML_API_VERSION;
    info.struct_size = (uint32_t)sizeof(GrmlGameInfo);
    return (int)info.struct_size;
}
