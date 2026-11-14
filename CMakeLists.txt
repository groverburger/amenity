cmake_minimum_required(VERSION 3.25)
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "Minimum macOS version")
endif()
# Link the C runtime statically on Windows so the .exe runs without installing anything.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(amenity VERSION 0.1.0 LANGUAGES C CXX)

option(AMENITY_APP_BUNDLE "Build Amenity.app on macOS instead of a plain executable" OFF)

if(MSVC)
  set(AMENITY_WARNINGS /W3)
  add_compile_options(/utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  set(AMENITY_WARNINGS -Wall -Wextra)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW TRUE)
# imgui and miniaudio are compiled directly below, so their own build files
# (if any) are skipped by pointing SOURCE_SUBDIR at a directory that doesn't exist.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.9
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR unused)
FetchContent_Declare(miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.25
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR unused)
FetchContent_Declare(signalsmith-stretch
  GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
  GIT_TAG 1.4.0
  GIT_SHALLOW TRUE
  SYSTEM)
FetchContent_MakeAvailable(glfw imgui miniaudio signalsmith-stretch)

add_library(miniaudio_impl STATIC src/miniaudio_impl.c)
target_include_directories(miniaudio_impl SYSTEM PUBLIC ${miniaudio_SOURCE_DIR})

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_lib SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC glfw)

# Everything testable without a window or an audio device.
add_library(amenity_core STATIC src/sample.cpp src/slicer.cpp src/engine.cpp src/timeline.cpp src/effects.cpp
  src/platform.cpp)
target_include_directories(amenity_core PUBLIC src)
target_link_libraries(amenity_core PUBLIC miniaudio_impl)
target_link_libraries(amenity_core PRIVATE signalsmith-stretch)
target_compile_options(amenity_core PRIVATE ${AMENITY_WARNINGS})

add_executable(amenity src/main.cpp)
target_link_libraries(amenity PRIVATE amenity_core imgui_lib glfw)
target_compile_options(amenity PRIVATE ${AMENITY_WARNINGS})

add_executable(amenity_tests tests/tests.cpp)
target_link_libraries(amenity_tests PRIVATE amenity_core)
target_compile_options(amenity_tests PRIVATE ${AMENITY_WARNINGS})

if(APPLE)
  set(APPLE_AUDIO_FRAMEWORKS
    "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
  target_link_libraries(miniaudio_impl PUBLIC ${APPLE_AUDIO_FRAMEWORKS})
  target_link_libraries(amenity PRIVATE "-framework OpenGL")
  if(AMENITY_APP_BUNDLE)
    set_target_properties(amenity PROPERTIES
      MACOSX_BUNDLE TRUE
      OUTPUT_NAME amenity
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)
    configure_file(packaging/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)
  endif()
elseif(WIN32)
  target_link_libraries(amenity_core PUBLIC mfplat mfreadwrite mfuuid ole32 shell32)
  target_link_libraries(amenity PRIVATE opengl32)
  target_sources(amenity PRIVATE packaging/amenity.manifest)
  target_sources(amenity_tests PRIVATE packaging/amenity.manifest)
  # A windowed app with no console, still starting at main().
  set_target_properties(amenity PROPERTIES WIN32_EXECUTABLE TRUE)
  if(MSVC)
    target_link_options(amenity PRIVATE /ENTRY:mainCRTStartup)
  endif()
endif()

enable_testing()
add_test(NAME amenity_tests COMMAND amenity_tests)
