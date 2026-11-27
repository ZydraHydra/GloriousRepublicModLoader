function(grml_embed_offsets target_name ini_path)
    cmake_parse_arguments(EMBED "" "HEADER_NAME;SYMBOL_NAME" "" ${ARGN})
    if(NOT EMBED_HEADER_NAME)
        set(EMBED_HEADER_NAME "embedded_grml_offsets.hpp")
    endif()
    if(NOT EMBED_SYMBOL_NAME)
        set(EMBED_SYMBOL_NAME "kEmbeddedGrmlOffsetsIni")
    endif()

    if(NOT EXISTS "${ini_path}")
        message(FATAL_ERROR "grml_embed_offsets: offsets file not found: ${ini_path}")
    endif()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ini_path}")
    file(READ "${ini_path}" _offsets_raw)

    set(_delim "GRMLOFFSETS")
    string(FIND "${_offsets_raw}" ")${_delim}\"" _has_collision)
    if(NOT _has_collision EQUAL -1)
        message(FATAL_ERROR "grml_embed_offsets: ${ini_path} contains forbidden delimiter ')${_delim}\"'")
    endif()

    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(_out_hdr "${_gen_dir}/${EMBED_HEADER_NAME}")
    file(MAKE_DIRECTORY "${_gen_dir}")

    set(_hdr_text
"#pragma once
// Auto-generated from ${ini_path} — do not edit manually.
namespace grml::generated {
inline constexpr const char* ${EMBED_SYMBOL_NAME} = R\"${_delim}(
${_offsets_raw}
)${_delim}\";
} // namespace grml::generated
")
    file(WRITE "${_out_hdr}.tmp" "${_hdr_text}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out_hdr}.tmp" "${_out_hdr}"
        RESULT_VARIABLE _copy_rc
    )
    file(REMOVE "${_out_hdr}.tmp")
    if(NOT _copy_rc EQUAL 0)
        message(FATAL_ERROR "grml_embed_offsets: failed to generate ${_out_hdr}")
    endif()

    target_include_directories(${target_name} PRIVATE "${_gen_dir}")
endfunction()
