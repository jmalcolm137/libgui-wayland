# LibWM — the Wayland <-> LibGUI compatibility layer.
#
# This directory is the libgui-wayland entry point of the Lagom build (see
# patches/0001-*.patch), so it can reuse the host include paths and the
# already-built SerenityOS libraries. It builds the shim library itself, then
# adds the directories that depend on it:
#
#   src/test   LibWM's own test programs and the CTest registrations
#   services   upstream SerenityOS services LibWM spawns on demand
#   apps       unmodified upstream applications used by the integration tests

set(LIBGUI_WAYLAND_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE INTERNAL "libgui-wayland source root")
include("${LIBGUI_WAYLAND_ROOT}/cmake/LibWMHelpers.cmake")

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

add_library(windowserver SHARED
    ClipboardServerConnection.cpp
    ConfigServerConnection.cpp
    Environment.cpp
    FileSystemAccessServerConnection.cpp
    LaunchServerConnection.cpp
    LibWM.cpp
    MenuController.cpp
    WaylandClient.cpp
    WaylandDataDevice.cpp
    WaylandInput.cpp
    WaylandPopup.cpp
    WaylandSurface.cpp
    WindowServerConnection.cpp
    WindowServerSettings.cpp
    "${SERENITY_PROJECT_ROOT}/Userland/Services/ImageDecoder/ConnectionFromClient.cpp"
)

# The library's file name is historical (it was LibWindowServer); `wm` is the
# short alias older references use.
add_library(wm ALIAS windowserver)

# Wayland protocols the shim speaks.
libwm_wayland_protocol(windowserver xdg-shell "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
# wp_viewporter: stretch the last frame during an interactive resize, and set
# the logical destination size under fractional HiDPI.
libwm_wayland_protocol(windowserver viewporter "${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml")
# wp_fractional_scale_v1: per-surface fractional scale in 1/120ths.
libwm_wayland_protocol(windowserver fractional-scale-v1 "${WAYLAND_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml")
libwm_wayland_protocol(windowserver text-input-unstable-v3 "${WAYLAND_PROTOCOLS_DIR}/unstable/text-input/text-input-unstable-v3.xml")
libwm_wayland_protocol(windowserver xdg-decoration "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml")
libwm_wayland_protocol(windowserver xdg-output "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml")
# wlr-layer-shell: the Serenity Taskbar/Desktop/applets map to layer surfaces.
libwm_wayland_protocol(windowserver wlr-layer-shell-unstable-v1 "${CMAKE_CURRENT_SOURCE_DIR}/protocol/wlr-layer-shell-unstable-v1.xml")
# serenity-window: compositor <-> shim chrome control (menubar, window menu,
# host settings, wallpaper, capture).
libwm_wayland_protocol(windowserver serenity-window "${CMAKE_CURRENT_SOURCE_DIR}/protocol/serenity-window.xml")

target_include_directories(windowserver PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/wayland"
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
)
target_compile_definitions(windowserver PRIVATE "LIBWM_DEFAULT_RES=\"${SERENITY_PROJECT_ROOT}/Base/res\"")
# The generated Wayland protocol code drops const in its listener trampolines.
target_compile_options(windowserver PRIVATE -Wno-cast-qual)
target_link_libraries(windowserver PRIVATE LibCore LibGfx LibIPC LibGUI LibLaunch LibThreading LibURL ${WAYLAND_CLIENT_LIBRARIES} ${XKBCOMMON_LIBRARIES})
add_dependencies(windowserver
    generate_WindowServerEndpoint.h
    generate_WindowClientEndpoint.h
    generate_WindowManagerServerEndpoint.h
    generate_WindowManagerClientEndpoint.h
    generate_ClipboardClientEndpoint.h
    generate_ClipboardServerEndpoint.h
    generate_NotificationClientEndpoint.h
    generate_NotificationServerEndpoint.h
    generate_LaunchClientEndpoint.h
    generate_LaunchServerEndpoint.h
    generate_FileSystemAccessClientEndpoint.h
    generate_FileSystemAccessServerEndpoint.h
    generate_AudioClientEndpoint.h
    generate_AudioServerEndpoint.h
    generate_AudioManagerClientEndpoint.h
    generate_AudioManagerServerEndpoint.h
    generate_ConfigClientEndpoint.h
    generate_ConfigServerEndpoint.h
    generate_ImageDecoderClientEndpoint.h
    generate_ImageDecoderServerEndpoint.h
)

# Lagom calls enable_testing() only after including this directory, which would
# leave src/test without a CTestTestfile; enable it here so its suites register.
enable_testing()
add_subdirectory(test)
add_subdirectory("${LIBGUI_WAYLAND_ROOT}/services" "${CMAKE_CURRENT_BINARY_DIR}/services")
add_subdirectory("${LIBGUI_WAYLAND_ROOT}/apps" "${CMAKE_CURRENT_BINARY_DIR}/apps")
