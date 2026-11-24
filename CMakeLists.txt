cmake_minimum_required(VERSION 3.24)
project(FRZZReaperMediaImport VERSION 1.3.0 LANGUAGES CXX)

if(WIN32)
  if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Windows x64 with the MSVC ABI is required")
  endif()
elseif(APPLE)
  if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "macOS target architectures" FORCE)
  endif()
else()
  message(FATAL_ERROR "Only Windows x64 and macOS are supported")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(FRZZ_ENABLE_LOGGING "Write lifecycle diagnostics to REAPER resource directory" ON)

add_library(frzz_wdl STATIC external/WDL/WDL/projectcontext.cpp)
if(MSVC)
  target_compile_definitions(frzz_wdl PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

set(FFMPEG_HEADER_VERSIONS 5.1.6 6.1.2 7.1.1 8.1.2 9.0.2)
set(FFMPEG_MAJOR 59)
foreach(HEADER_VERSION IN LISTS FFMPEG_HEADER_VERSIONS)
  add_library(frzz_ff${FFMPEG_MAJOR} OBJECT src/ffmpeg_scan.cpp src/thd_ac3_decoder.cpp src/container_ffmpeg.cpp)
  list(APPEND FRZZ_FFMPEG_ADAPTERS frzz_ff${FFMPEG_MAJOR})
  target_include_directories(frzz_ff${FFMPEG_MAJOR} SYSTEM PRIVATE external/ffmpeg-headers/${HEADER_VERSION})
  target_compile_definitions(frzz_ff${FFMPEG_MAJOR} PRIVATE
    FRZZ_FFMPEG_SCAN=scan_ff${FFMPEG_MAJOR}
    FRZZ_THD_FACTORY=create_thd_ff${FFMPEG_MAJOR}
    FRZZ_CONTAINER_PROBE=probe_container_${FFMPEG_MAJOR}
    FRZZ_CONTAINER_OPEN=open_container_${FFMPEG_MAJOR}
    FRZZ_CONTAINER_PLANAR_BED=native_planar_bed_${FFMPEG_MAJOR})
  if(WIN32 OR APPLE)
    target_sources(frzz_ff${FFMPEG_MAJOR} PRIVATE src/container_native_timing.cpp)
    target_compile_definitions(frzz_ff${FFMPEG_MAJOR} PRIVATE FRZZ_NATIVE_TIMING=native_timing_${FFMPEG_MAJOR})
  endif()
  if(MSVC)
    target_compile_definitions(frzz_ff${FFMPEG_MAJOR} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(frzz_ff${FFMPEG_MAJOR} PRIVATE /utf-8 /EHsc /wd4996)
  else()
    target_compile_options(frzz_ff${FFMPEG_MAJOR} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  math(EXPR FFMPEG_MAJOR "${FFMPEG_MAJOR}+1")
endforeach()

set(FRZZ_PLUGIN_SOURCES
  src/container_audio.cpp
  src/container_audio_menu.cpp
  src/video_settings.cpp
  src/container_peaks.cpp
  src/matroska_eac3.cpp
  src/container_decoder.cpp
  src/mov_track_metadata.cpp
  src/plugin.cpp
  src/subtitle_parser.cpp
  src/media_metadata.cpp
  src/thd_ac3.cpp)
if(WIN32)
  list(APPEND FRZZ_PLUGIN_SOURCES src/version.rc src/container_audio_dialog.cpp)
endif()

add_library(reaper_frzz_media_import SHARED ${FRZZ_PLUGIN_SOURCES})
target_link_libraries(reaper_frzz_media_import PRIVATE
  ${FRZZ_FFMPEG_ADAPTERS} frzz_wdl)
target_include_directories(reaper_frzz_media_import SYSTEM PRIVATE
  external/reaper-sdk/sdk
  external/WDL/WDL)

function(frzz_configure_hdr_adapters)
  target_compile_definitions(frzz_ff59 PRIVATE FRZZ_HDR_LEGACY_METADATA)
  target_compile_definitions(frzz_ff60 PRIVATE FRZZ_HDR_LEGACY_METADATA)
endfunction()

if(MSVC)
  enable_language(C)
  enable_language(ASM_MASM)
  include(FetchContent)
  FetchContent_Declare(minhook
    URL https://codeload.github.com/TsudaKageyu/minhook/tar.gz/c3fcafdc10146beb5919319d0683e44e3c30d537
    URL_HASH SHA256=31757833475be039c16359d4e276e718db6ba2ba61a6b870053a112106015822
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR frzz-static)
  FetchContent_MakeAvailable(minhook)
  add_library(frzz_minhook STATIC
    "${minhook_SOURCE_DIR}/src/buffer.c"
    "${minhook_SOURCE_DIR}/src/hook.c"
    "${minhook_SOURCE_DIR}/src/trampoline.c"
    "${minhook_SOURCE_DIR}/src/hde/hde64.c")
  target_include_directories(frzz_minhook PUBLIC "${minhook_SOURCE_DIR}/include")
  install(FILES "${minhook_SOURCE_DIR}/LICENSE.txt" DESTINATION "." RENAME "LICENSE-MINHOOK.txt")
  target_sources(reaper_frzz_media_import PRIVATE src/container_native_video.cpp src/container_native_video_win64.asm)
  option(FRZZ_ENABLE_WINDOWS_HDR "Build the D3D11 HDR renderer" ON)
  option(FRZZ_ENABLE_WINDOWS_QSV "Enable experimental Intel oneVPL surfaces for the Windows HDR renderer" OFF)
  if(FRZZ_ENABLE_WINDOWS_HDR)
    frzz_configure_hdr_adapters()
    if(FRZZ_ENABLE_WINDOWS_QSV)
      find_path(FRZZ_ONEVPL_INCLUDE_DIR vpl/mfxstructures.h REQUIRED)
    endif()
    foreach(adapter IN LISTS FRZZ_FFMPEG_ADAPTERS)
      target_compile_definitions(${adapter} PRIVATE FRZZ_WINDOWS_HDR=windows_hdr_${adapter} _CRT_SECURE_NO_WARNINGS)
      target_sources(${adapter} PRIVATE src/video_hdr_windows.cpp src/video_hdr_d3d11.c src/video_hdr_dxva2.cpp)
      target_include_directories(${adapter} SYSTEM PRIVATE
        external/reaper-sdk/sdk external/WDL/WDL)
      target_compile_features(${adapter} PRIVATE c_std_11)
      if(FRZZ_ENABLE_WINDOWS_QSV)
        target_include_directories(${adapter} SYSTEM PRIVATE "${FRZZ_ONEVPL_INCLUDE_DIR}")
        target_compile_definitions(${adapter} PRIVATE FRZZ_WINDOWS_QSV)
      endif()
    endforeach()
    target_sources(reaper_frzz_media_import PRIVATE src/video_hdr_shader_cache.cpp src/video_hdr_color.c)
    target_compile_definitions(reaper_frzz_media_import PRIVATE FRZZ_ENABLE_WINDOWS_HDR)
    target_compile_features(reaper_frzz_media_import PRIVATE c_std_11)
    target_link_libraries(reaper_frzz_media_import PRIVATE d3d11 dxgi dxguid d3dcompiler)
  endif()
  target_link_libraries(reaper_frzz_media_import PRIVATE comctl32 advapi32 frzz_minhook)
  target_compile_definitions(reaper_frzz_media_import PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_compile_options(reaper_frzz_media_import PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4;/utf-8;/EHsc>)
elseif(APPLE)
  enable_language(OBJCXX ASM)
  option(FRZZ_ENABLE_MACOS_HDR "Build the Intel Mac VideoToolbox HDR renderer" ON)
  if(FRZZ_ENABLE_MACOS_HDR)
    enable_language(C)
    frzz_configure_hdr_adapters()
    find_library(FRZZ_COREVIDEO CoreVideo REQUIRED)
    find_library(FRZZ_IOSURFACE IOSurface REQUIRED)
    find_library(FRZZ_OPENGL OpenGL REQUIRED)
    foreach(adapter IN LISTS FRZZ_FFMPEG_ADAPTERS)
      target_compile_definitions(${adapter} PRIVATE FRZZ_MACOS_HDR=macos_hdr_${adapter})
      set_target_properties(${adapter} PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED YES C_STANDARD 11)
      target_sources(${adapter} PRIVATE src/video_hdr_macos.mm src/video_hdr_placebo.c)
      target_include_directories(${adapter} SYSTEM PRIVATE
        external/reaper-sdk/sdk external/WDL/WDL "${CMAKE_BINARY_DIR}/compat")
    endforeach()
    target_sources(reaper_frzz_media_import PRIVATE src/video_hdr_color.c)
    target_link_libraries(reaper_frzz_media_import PRIVATE
      "${FRZZ_COREVIDEO}" "${FRZZ_IOSURFACE}" "${FRZZ_OPENGL}")
    target_compile_definitions(reaper_frzz_media_import PRIVATE FRZZ_ENABLE_MACOS_HDR)
  endif()
  target_sources(reaper_frzz_media_import PRIVATE
    src/container_native_video.cpp src/container_native_video_macos.cpp src/container_native_video_macos.S)
  set_target_properties(reaper_frzz_media_import PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED YES)
  find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
  target_sources(reaper_frzz_media_import PRIVATE src/container_audio_dialog.mm external/WDL/WDL/swell/swell-modstub.mm)
  target_link_libraries(reaper_frzz_media_import PRIVATE "${COCOA_FRAMEWORK}")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/compat")
  file(CREATE_LINK "${CMAKE_SOURCE_DIR}/external/WDL/WDL"
    "${CMAKE_BINARY_DIR}/WDL" SYMBOLIC)
  target_include_directories(reaper_frzz_media_import SYSTEM PRIVATE
    "${CMAKE_BINARY_DIR}/compat")
  target_compile_definitions(reaper_frzz_media_import PRIVATE SWELL_PROVIDED_BY_APP NOMINMAX)
  target_compile_options(reaper_frzz_media_import PRIVATE -Wall -Wextra -Wpedantic)
  target_link_options(reaper_frzz_media_import PRIVATE "-Wl,-undefined,dynamic_lookup")
endif()

if(FRZZ_ENABLE_LOGGING)
  target_compile_definitions(reaper_frzz_media_import PRIVATE FRZZ_ENABLE_LOGGING)
endif()
set_target_properties(reaper_frzz_media_import PROPERTIES PREFIX "")
install(TARGETS reaper_frzz_media_import
  RUNTIME DESTINATION .
  LIBRARY DESTINATION .)
if(FRZZ_ENABLE_WINDOWS_HDR OR FRZZ_ENABLE_MACOS_HDR)
  install(FILES external/LICENSE-libplacebo.txt DESTINATION .)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(subtitle_parser_tests tests/subtitle_parser_tests.cpp src/subtitle_parser.cpp)
  if(MSVC)
    target_compile_options(subtitle_parser_tests PRIVATE /W4 /utf-8 /EHsc)
    target_compile_definitions(subtitle_parser_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  else()
    target_compile_options(subtitle_parser_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME subtitle_parser COMMAND subtitle_parser_tests)

  add_executable(mov_track_metadata_tests tests/mov_track_metadata_tests.cpp src/mov_track_metadata.cpp)
  target_include_directories(mov_track_metadata_tests PRIVATE src)
  if(MSVC)
    target_compile_options(mov_track_metadata_tests PRIVATE /W4 /utf-8 /EHsc)
    target_compile_definitions(mov_track_metadata_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  else()
    target_compile_options(mov_track_metadata_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME mov_track_metadata COMMAND mov_track_metadata_tests)

  add_executable(container_peaks_tests tests/container_peaks_tests.cpp src/container_peaks.cpp)
  target_include_directories(container_peaks_tests PRIVATE src)
  target_include_directories(container_peaks_tests SYSTEM PRIVATE external/reaper-sdk/sdk)
  if(MSVC)
    target_compile_options(container_peaks_tests PRIVATE /W4 /utf-8 /EHsc)
    target_compile_definitions(container_peaks_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  elseif(APPLE)
    target_include_directories(container_peaks_tests SYSTEM PRIVATE "${CMAKE_BINARY_DIR}/compat")
    target_compile_definitions(container_peaks_tests PRIVATE SWELL_PROVIDED_BY_APP NOMINMAX)
    target_compile_options(container_peaks_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME container_peaks COMMAND container_peaks_tests)

  add_executable(matroska_eac3_tests tests/matroska_eac3_tests.cpp src/matroska_eac3.cpp)
  target_include_directories(matroska_eac3_tests PRIVATE src)
  if(MSVC)
    target_compile_options(matroska_eac3_tests PRIVATE /W4 /utf-8 /EHsc)
  else()
    target_compile_options(matroska_eac3_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME matroska_eac3 COMMAND matroska_eac3_tests)

  add_executable(hevc_preroll_tests tests/hevc_preroll_tests.cpp)
  target_include_directories(hevc_preroll_tests PRIVATE src)
  add_test(NAME hevc_preroll COMMAND hevc_preroll_tests)

  if(FRZZ_ENABLE_WINDOWS_HDR OR FRZZ_ENABLE_MACOS_HDR)
    add_executable(video_hdr_color_tests tests/video_hdr_color_tests.c src/video_hdr_color.c)
    target_compile_features(video_hdr_color_tests PRIVATE c_std_11)
    target_include_directories(video_hdr_color_tests PRIVATE src)
    target_include_directories(video_hdr_color_tests SYSTEM PRIVATE external/ffmpeg-headers/9.0.2)
    if(WIN32)
      target_link_libraries(video_hdr_color_tests PRIVATE d3dcompiler)
    endif()
    add_test(NAME video_hdr_color COMMAND video_hdr_color_tests)
  endif()
endif()
