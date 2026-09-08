# pred_compile_shaders(TARGET <name> SHADER_ROOT <dir> OUTPUT_DIR <dir>)
#
# Compiles every vs_*.sc / fs_*.sc / cs_*.sc found under SHADER_ROOT with the
# bgfx shader compiler for each supported backend profile. Each shader's
# directory must contain a varying.def.sc. Output layout:
#
#   <OUTPUT_DIR>/Shaders/<profile>/<name>.bin
#
# where <profile> is one of: dx11, spirv.
function(pred_compile_shaders)
    cmake_parse_arguments(ARG "" "TARGET;SHADER_ROOT;OUTPUT_DIR" "" ${ARGN})
    foreach(required TARGET SHADER_ROOT OUTPUT_DIR)
        if(NOT ARG_${required})
            message(FATAL_ERROR "pred_compile_shaders: missing ${required}")
        endif()
    endforeach()

    # --- Candidate vcpkg installed trees (the toolchain may expose either variable name).
    set(_installed_roots)
    foreach(_var VCPKG_INSTALLED_DIR _VCPKG_INSTALLED_DIR)
        if(DEFINED ${_var})
            list(APPEND _installed_roots "${${_var}}")
        endif()
    endforeach()
    list(APPEND _installed_roots "${CMAKE_BINARY_DIR}/vcpkg_installed")
    list(REMOVE_DUPLICATES _installed_roots)

    # --- Locate shaderc (installed by the bgfx 'tools' feature for the host triplet).
    set(_tool_hints)
    foreach(_root IN LISTS _installed_roots)
        file(GLOB _tool_dirs LIST_DIRECTORIES true "${_root}/*/tools/bgfx")
        list(APPEND _tool_hints ${_tool_dirs})
    endforeach()
    find_program(PRED_SHADERC NAMES shaderc HINTS ${_tool_hints} DOC "bgfx shader compiler")
    if(NOT PRED_SHADERC)
        message(FATAL_ERROR
            "bgfx shaderc not found. Make sure vcpkg installed bgfx with the 'tools' feature "
            "for the host triplet. Searched: ${_tool_hints}")
    endif()

    # --- Locate bgfx_shader.sh (shader include shipped with bgfx).
    set(_include_hints)
    foreach(_root IN LISTS _installed_roots)
        file(GLOB _include_dirs LIST_DIRECTORIES true
            "${_root}/*/include/bgfx"
            "${_root}/*/include")
        list(APPEND _include_hints ${_include_dirs})
    endforeach()
    find_path(PRED_BGFX_SHADER_INCLUDE_DIR NAMES bgfx_shader.sh HINTS ${_include_hints}
        DOC "Directory containing bgfx_shader.sh")
    if(NOT PRED_BGFX_SHADER_INCLUDE_DIR)
        message(FATAL_ERROR "bgfx_shader.sh not found. Searched: ${_include_hints}")
    endif()

    # Three parallel lists, indexed together below. They are kept separate because a CMake list of
    # semicolon-joined strings flattens into one long list rather than a list of triples.
    set(_profile_dirs      dx11    spirv)   # output subdirectory, matches Renderer::ShaderProfileDir()
    set(_profile_platforms windows linux)   # shaderc --platform
    set(_profile_names     s_5_0   spirv)   # shaderc --profile

    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS "${ARG_SHADER_ROOT}/*.sc")
    set(_outputs)
    foreach(_src IN LISTS _sources)
        get_filename_component(_name "${_src}" NAME_WE)
        get_filename_component(_dir "${_src}" DIRECTORY)
        if(_name MATCHES "^vs_")
            set(_type vertex)
        elseif(_name MATCHES "^fs_")
            set(_type fragment)
        elseif(_name MATCHES "^cs_")
            set(_type compute)
        else()
            continue()
        endif()
        set(_varying "${_dir}/varying.def.sc")
        if(NOT EXISTS "${_varying}")
            message(FATAL_ERROR "Shader ${_src} has no varying.def.sc next to it")
        endif()
        list(LENGTH _profile_dirs _profile_count)
        math(EXPR _profile_last "${_profile_count} - 1")
        foreach(_i RANGE ${_profile_last})
            list(GET _profile_dirs ${_i} _profile_dir)
            list(GET _profile_platforms ${_i} _platform)
            list(GET _profile_names ${_i} _profile)
            set(_out_dir "${ARG_OUTPUT_DIR}/Shaders/${_profile_dir}")
            set(_out "${_out_dir}/${_name}.bin")
            add_custom_command(
                OUTPUT "${_out}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${_out_dir}"
                COMMAND "${PRED_SHADERC}"
                    -f "${_src}"
                    -o "${_out}"
                    --type ${_type}
                    --platform ${_platform}
                    --profile ${_profile}
                    -i "${PRED_BGFX_SHADER_INCLUDE_DIR}"
                    -i "${_dir}"
                    -i "${ARG_SHADER_ROOT}"
                    --varyingdef "${_varying}"
                DEPENDS "${_src}" "${_varying}" "${PRED_BGFX_SHADER_INCLUDE_DIR}/bgfx_shader.sh"
                COMMENT "shaderc [${_profile_dir}] ${_name}"
                VERBATIM)
            list(APPEND _outputs "${_out}")
        endforeach()
    endforeach()

    add_custom_target(${ARG_TARGET} ALL DEPENDS ${_outputs})
    set_target_properties(${ARG_TARGET} PROPERTIES FOLDER "Shaders")
    message(STATUS "Shaders: ${PRED_SHADERC} -> ${ARG_OUTPUT_DIR}/Shaders")
endfunction()
