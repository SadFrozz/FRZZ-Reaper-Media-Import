cmake_minimum_required(VERSION 3.24)
project(FRZZReaperMediaImport VERSION 1.1.1 LANGUAGES CXX)

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
  add_library(frzz_ff${FFMPEG_MAJOR} OBJECT src/ffmpeg_scan.cpp src/thd_ac3_decoder.cpp)
  list(APPEND FRZZ_FFMPEG_ADAPTERS frzz_ff${FFMPEG_MAJOR})
  target_include_directories(frzz_ff${FFMPEG_MAJOR} SYSTEM PRIVATE external/ffmpeg-headers/${HEADER_VERSION})
  target_compile_definitions(frzz_ff${FFMPEG_MAJOR} PRIVATE
    FRZZ_FFMPEG_SCAN=scan_ff${FFMPEG_MAJOR}
    FRZZ_THD_FACTORY=create_thd_ff${FFMPEG_MAJOR})
  if(MSVC)
    target_compile_definitions(frzz_ff${FFMPEG_MAJOR} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(frzz_ff${FFMPEG_MAJOR} PRIVATE /utf-8 /EHsc /wd4996)
  else()
    target_compile_options(frzz_ff${FFMPEG_MAJOR} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  math(EXPR FFMPEG_MAJOR "${FFMPEG_MAJOR}+1")
endforeach()

set(FRZZ_PLUGIN_SOURCES
  src/plugin.cpp
  src/subtitle_parser.cpp
  src/media_metadata.cpp
  src/thd_ac3.cpp)
if(WIN32)
  list(APPEND FRZZ_PLUGIN_SOURCES src/version.rc)
endif()

add_library(reaper_frzz_media_import SHARED ${FRZZ_PLUGIN_SOURCES})
target_link_libraries(reaper_frzz_media_import PRIVATE
  ${FRZZ_FFMPEG_ADAPTERS} frzz_wdl)
target_include_directories(reaper_frzz_media_import SYSTEM PRIVATE
  external/reaper-sdk/sdk
  external/WDL/WDL)

if(MSVC)
  target_compile_definitions(reaper_frzz_media_import PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_compile_options(reaper_frzz_media_import PRIVATE /W4 /utf-8 /EHsc)
elseif(APPLE)
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
endif()
