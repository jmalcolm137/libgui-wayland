# LibWM — the Wayland <-> LibGUI compatibility layer.
#
# This directory is the libgui-wayland entry point of the Lagom build (see
# patches/lagom-and-resources/0001-*.patch), so it can reuse the host include paths and the
# already-built SerenityOS libraries. It builds the shim library itself, then
# adds the directories that depend on it:
#
#   src/test   LibWM's own test programs and the CTest registrations
#   services   upstream SerenityOS services LibWM spawns on demand
#
# The applications are not part of this build: the desktop that ships them
# defines their Lagom targets, builds them and tests them against its compositor.

set(LIBGUI_WAYLAND_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE INTERNAL "libgui-wayland source root")
include("${LIBGUI_WAYLAND_ROOT}/cmake/LibWMHelpers.cmake")

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
# Optional: the org.freedesktop.portal.Settings bridge (appearance preference).
# Without it LibWM simply has no desktop appearance to follow.
pkg_check_modules(SDBUS libsystemd)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

# Default stubs for the endpoints LibWM serves in-process, generated from the
# Lagom IPC headers at build time.
set(LIBWM_STUB_DIR "${CMAKE_CURRENT_BINARY_DIR}/stubs" CACHE INTERNAL "Generated LibWM default stubs")
set(LIBWM_STUB_OUTPUTS "")
libwm_default_stub(WindowServer WindowServer EXTRA_INCLUDES WindowServer/ScreenLayout.h)
libwm_default_stub(ClipboardServer Clipboard)
libwm_default_stub(ConfigServer ConfigServer)
libwm_default_stub(LaunchServer LaunchServer)
libwm_default_stub(FileSystemAccessServer FileSystemAccessServer)
add_custom_target(libwm_default_stubs DEPENDS ${LIBWM_STUB_OUTPUTS})

add_library(LibWM SHARED
    ClipboardServerConnection.cpp
    ConfigServerConnection.cpp
    DesktopSettings.cpp
    Environment.cpp
    FileSystemAccessServerConnection.cpp
    LaunchServerConnection.cpp
    LibWM.cpp
    MenuController.cpp
    ScreenshotPortal.cpp
    SerenityShell.cpp
    WaylandActivation.cpp
    WaylandClient.cpp
    WaylandDataDevice.cpp
    WaylandInputObservation.cpp
    WaylandOutputs.cpp
    WaylandPopup.cpp
    WaylandSeat.cpp
    WaylandSurface.cpp
    WindowServerConnection.cpp
    WindowServerSettings.cpp
    "${SERENITY_PROJECT_ROOT}/Userland/Services/ImageDecoder/ConnectionFromClient.cpp"
)

# The library is libwm.so. It used to be built as libwindowserver.so and its
# target was `windowserver` (short form `wm`). Consumers may still link against
# or build those names (`target_link_libraries(... windowserver)`, `ninja
# windowserver`), so they are real INTERFACE targets that link and build LibWM:
# an ALIAS would link but is not a buildable target for Ninja. A
# libwindowserver.so symlink keeps binaries linked against the old file name
# working; through the symlink the loader sees one file, so the library is never
# loaded twice.
set_target_properties(LibWM PROPERTIES OUTPUT_NAME wm)
foreach(compat_name windowserver wm)
    # The header source is what makes CMake give an INTERFACE library a build
    # target (it compiles nothing).
    add_library(${compat_name} INTERFACE LibWM.h)
    target_link_libraries(${compat_name} INTERFACE LibWM)
    add_dependencies(${compat_name} LibWM)
endforeach()
add_custom_command(TARGET LibWM POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E create_symlink "$<TARGET_FILE_NAME:LibWM>" "$<TARGET_FILE_DIR:LibWM>/libwindowserver.so"
    VERBATIM
)

# Wayland protocols the shim speaks.
libwm_wayland_protocol(LibWM xdg-shell "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
# wp_viewporter: stretch the last frame during an interactive resize, and set
# the logical destination size under fractional HiDPI.
libwm_wayland_protocol(LibWM viewporter "${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml")
# wp_fractional_scale_v1: per-surface fractional scale in 1/120ths.
libwm_wayland_protocol(LibWM fractional-scale-v1 "${WAYLAND_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml")
libwm_wayland_protocol(LibWM text-input-unstable-v3 "${WAYLAND_PROTOCOLS_DIR}/unstable/text-input/text-input-unstable-v3.xml")
libwm_wayland_protocol(LibWM xdg-decoration "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml")
libwm_wayland_protocol(LibWM xdg-output "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml")
# xdg_toplevel_icon_v1: the standard window icon (instead of serenity-window's).
libwm_wayland_protocol(LibWM xdg-toplevel-icon-v1 "${WAYLAND_PROTOCOLS_DIR}/staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml")
# wp_pointer_warp_v1: move the pointer to a location on one of our surfaces
# (Serenity's set_global_cursor_position, e.g. the Taskbar).
libwm_wayland_protocol(LibWM pointer-warp-v1 "${WAYLAND_PROTOCOLS_DIR}/staging/pointer-warp/pointer-warp-v1.xml")
# xdg-activation-v1: ask the compositor to raise/focus a window, using an
# activation token (the standard way a launcher raises a new window).
# xdg-activation is staged in the wayland-protocols this project pins, but it
# may move to stable; accept either location.
set(XDG_ACTIVATION_XML "${WAYLAND_PROTOCOLS_DIR}/staging/xdg-activation/xdg-activation-v1.xml")
if (NOT EXISTS "${XDG_ACTIVATION_XML}")
    set(XDG_ACTIVATION_XML "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-activation/xdg-activation-v1.xml")
endif()
libwm_wayland_protocol(LibWM xdg-activation-v1 "${XDG_ACTIVATION_XML}")
# ext-input-observation-v1: the pointer's position relative to one of our
# surfaces, even when it is outside it (Serenity's set_global_mouse_tracking).
# A draft (wayland-protocols MR !335) implemented by SerenityDE and wlmaker, so
# the XML is vendored.
libwm_wayland_protocol(LibWM ext-input-observation-v1 "${CMAKE_CURRENT_SOURCE_DIR}/protocol/ext-input-observation-v1.xml")
# zwp_primary_selection_v1: the middle-click selection shared with other toolkits.
libwm_wayland_protocol(LibWM primary-selection-unstable-v1 "${WAYLAND_PROTOCOLS_DIR}/unstable/primary-selection/primary-selection-unstable-v1.xml")
# wlr-layer-shell: the Serenity Taskbar/Desktop/applets map to layer surfaces.
libwm_wayland_protocol(LibWM wlr-layer-shell-unstable-v1 "${CMAKE_CURRENT_SOURCE_DIR}/protocol/wlr-layer-shell-unstable-v1.xml")
# serenity-window: compositor <-> shim chrome control (menubar, window menu,
# host settings, wallpaper, capture).
libwm_wayland_protocol(LibWM serenity-window "${CMAKE_CURRENT_SOURCE_DIR}/protocol/serenity-window.xml")

target_include_directories(LibWM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${LIBWM_STUB_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/wayland"
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
)
target_compile_definitions(LibWM PRIVATE "LIBWM_DEFAULT_RES=\"${SERENITY_PROJECT_ROOT}/Base/res\"")
if (SDBUS_FOUND)
    target_compile_definitions(LibWM PRIVATE LIBWM_HAVE_SDBUS=1)
    target_include_directories(LibWM PRIVATE ${SDBUS_INCLUDE_DIRS})
    target_link_libraries(LibWM PRIVATE ${SDBUS_LIBRARIES})
endif()
# The generated Wayland protocol code drops const in its listener trampolines.
target_compile_options(LibWM PRIVATE -Wno-cast-qual)
# A shared library may otherwise link with undefined symbols, which then only
# fail when an application loads it.
target_link_options(LibWM PRIVATE "-Wl,--no-undefined")
target_link_libraries(LibWM PRIVATE AK LibCore LibGfx LibIPC LibGUI LibLaunch LibThreading LibURL ${WAYLAND_CLIENT_LIBRARIES} ${XKBCOMMON_LIBRARIES})
add_dependencies(LibWM
    libwm_default_stubs
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
