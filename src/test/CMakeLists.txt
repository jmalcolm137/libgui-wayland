# LibWM's test programs and their CTest registrations.
#
# These are minimal LibGUI applications (and one direct unit-test binary) that
# the scripts/run-*.sh integration suites drive under a headless Wayland
# compositor. Run them all with:
#
#   ctest --test-dir build/lagom -L libwm --output-on-failure
#
# or use scripts/check.sh, which also builds first.

# The tests include LibWM's private headers, some of which use the generated stubs.
set(LIBWM_TEST_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/.." "${LIBWM_STUB_DIR}")

# The menu suite's menubar target (Calculator moved to the desktop).
libwm_gui_executable(libwm-menu-test
    SOURCES MenuTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC)

# A minimal unmodified-style LibGUI application used to exercise the shim.
libwm_gui_executable(libwm-test-window
    SOURCES TestWindow.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC)

# Reads the Wayland clipboard then publishes text, to exercise the clipboard bridge.
libwm_gui_executable(libwm-clipboard-test
    SOURCES ClipboardTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC)

# Shows the About dialog directly (it hardcodes /res/... paths).
libwm_gui_executable(libwm-about-test
    SOURCES AboutTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC)

# Toggles fullscreen to verify xdg_toplevel fullscreen handling.
libwm_gui_executable(libwm-fullscreen-test
    SOURCES FullscreenTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC)

# A menubar with a nested submenu, to verify submenu navigation.
libwm_gui_executable(libwm-submenu-test
    SOURCES SubmenuTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC)

# Spawns an app through Desktop::AppFile - the Taskbar system menu's launch path.
libwm_gui_executable(libwm-appfile-test
    SOURCES AppFileLaunchTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC LibDesktop LibURL)

# Client-side GL through LibWM: LibGL renders a solid colour (EGLGPU or
# LibSoftGPU, forced by the suite), LibGUI paints the bitmap, LibWM presents it.
# This is the framework's own client-side GL coverage; the desktop builds and
# tests the GL applications (3DFileViewer, Tubes) itself.
libwm_gui_executable(libwm-gl-test
    SOURCES GlTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC LibGL LibGPU)

# The WindowServer requests with no Wayland equivalent (base size/increment,
# aspect ratio, forced shadow, flash flush, global mouse tracking, screen
# numbers, theme refresh): each is sent once, so the suite can check the trace.
libwm_gui_executable(libwm-wm-requests-test
    SOURCES WmRequestsTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC)

# PDF rendering through LibWM (the PDFViewer application lives in the desktop):
# opens a PDF through the FileSystemAccess portal and renders it with LibPDF.
libwm_gui_executable(libwm-pdf-test
    SOURCES PdfTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC LibPDF LibFileSystemAccessClient)

# xdg-activation-v1: a window opened from a click requests activation, and a
# launcher-provided XDG_ACTIVATION_TOKEN activates the first window.
libwm_gui_executable(libwm-activation-test
    SOURCES ActivationTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC)

# Animated client-side 3D GL (3DFileViewer lives in the desktop): LibGL renders
# frames into an offscreen bitmap, LibGUI paints it, LibWM presents it.
libwm_gui_executable(libwm-3d-test
    SOURCES ThreeDTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC LibGL LibGPU)

# Global mouse tracking: a GUI::MouseTracker subclass must receive the pointer
# position outside the window too (ext-input-observation-v1).
libwm_gui_executable(libwm-mouse-tracking-test
    SOURCES MouseTrackingTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC)

# set_global_cursor_position through wp_pointer_warp_v1: asks for the cursor to
# be moved to an output position on this window.
libwm_gui_executable(libwm-warp-test
    SOURCES WarpTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC)

# Interactive resize: asks for a bottom-right resize as the ResizeCorner does.
libwm_gui_executable(libwm-resize-test
    SOURCES ResizeTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC)

# HiDPI fine detail: full-width rects and thin/thick lines dumped at 1x and 2x,
# checked for ops that don't scale.
libwm_gui_executable(libwm-hidpi-detail-test
    SOURCES HiDpiDetailTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC)

# Direct unit tests of LibWM's Wayland/Linux translation helpers (MenuController,
# clipboard PNG/Serenity image transcoding, in-process ConfigServer portal,
# WaylandClient thread safety, scale inference, resize direction mapping).
libwm_gui_executable(libwm-unit-test
    SOURCES UnitTest.cpp
    INCLUDES ${LIBWM_TEST_INCLUDES}
    LIBS LibMain LibGUI LibGfx LibCore LibIPC LibConfig)
add_dependencies(libwm-unit-test libwm_default_stubs)

# --- CTest --------------------------------------------------------------------
# Each suite is a script that builds what it needs, starts its own headless
# compositor in a private XDG_RUNTIME_DIR, and exits non-zero on any failed
# check. They invoke ninja on this build tree, so they must not run
# concurrently with each other (RUN_SERIAL).
set(LIBWM_SCRIPTS "${LIBGUI_WAYLAND_ROOT}/scripts")
set(LIBWM_TEST_ENVIRONMENT
    "BUILD_DIR=${CMAKE_BINARY_DIR}"
    "SERENITY_SRC=${SERENITY_PROJECT_ROOT}"
)

function(libwm_add_script_test name script labels)
    add_test(NAME "libwm.${name}" COMMAND "${LIBWM_SCRIPTS}/${script}")
    set_tests_properties("libwm.${name}" PROPERTIES
        ENVIRONMENT "${LIBWM_TEST_ENVIRONMENT}"
        LABELS "libwm;${labels}"
        RUN_SERIAL TRUE
        TIMEOUT 900
    )
endfunction()

libwm_add_script_test(headless run-headless-tests.sh "integration")
libwm_add_script_test(mouse-tracking run-mouse-tracking-test.sh "integration")
libwm_add_script_test(window-and-portal run-window-and-portal-tests.sh "integration")
libwm_add_script_test(clipboard run-clipboard-test.sh "integration")
libwm_add_script_test(menu run-menu-test.sh "integration")
libwm_add_script_test(pdf run-pdf-test.sh "integration")
libwm_add_script_test(activation run-activation-test.sh "integration")
libwm_add_script_test(gl run-gl-test.sh "integration;gl")
libwm_add_script_test(settings-portal run-settings-portal-test.sh "integration")
# No libsystemd in the build: the portal bridge is not compiled in.
set_tests_properties(libwm.settings-portal PROPERTIES SKIP_RETURN_CODE 77)
libwm_add_script_test(3d run-3d-test.sh "integration;gl")
libwm_add_script_test(triage test-triage-failure.sh "tools")
libwm_add_script_test(patches check-patches.sh "tools")
libwm_add_script_test(upstream-libgfx run-lib-tests.sh "upstream")
