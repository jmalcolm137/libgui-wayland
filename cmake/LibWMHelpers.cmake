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
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} windowserver)
endfunction()
