# LibWM — the Wayland <-> LibGUI compatibility layer.
#
# This file is included from the Lagom build (see patches/0001-*.patch) so it can
# reuse the host include paths and the already-built SerenityOS libraries.

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(WM_WAYLAND_DIR "${CMAKE_CURRENT_BINARY_DIR}/wayland")
file(MAKE_DIRECTORY "${WM_WAYLAND_DIR}")
set(XDG_SHELL_XML "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
add_custom_command(
    OUTPUT
        "${WM_WAYLAND_DIR}/xdg-shell-client-protocol.h"
        "${WM_WAYLAND_DIR}/xdg-shell-protocol.c"
    COMMAND "${WAYLAND_SCANNER}" client-header "${XDG_SHELL_XML}" "${WM_WAYLAND_DIR}/xdg-shell-client-protocol.h"
    COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_SHELL_XML}" "${WM_WAYLAND_DIR}/xdg-shell-protocol.c"
    DEPENDS "${XDG_SHELL_XML}"
    VERBATIM
)

set(XDG_DECORATION_XML "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml")
add_custom_command(
    OUTPUT
        "${WM_WAYLAND_DIR}/xdg-decoration-client-protocol.h"
        "${WM_WAYLAND_DIR}/xdg-decoration-protocol.c"
    COMMAND "${WAYLAND_SCANNER}" client-header "${XDG_DECORATION_XML}" "${WM_WAYLAND_DIR}/xdg-decoration-client-protocol.h"
    COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_DECORATION_XML}" "${WM_WAYLAND_DIR}/xdg-decoration-protocol.c"
    DEPENDS "${XDG_DECORATION_XML}"
    VERBATIM
)

set(XDG_OUTPUT_XML "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml")
add_custom_command(
    OUTPUT
        "${WM_WAYLAND_DIR}/xdg-output-client-protocol.h"
        "${WM_WAYLAND_DIR}/xdg-output-protocol.c"
    COMMAND "${WAYLAND_SCANNER}" client-header "${XDG_OUTPUT_XML}" "${WM_WAYLAND_DIR}/xdg-output-client-protocol.h"
    COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_OUTPUT_XML}" "${WM_WAYLAND_DIR}/xdg-output-protocol.c"
    DEPENDS "${XDG_OUTPUT_XML}"
    VERBATIM
)

add_library(wm SHARED
    LibWM.cpp
    MenuController.cpp
    WaylandClient.cpp
    WindowServerConnection.cpp
    "${WM_WAYLAND_DIR}/xdg-shell-protocol.c"
    "${WM_WAYLAND_DIR}/xdg-decoration-protocol.c"
    "${WM_WAYLAND_DIR}/xdg-output-protocol.c"
)
target_include_directories(wm PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${WM_WAYLAND_DIR}"
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
)
target_compile_definitions(wm PRIVATE "LIBWM_DEFAULT_RES=\"${SERENITY_PROJECT_ROOT}/Base/res\"")
# The generated Wayland protocol code drops const in its listener trampolines.
target_compile_options(wm PRIVATE -Wno-cast-qual)
target_link_libraries(wm PRIVATE LibCore LibGfx LibIPC LibGUI LibThreading ${WAYLAND_CLIENT_LIBRARIES} ${XKBCOMMON_LIBRARIES})

# A minimal unmodified-style LibGUI application used to exercise the shim.
add_executable(libwm-test-window test/TestWindow.cpp)
target_include_directories(libwm-test-window PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(libwm-test-window PRIVATE LibMain LibGUI LibGfx LibCore LibIPC wm)

# Reads the Wayland clipboard then publishes text, to exercise the clipboard bridge.
add_executable(libwm-clipboard-test test/ClipboardTest.cpp)
target_include_directories(libwm-clipboard-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_options(libwm-clipboard-test PRIVATE "-Wl,--no-as-needed")
target_link_libraries(libwm-clipboard-test PRIVATE LibMain LibGUI LibGfx LibCore LibIPC wm)

# Shows the About dialog directly, to reproduce the About crash.
add_executable(libwm-about-test test/AboutTest.cpp)
target_include_directories(libwm-about-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_options(libwm-about-test PRIVATE "-Wl,--no-as-needed")
target_link_libraries(libwm-about-test PRIVATE LibMain LibGUI LibGfx LibCore LibIPC wm)

# Toggles fullscreen to verify xdg_toplevel fullscreen handling.
add_executable(libwm-fullscreen-test test/FullscreenTest.cpp)
target_include_directories(libwm-fullscreen-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_options(libwm-fullscreen-test PRIVATE "-Wl,--no-as-needed")
target_link_libraries(libwm-fullscreen-test PRIVATE LibMain LibGUI LibGfx LibCore LibIPC wm)

# --- Unmodified SerenityOS applications ---------------------------------------
# Built directly from their upstream sources. No source is edited; only the GML
# is precompiled (the app's own CMakeLists uses compile_gml, which requires a
# relative path, so we invoke the compiler directly here).

set(CALCULATOR_SRC "${SERENITY_PROJECT_ROOT}/Userland/Applications/Calculator")
set(CALCULATOR_GML "${CMAKE_CURRENT_BINARY_DIR}/CalculatorGML.cpp")
add_custom_command(
    OUTPUT "${CALCULATOR_GML}"
    COMMAND $<TARGET_FILE:Lagom::GMLCompiler> "${CALCULATOR_SRC}/CalculatorWindow.gml" > "${CALCULATOR_GML}.tmp"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CALCULATOR_GML}.tmp" "${CALCULATOR_GML}"
    COMMAND ${CMAKE_COMMAND} -E remove "${CALCULATOR_GML}.tmp"
    VERBATIM
    DEPENDS Lagom::GMLCompiler "${CALCULATOR_SRC}/CalculatorWindow.gml"
    MAIN_DEPENDENCY "${CALCULATOR_SRC}/CalculatorWindow.gml"
)
add_executable(Calculator
    "${CALCULATOR_SRC}/main.cpp"
    "${CALCULATOR_SRC}/Calculator.cpp"
    "${CALCULATOR_SRC}/CalculatorWidget.cpp"
    "${CALCULATOR_SRC}/Keypad.cpp"
    "${CALCULATOR_GML}"
)
target_include_directories(Calculator PRIVATE "${CALCULATOR_SRC}")
# LibWM registers its portals from a static initializer; keep the library loaded.
target_link_options(Calculator PRIVATE "-Wl,--no-as-needed")
target_link_libraries(Calculator PRIVATE LibMain LibGUI LibGfx LibCore LibIPC LibCrypto LibDesktop LibURL wm)
