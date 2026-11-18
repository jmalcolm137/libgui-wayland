# Helpers shared by the libgui-wayland CMake directories.

# Generate the client header and glue code for a Wayland protocol XML and add
# them to TARGET. The header is <name>-client-protocol.h in
# ${CMAKE_CURRENT_BINARY_DIR}/wayland, which TARGET should have on its include
# path.
function(libwm_wayland_protocol target name xml)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/wayland")
    set(header "${dir}/${name}-client-protocol.h")
    set(code "${dir}/${name}-protocol.c")
    add_custom_command(
        OUTPUT "${header}" "${code}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${dir}"
        COMMAND "${WAYLAND_SCANNER}" client-header "${xml}" "${header}"
        COMMAND "${WAYLAND_SCANNER}" private-code "${xml}" "${code}"
        DEPENDS "${xml}"
        VERBATIM
    )
    target_sources(${target} PRIVATE "${header}" "${code}")
endfunction()

# An executable that uses LibGUI through the shim:
#
#   libwm_gui_executable(name SOURCES ... [LIBS ...] [INCLUDES ...])
#
# LibWM installs its portals from a static initializer and nothing references
# its symbols directly, so it is linked with --no-as-needed to keep it loaded.
function(libwm_gui_executable name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;INCLUDES" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    if (ARG_INCLUDES)
        target_include_directories(${name} PRIVATE ${ARG_INCLUDES})
    endif()
    target_link_options(${name} PRIVATE "-Wl,--no-as-needed")
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} LibWM)
endfunction()

# Generate <Base>DefaultStub.h, a base class giving every method of an IPC
# endpoint's Stub a harmless default, so a portal server only overrides what it
# implements (see tools/gen-default-stub.py). It is regenerated whenever the
# endpoint's .ipc changes, so it cannot drift from the protocol.
#
#   libwm_default_stub(<Base> <service dir> [EXTRA_INCLUDES ...])
#
# e.g. libwm_default_stub(ClipboardServer Clipboard). Outputs go to
# ${LIBWM_STUB_DIR} and are collected in the LIBWM_STUB_OUTPUTS variable.
function(libwm_default_stub base service_dir)
    cmake_parse_arguments(ARG "" "" "EXTRA_INCLUDES" ${ARGN})
    set(endpoint_header "${CMAKE_BINARY_DIR}/Userland/Services/${service_dir}/${base}Endpoint.h")
    set(output "${LIBWM_STUB_DIR}/${base}DefaultStub.h")
    set(extra_args "")
    foreach(include IN LISTS ARG_EXTRA_INCLUDES)
        list(APPEND extra_args --extra-include "${include}")
    endforeach()
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${LIBWM_STUB_DIR}"
        COMMAND "${Python3_EXECUTABLE}" "${LIBGUI_WAYLAND_ROOT}/tools/gen-default-stub.py"
            "${endpoint_header}" "${output}" --base "${base}" --include-dir "${service_dir}" ${extra_args}
        DEPENDS "${LIBGUI_WAYLAND_ROOT}/tools/gen-default-stub.py" "${endpoint_header}" "generate_${base}Endpoint.h"
        VERBATIM
    )
    set(LIBWM_STUB_OUTPUTS ${LIBWM_STUB_OUTPUTS} "${output}" PARENT_SCOPE)
endfunction()
