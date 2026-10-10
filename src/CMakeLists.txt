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

# wp_viewporter (stretch the last frame during an interactive resize).
set(VIEWPORTER_XML "${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml")
add_custom_command(
    OUTPUT
        "${WM_WAYLAND_DIR}/viewporter-client-protocol.h"
        "${WM_WAYLAND_DIR}/viewporter-protocol.c"
    COMMAND "${WAYLAND_SCANNER}" client-header "${VIEWPORTER_XML}" "${WM_WAYLAND_DIR}/viewporter-client-protocol.h"
    COMMAND "${WAYLAND_SCANNER}" private-code "${VIEWPORTER_XML}" "${WM_WAYLAND_DIR}/viewporter-protocol.c"
    DEPENDS "${VIEWPORTER_XML}"
    VERBATIM
)

# wlr-layer-shell: the Serenity Taskbar/Desktop/applets map to layer surfaces.
set(WLR_LAYER_SHELL_XML "${CMAKE_CURRENT_SOURCE_DIR}/protocol/wlr-layer-shell-unstable-v1.xml")
add_custom_command(
    OUTPUT
        "${WM_WAYLAND_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h"
        "${WM_WAYLAND_DIR}/wlr-layer-shell-unstable-v1-protocol.c"
    COMMAND "${WAYLAND_SCANNER}" client-header "${WLR_LAYER_SHELL_XML}" "${WM_WAYLAND_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h"
    COMMAND "${WAYLAND_SCANNER}" private-code "${WLR_LAYER_SHELL_XML}" "${WM_WAYLAND_DIR}/wlr-layer-shell-unstable-v1-protocol.c"
    DEPENDS "${WLR_LAYER_SHELL_XML}"
    VERBATIM
)

# serenity-window: compositor -> LibWindowServer chrome control (window menu).
set(SERENITY_WINDOW_XML "${CMAKE_CURRENT_SOURCE_DIR}/protocol/serenity-window.xml")
add_custom_command(
    OUTPUT
        "${WM_WAYLAND_DIR}/serenity-window-client-protocol.h"
        "${WM_WAYLAND_DIR}/serenity-window-protocol.c"
    COMMAND "${WAYLAND_SCANNER}" client-header "${SERENITY_WINDOW_XML}" "${WM_WAYLAND_DIR}/serenity-window-client-protocol.h"
    COMMAND "${WAYLAND_SCANNER}" private-code "${SERENITY_WINDOW_XML}" "${WM_WAYLAND_DIR}/serenity-window-protocol.c"
    DEPENDS "${SERENITY_WINDOW_XML}"
    VERBATIM
)

add_library(windowserver SHARED
    LibWM.cpp
    MenuController.cpp
    WaylandClient.cpp
    WindowServerConnection.cpp
    "${SERENITY_PROJECT_ROOT}/Userland/Services/ImageDecoder/ConnectionFromClient.cpp"
    "${WM_WAYLAND_DIR}/xdg-shell-protocol.c"
    "${WM_WAYLAND_DIR}/xdg-decoration-protocol.c"
    "${WM_WAYLAND_DIR}/xdg-output-protocol.c"
    "${WM_WAYLAND_DIR}/viewporter-protocol.c"
    "${WM_WAYLAND_DIR}/wlr-layer-shell-unstable-v1-protocol.c"
    "${WM_WAYLAND_DIR}/serenity-window-protocol.c"
)

# LibWM was renamed to LibWindowServer. Keep the old target name working for
# existing references (patches/scripts) during the transition.
add_library(wm ALIAS windowserver)

target_include_directories(windowserver PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${WM_WAYLAND_DIR}"
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
)
target_compile_definitions(windowserver PRIVATE "LIBWM_DEFAULT_RES=\"${SERENITY_PROJECT_ROOT}/Base/res\"")
# The generated Wayland protocol code drops const in its listener trampolines.
target_compile_options(windowserver PRIVATE -Wno-cast-qual)
target_link_libraries(windowserver PRIVATE LibCore LibGfx LibIPC LibGUI LibLaunch LibThreading LibURL ${WAYLAND_CLIENT_LIBRARIES} ${XKBCOMMON_LIBRARIES})

# A minimal unmodified-style LibGUI application used to exercise the shim.
add_executable(libwm-test-window test/TestWindow.cpp)
target_include_directories(libwm-test-window PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(libwm-test-window PRIVATE LibMain LibGUI LibGfx LibCore LibIPC windowserver)

# Reads the Wayland clipboard then publishes text, to exercise the clipboard bridge.
add_executable(libwm-clipboard-test test/ClipboardTest.cpp)
target_include_directories(libwm-clipboard-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_options(libwm-clipboard-test PRIVATE "-Wl,--no-as-needed")
target_link_libraries(libwm-clipboard-test PRIVATE LibMain LibGUI LibGfx LibCore LibIPC windowserver)

# Shows the About dialog directly, to reproduce the About crash.
add_executable(libwm-about-test test/AboutTest.cpp)
target_include_directories(libwm-about-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_options(libwm-about-test PRIVATE "-Wl,--no-as-needed")
target_link_libraries(libwm-about-test PRIVATE LibMain LibGUI LibGfx LibCore LibIPC windowserver)

# Toggles fullscreen to verify xdg_toplevel fullscreen handling.
add_executable(libwm-fullscreen-test test/FullscreenTest.cpp)
target_include_directories(libwm-fullscreen-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_options(libwm-fullscreen-test PRIVATE "-Wl,--no-as-needed")
target_link_libraries(libwm-fullscreen-test PRIVATE LibMain LibGUI LibGfx LibCore LibIPC windowserver)

# A menubar with a nested submenu, to verify submenu navigation.
add_executable(libwm-submenu-test test/SubmenuTest.cpp)
target_include_directories(libwm-submenu-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_options(libwm-submenu-test PRIVATE "-Wl,--no-as-needed")
target_link_libraries(libwm-submenu-test PRIVATE LibMain LibGUI LibGfx LibCore LibIPC windowserver)

# Probes the launch and image session-service portals, with and without an
# external session service (in-process fallback vs. deferral). Driven by
# scripts/run-services-test.sh; see docs/06-service-layering.md.
add_executable(libwm-services-test test/ServiceLayeringTest.cpp)
target_include_directories(libwm-services-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_options(libwm-services-test PRIVATE "-Wl,--no-as-needed")
target_link_libraries(libwm-services-test PRIVATE LibMain LibGUI LibGfx LibCore LibIPC LibURL LibDesktop LibLaunch LibImageDecoderClient windowserver)

# Spawns an app through Desktop::AppFile - the Taskbar system menu's launch path.
add_executable(libwm-appfile-test test/AppFileLaunchTest.cpp)
target_include_directories(libwm-appfile-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_options(libwm-appfile-test PRIVATE "-Wl,--no-as-needed")
target_link_libraries(libwm-appfile-test PRIVATE LibMain LibGUI LibGfx LibCore LibIPC LibDesktop windowserver)

# Shows a notification through LibGUI::Notification (spike §3: NotificationServer).
add_executable(libwm-notify-test test/NotificationTest.cpp)
target_include_directories(libwm-notify-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_options(libwm-notify-test PRIVATE "-Wl,--no-as-needed")
target_link_libraries(libwm-notify-test PRIVATE LibMain LibGUI LibGfx LibCore LibIPC windowserver)

# --- The out-of-process WebContent service ------------------------------------
# Built from the upstream sources. It is a single-client system service; LibWM
# spawns it on demand and hands it a socket via SOCKET_TAKEOVER (the same
# contract SystemServer uses), so no Serenity SystemServer is needed. Help,
# Browser and any other WebView app reach it through the webcontent portal.
set(WEBCONTENT_SRC "${SERENITY_PROJECT_ROOT}/Userland/Services/WebContent")
add_executable(WebContent
    "${WEBCONTENT_SRC}/ConnectionFromClient.cpp"
    "${WEBCONTENT_SRC}/ConsoleGlobalEnvironmentExtensions.cpp"
    "${WEBCONTENT_SRC}/ImageCodecPluginSerenity.cpp"
    "${WEBCONTENT_SRC}/PageClient.cpp"
    "${WEBCONTENT_SRC}/PageHost.cpp"
    "${WEBCONTENT_SRC}/WebContentConsoleClient.cpp"
    "${WEBCONTENT_SRC}/WebDriverConnection.cpp"
    "${WEBCONTENT_SRC}/main.cpp"
)
target_include_directories(WebContent PRIVATE "${SERENITY_PROJECT_ROOT}/Userland/Services")
target_link_options(WebContent PRIVATE "-Wl,--no-as-needed")
# WebContent is a client-stack process too: link the shim so its outgoing portal
# connects (RequestServer, ImageDecoder) go through LibWM and are spawned/served
# on demand, exactly as an application's are.
target_link_libraries(WebContent PRIVATE LibCore LibFileSystem LibIPC LibGfx LibAudio LibImageDecoderClient LibJS LibWebView LibWeb LibLocale LibMain LibURL windowserver)

# --- The out-of-process RequestServer service ---------------------------------
# WebContent (and the browser) require it: the HTTP/TLS backend for page loads.
# Also a single-client service, spawned on demand the same way.
set(REQUESTSERVER_SRC "${SERENITY_PROJECT_ROOT}/Userland/Services/RequestServer")
add_executable(RequestServer
    "${REQUESTSERVER_SRC}/ConnectionFromClient.cpp"
    "${REQUESTSERVER_SRC}/ConnectionCache.cpp"
    "${REQUESTSERVER_SRC}/GeminiProtocol.cpp"
    "${REQUESTSERVER_SRC}/GeminiRequest.cpp"
    "${REQUESTSERVER_SRC}/HttpProtocol.cpp"
    "${REQUESTSERVER_SRC}/HttpRequest.cpp"
    "${REQUESTSERVER_SRC}/HttpsProtocol.cpp"
    "${REQUESTSERVER_SRC}/HttpsRequest.cpp"
    "${REQUESTSERVER_SRC}/Protocol.cpp"
    "${REQUESTSERVER_SRC}/Request.cpp"
    "${REQUESTSERVER_SRC}/main.cpp"
)
target_include_directories(RequestServer PRIVATE "${SERENITY_PROJECT_ROOT}/Userland/Services")
target_link_options(RequestServer PRIVATE "-Wl,--no-as-needed")
target_link_libraries(RequestServer PRIVATE LibCore LibCrypto LibIPC LibGemini LibHTTP LibMain LibTLS LibWebSocket LibURL LibThreading)

# --- The out-of-process SQLServer service -------------------------------------
# A multi-client service (IPC::MultiServer). LibWM binds its listening socket and
# hands that over, rather than spawning one per connection. Browser needs it for
# history/bookmarks/cookies.
set(SQLSERVER_SRC "${SERENITY_PROJECT_ROOT}/Userland/Services/SQLServer")
add_executable(SQLServer
    "${SQLSERVER_SRC}/ConnectionFromClient.cpp"
    "${SQLSERVER_SRC}/DatabaseConnection.cpp"
    "${SQLSERVER_SRC}/SQLStatement.cpp"
    "${SQLSERVER_SRC}/main.cpp"
)
target_include_directories(SQLServer PRIVATE "${SERENITY_PROJECT_ROOT}/Userland/Services")
target_link_options(SQLServer PRIVATE "-Wl,--no-as-needed")
target_link_libraries(SQLServer PRIVATE LibCore LibIPC LibSQL LibMain)

# --- The out-of-process NotificationServer service ----------------------------
# The unmodified SerenityOS service (spike outcome: reuse; docs/09 §3). Its IPC
# endpoints are already generated by the Lagom build; only the GML is not.
set(NOTIFICATION_SERVER_SRC "${SERENITY_PROJECT_ROOT}/Userland/Services/NotificationServer")

set(NOTIFICATION_WIDGET_GML "${CMAKE_CURRENT_BINARY_DIR}/NotificationWidgetGML.cpp")
add_custom_command(
    OUTPUT "${NOTIFICATION_WIDGET_GML}"
    COMMAND $<TARGET_FILE:Lagom::GMLCompiler> "${NOTIFICATION_SERVER_SRC}/NotificationWidget.gml" > "${NOTIFICATION_WIDGET_GML}.tmp"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NOTIFICATION_WIDGET_GML}.tmp" "${NOTIFICATION_WIDGET_GML}"
    COMMAND ${CMAKE_COMMAND} -E remove "${NOTIFICATION_WIDGET_GML}.tmp"
    VERBATIM
    DEPENDS Lagom::GMLCompiler "${NOTIFICATION_SERVER_SRC}/NotificationWidget.gml"
    MAIN_DEPENDENCY "${NOTIFICATION_SERVER_SRC}/NotificationWidget.gml"
)

add_executable(NotificationServer
    "${NOTIFICATION_SERVER_SRC}/ConnectionFromClient.cpp"
    "${NOTIFICATION_SERVER_SRC}/NotificationWindow.cpp"
    "${NOTIFICATION_SERVER_SRC}/main.cpp"
    "${NOTIFICATION_WIDGET_GML}"
)
target_include_directories(NotificationServer PRIVATE "${NOTIFICATION_SERVER_SRC}")
target_link_options(NotificationServer PRIVATE "-Wl,--no-as-needed")
target_link_libraries(NotificationServer PRIVATE LibCore LibDesktop LibGfx LibGUI LibIPC LibMain LibURL windowserver)

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
target_link_libraries(Calculator PRIVATE LibMain LibGUI LibGfx LibCore LibIPC LibCrypto LibDesktop LibURL windowserver)

set(PDFVIEWER_SRC "${SERENITY_PROJECT_ROOT}/Userland/Applications/PDFViewer")
add_executable(PDFViewer
    "${PDFVIEWER_SRC}/main.cpp"
    "${PDFVIEWER_SRC}/OutlineModel.cpp"
    "${PDFVIEWER_SRC}/PDFViewer.cpp"
    "${PDFVIEWER_SRC}/PDFViewerWidget.cpp"
    "${PDFVIEWER_SRC}/SidebarWidget.cpp"
    "${PDFVIEWER_SRC}/ThumbnailsListView.cpp"
    "${PDFVIEWER_SRC}/ThumbnailsModel.cpp"
)
target_include_directories(PDFViewer PRIVATE "${PDFVIEWER_SRC}")
target_link_options(PDFViewer PRIVATE "-Wl,--no-as-needed")
target_link_libraries(PDFViewer PRIVATE LibMain LibGUI LibGfx LibCore LibIPC LibPDF LibFileSystemAccessClient LibConfig LibURL windowserver)

# --- Unmodified Piano (audio through LibSerenityAudio -> PipeWire) ------------
set(PIANO_SRC "${SERENITY_PROJECT_ROOT}/Userland/Applications/Piano")
set(PIANO_GEN "${CMAKE_CURRENT_BINARY_DIR}/Applications/Piano/ExportProgressWidget.h")
add_custom_command(
    OUTPUT "${PIANO_GEN}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/Applications/Piano"
    COMMAND "${Python3_EXECUTABLE}" "${SERENITY_PROJECT_ROOT}/Meta/embed_as_string.py" --type=string-view "${PIANO_SRC}/ExportProgressWidget.gml" -o "${PIANO_GEN}" -n export_progress_widget
    VERBATIM
    DEPENDS "${SERENITY_PROJECT_ROOT}/Meta/embed_as_string.py" "${PIANO_SRC}/ExportProgressWidget.gml"
    MAIN_DEPENDENCY "${PIANO_SRC}/ExportProgressWidget.gml"
)
add_executable(Piano
    "${PIANO_SRC}/main.cpp"
    "${PIANO_SRC}/AudioPlayerLoop.cpp"
    "${PIANO_SRC}/ExportProgressWindow.cpp"
    "${PIANO_SRC}/KeysWidget.cpp"
    "${PIANO_SRC}/MainWidget.cpp"
    "${PIANO_SRC}/PlayerWidget.cpp"
    "${PIANO_SRC}/RollWidget.cpp"
    "${PIANO_SRC}/SamplerWidget.cpp"
    "${PIANO_SRC}/TrackControlsWidget.cpp"
    "${PIANO_SRC}/TrackManager.cpp"
    "${PIANO_SRC}/WaveWidget.cpp"
    "${PIANO_SRC}/ProcessorParameterWidget/ParameterWidget.cpp"
    "${PIANO_SRC}/ProcessorParameterWidget/Slider.cpp"
    "${PIANO_GEN}"
)
target_include_directories(Piano PRIVATE "${PIANO_SRC}" "${CMAKE_CURRENT_BINARY_DIR}")
# LibWM and LibSerenityAudio register their portals from static initializers.
target_link_options(Piano PRIVATE "-Wl,--no-as-needed")
target_link_libraries(Piano PRIVATE LibMain LibGUI LibGfx LibCore LibIPC LibAudio LibDSP LibThreading windowserver serenity-audio)
