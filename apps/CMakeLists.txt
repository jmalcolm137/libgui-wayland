# Unmodified SerenityOS applications, built directly from their upstream
# sources against the shim. The integration tests drive them (Calculator in
# run-window-and-portal-tests.sh, PDFViewer in run-pdfviewer-test.sh, Piano for
# audio). A desktop's full application set is built by the desktop, not here.
#
# No source is edited; only the GML is precompiled (an app's own CMakeLists uses
# compile_gml, which requires a relative path, so the compiler is invoked
# directly here).

# --- Calculator ---------------------------------------------------------------
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
libwm_gui_executable(Calculator
    SOURCES
        "${CALCULATOR_SRC}/main.cpp"
        "${CALCULATOR_SRC}/Calculator.cpp"
        "${CALCULATOR_SRC}/CalculatorWidget.cpp"
        "${CALCULATOR_SRC}/Keypad.cpp"
        "${CALCULATOR_GML}"
    INCLUDES "${CALCULATOR_SRC}"
    LIBS LibMain LibGUI LibGfx LibCore LibIPC LibCrypto LibDesktop LibURL
)

# --- PDFViewer ----------------------------------------------------------------
set(PDFVIEWER_SRC "${SERENITY_PROJECT_ROOT}/Userland/Applications/PDFViewer")
libwm_gui_executable(PDFViewer
    SOURCES
        "${PDFVIEWER_SRC}/main.cpp"
        "${PDFVIEWER_SRC}/OutlineModel.cpp"
        "${PDFVIEWER_SRC}/PDFViewer.cpp"
        "${PDFVIEWER_SRC}/PDFViewerWidget.cpp"
        "${PDFVIEWER_SRC}/SidebarWidget.cpp"
        "${PDFVIEWER_SRC}/ThumbnailsListView.cpp"
        "${PDFVIEWER_SRC}/ThumbnailsModel.cpp"
    INCLUDES "${PDFVIEWER_SRC}"
    LIBS LibMain LibGUI LibGfx LibCore LibIPC LibPDF LibFileSystemAccessClient LibConfig LibURL
)

# --- Piano (audio through the AudioServer service) ---------------------------
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
libwm_gui_executable(Piano
    SOURCES
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
    INCLUDES "${PIANO_SRC}" "${CMAKE_CURRENT_BINARY_DIR}"
    LIBS LibMain LibGUI LibGfx LibCore LibIPC LibAudio LibDSP LibThreading
)
