# LibSerenityAudio — the PipeWire <-> LibAudio compatibility layer.
#
# Included from the Lagom build (see patches/0001-*.patch) so it can reuse the
# host include paths and the already-built SerenityOS libraries.

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

add_library(serenity-audio SHARED
    LibSerenityAudio.cpp
    AudioServerConnection.cpp
    ClientAudioStream.cpp
    Mixer.cpp
    PipeWireBackend.cpp
)
target_include_directories(serenity-audio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# PipeWire's C API leaves enum/struct members zero-initialized under designated
# initializers, which the build treats as errors.
target_compile_options(serenity-audio PRIVATE -Wno-missing-field-initializers)
# LibSerenityAudio registers its portal from a static initializer; keep it loaded.
target_link_options(serenity-audio PRIVATE "-Wl,--no-as-needed")
target_link_libraries(serenity-audio PRIVATE
    PkgConfig::PIPEWIRE
    LibAudio
    LibCore
    LibIPC
    LibThreading
)
