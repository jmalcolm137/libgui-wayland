# EGLGPU — a Mesa/EGL-backed GPU::Device (with a LibSoftGPU fallback).
#
# Included from the Lagom build (see patches/0007-*.patch) so it can reuse the
# host include paths and the already-built SerenityOS libraries. It is loaded at
# runtime by LibGPU's driver, not linked by applications.

find_package(OpenGL QUIET COMPONENTS OpenGL EGL)
if (NOT OPENGL_FOUND)
    message(STATUS "EGLGPU: OpenGL/EGL development files not found; skipping the Mesa GPU::Device (LibSoftGPU remains available)")
    return()
endif()

add_library(eglgpu SHARED
    EGLGPU.cpp
)
target_include_directories(eglgpu PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(eglgpu PROPERTIES
    OUTPUT_NAME lagom-eglgpu
    SOVERSION 0
    VERSION 0.0.0
)
target_link_libraries(eglgpu PRIVATE
    LibGPU
    LibGfx
    LibSoftGPU
    LibCore
    OpenGL::OpenGL
    OpenGL::EGL
)
