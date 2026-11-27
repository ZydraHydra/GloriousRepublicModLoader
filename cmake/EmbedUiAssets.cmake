function(grml_embed_ui_assets target_name dist_dir)
    if(NOT EXISTS "${dist_dir}/index.html")
        message(FATAL_ERROR "grml_embed_ui_assets: dist/index.html not found in ${dist_dir}. Run 'npm run build' in src/launcher/ui first.")
    endif()

    file(GLOB_RECURSE _files RELATIVE "${dist_dir}" "${dist_dir}/*")
    list(SORT _files)

    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${_gen_dir}")
    set(_out_hpp "${_gen_dir}/embedded_ui_assets.hpp")
    set(_out_cpp "${_gen_dir}/embedded_ui_assets.cpp")

    set(_cpp_lines
"// Auto-generated from ${dist_dir} — do not edit manually.
#include \"embedded_ui_assets.hpp\"
#include <array>

namespace grml::generated {
namespace {
")
    set(_table_entries "")
    set(_idx 0)

    foreach(_rel IN LISTS _files)
        set(_full "${dist_dir}/${_rel}")
        if(IS_DIRECTORY "${_full}")
            continue()
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_full}")

        file(READ "${_full}" _hex HEX)
        string(LENGTH "${_hex}" _hex_len)
        math(EXPR _byte_len "${_hex_len} / 2")

        # Convert hex string "aabbcc" -> "0xaa,0xbb,0xcc,"
        string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," _bytes_c "${_hex}")
        if(_byte_len EQUAL 0)
            set(_bytes_c "0x00")
            set(_actual_len 0)
        else()
            set(_actual_len ${_byte_len})
        endif()

        get_filename_component(_ext "${_rel}" LAST_EXT)
        string(TOLOWER "${_ext}" _ext_lo)
        set(_mime "application/octet-stream")
        if(_ext_lo STREQUAL ".html")
            set(_mime "text/html; charset=utf-8")
        elseif(_ext_lo STREQUAL ".js" OR _ext_lo STREQUAL ".mjs")
            set(_mime "application/javascript; charset=utf-8")
        elseif(_ext_lo STREQUAL ".css")
            set(_mime "text/css; charset=utf-8")
        elseif(_ext_lo STREQUAL ".json")
            set(_mime "application/json; charset=utf-8")
        elseif(_ext_lo STREQUAL ".png")
            set(_mime "image/png")
        elseif(_ext_lo STREQUAL ".ico")
            set(_mime "image/x-icon")
        elseif(_ext_lo STREQUAL ".svg")
            set(_mime "image/svg+xml")
        elseif(_ext_lo STREQUAL ".woff2")
            set(_mime "font/woff2")
        elseif(_ext_lo STREQUAL ".ogg")
            set(_mime "audio/ogg")
        endif()

        string(APPEND _cpp_lines "constexpr uint8_t kAsset_${_idx}[] = { ${_bytes_c} };\n")
        string(APPEND _table_entries "    { \"/${_rel}\", \"${_mime}\", kAsset_${_idx}, ${_actual_len} },\n")
        math(EXPR _idx "${_idx} + 1")
    endforeach()

    string(APPEND _cpp_lines
"} // namespace

std::span<const EmbeddedUiAsset> embeddedUiAssets() noexcept
{
    static constexpr EmbeddedUiAsset kTable[] = {
${_table_entries}    };
    return kTable;
}

} // namespace grml::generated
")

    set(_hpp_text
"#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace grml::generated {

struct EmbeddedUiAsset {
    const char*    urlPath;
    const char*    mimeType;
    const uint8_t* data;
    size_t         size;
};

std::span<const EmbeddedUiAsset> embeddedUiAssets() noexcept;

} // namespace grml::generated
")

    file(WRITE "${_out_hpp}.tmp" "${_hpp_text}")
    file(WRITE "${_out_cpp}.tmp" "${_cpp_lines}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out_hpp}.tmp" "${_out_hpp}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out_cpp}.tmp" "${_out_cpp}")
    file(REMOVE "${_out_hpp}.tmp" "${_out_cpp}.tmp")

    target_sources(${target_name} PRIVATE "${_out_cpp}")
    target_include_directories(${target_name} PRIVATE "${_gen_dir}")
endfunction()
