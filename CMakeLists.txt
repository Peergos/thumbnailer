cmake_minimum_required(VERSION 3.16)
project(thumbnailer C)

set(CMAKE_C_STANDARD 11)

find_package(JNI REQUIRED)

# FFMPEG_ROOT can be passed in from CI: -DFFMPEG_ROOT=/path/to/ffmpeg-build
if(DEFINED FFMPEG_ROOT)
    set(CMAKE_PREFIX_PATH ${FFMPEG_ROOT})
    set(ENV{PKG_CONFIG_PATH} "${FFMPEG_ROOT}/lib/pkgconfig")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC  REQUIRED libavcodec)
pkg_check_modules(AVUTIL   REQUIRED libavutil)
pkg_check_modules(SWSCALE  REQUIRED libswscale)
pkg_check_modules(LIBWEBP  REQUIRED libwebp)

add_library(thumbnailer SHARED native/thumbnailer.c)

set_target_properties(thumbnailer PROPERTIES
    PREFIX ""          # no "lib" prefix on the output name
    C_VISIBILITY_PRESET hidden
)

target_include_directories(thumbnailer PRIVATE
    ${JNI_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${LIBWEBP_INCLUDE_DIRS}
    $<$<BOOL:${FFMPEG_ROOT}>:${FFMPEG_ROOT}/include>
)

# Static link so the .so/.dylib/.dll is self-contained.
# LIBRARY_DIRS tells the linker where the .a files live (e.g. ffmpeg-build/lib).
target_link_directories(thumbnailer PRIVATE
    ${AVFORMAT_LIBRARY_DIRS}
    ${AVCODEC_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
    ${SWSCALE_LIBRARY_DIRS}
    ${LIBWEBP_LIBRARY_DIRS}
)

target_link_libraries(thumbnailer PRIVATE
    ${AVFORMAT_STATIC_LIBRARIES}
    ${AVCODEC_STATIC_LIBRARIES}
    ${AVUTIL_STATIC_LIBRARIES}
    ${SWSCALE_STATIC_LIBRARIES}
    ${LIBWEBP_STATIC_LIBRARIES}
)

target_link_options(thumbnailer PRIVATE
    ${AVFORMAT_LDFLAGS_OTHER}
    ${AVCODEC_LDFLAGS_OTHER}
    ${AVUTIL_LDFLAGS_OTHER}
    ${SWSCALE_LDFLAGS_OTHER}
    ${LIBWEBP_LDFLAGS_OTHER}
)

# On macOS, don't embed an absolute path to the JVM dylib
if(APPLE)
    set_target_properties(thumbnailer PROPERTIES
        LINK_FLAGS "-undefined dynamic_lookup"
    )
endif()

# Bind all global symbols locally within the .so so that PC-relative
# relocations in FFmpeg's assembly code (R_X86_64_PC32, R_AARCH64_*)
# are valid — the linker can guarantee those symbols won't be interposed.
if(UNIX AND NOT APPLE)
    target_link_options(thumbnailer PRIVATE -Wl,-Bsymbolic)
endif()
