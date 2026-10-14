cmake_minimum_required(VERSION 3.28)
project(VisualEnhancer VERSION 16.0.0 LANGUAGES C CXX RC)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "Visual Enhancer requires a Windows MSVC x64 toolchain.")
endif()

get_filename_component(VE_WORKSPACE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(VE_NATIVE_ROOT "${VE_WORKSPACE_ROOT}/native (dev)" CACHE PATH "Native engine source and SDK directory")
set(VE_TEST_ROOT "${VE_WORKSPACE_ROOT}/Tests" CACHE PATH "Permanent test source directory")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.12...<6.13 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Multimedia Network Svg Concurrent)
get_target_property(VE_QT_PLATFORM_INCLUDES Qt6::Platform INTERFACE_INCLUDE_DIRECTORIES)
if(NOT VE_QT_PLATFORM_INCLUDES MATCHES "[/\\\\]mkspecs[/\\\\]win32-msvc(;|$)")
    message(FATAL_ERROR "Visual Enhancer requires the Qt 6.12.x MSVC x64 kit.")
endif()

file(GLOB_RECURSE LIBRAW_SOURCES CONFIGURE_DEPENDS "third_party/LibRaw-0.22.2/src/*.cpp")
list(FILTER LIBRAW_SOURCES EXCLUDE REGEX "_ph\\.cpp$")
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/zlib-1.3.2 EXCLUDE_FROM_ALL)
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
set(WITH_SIMD OFF CACHE BOOL "" FORCE)
set(WITH_TURBOJPEG OFF CACHE BOOL "" FORCE)
include(ExternalProject)
set(VE_JPEG_BINARY "${CMAKE_CURRENT_BINARY_DIR}/jpeg")
file(MAKE_DIRECTORY "${VE_JPEG_BINARY}")
ExternalProject_Add(ve_jpeg_build
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libjpeg-turbo-3.1.3"
    BINARY_DIR "${VE_JPEG_BINARY}"
    CMAKE_ARGS -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF -DWITH_CRT_DLL=ON
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG> --target jpeg-static --parallel 8
    INSTALL_COMMAND ""
)
add_library(ve_jpeg STATIC IMPORTED GLOBAL)
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" upper)
    set_target_properties(ve_jpeg PROPERTIES "IMPORTED_LOCATION_${upper}" "${VE_JPEG_BINARY}/${config}/jpeg-static.lib")
endforeach()
add_dependencies(ve_jpeg ve_jpeg_build)
add_library(ve_libraw STATIC ${LIBRAW_SOURCES})
target_include_directories(ve_libraw SYSTEM PUBLIC third_party/LibRaw-0.22.2)
target_include_directories(ve_libraw PRIVATE third_party/libjpeg-turbo-3.1.3/src "${VE_JPEG_BINARY}")
target_compile_definitions(ve_libraw PUBLIC LIBRAW_NODLL PRIVATE USE_ZLIB USE_JPEG)
target_link_libraries(ve_libraw PRIVATE ws2_32 zlibstatic ve_jpeg)

add_library(ve_core STATIC
    src/configuration/settings.cpp
    src/infrastructure/paths.cpp
    src/infrastructure/process.cpp
    src/infrastructure/log.cpp
    src/infrastructure/vulkan_device.cpp
    src/media/media.cpp
    src/media/video_preview.cpp
    src/media/video_backend.cpp
    src/media/image_codecs.cpp
    src/media/png_encoder.cpp
    src/media/image_metadata.cpp
    src/media/image_alpha.cpp
    src/media/packet_stream.cpp
    src/media/cuda_video.cpp
    src/media/host_video.cpp
    src/media/video_delivery.cpp
    src/media/frame_batch.cpp
    src/processing/native_engines.cpp
    src/processing/vfx_native.cpp
    src/processing/vfx_video.cpp
    src/processing/vfx_worker.cpp
    src/processing/neural_video_delivery.cpp
    src/processing/effects.cpp
    src/processing/frame_stream.cpp
    src/processing/video_still_preview.cpp
    src/processing/color_analysis.cpp
    src/processing/pipeline.cpp
    src/processing/interpolation_video.cpp
)
target_include_directories(ve_core PUBLIC src "${VE_NATIVE_ROOT}/modules" PRIVATE third_party/libheif/include third_party/ffmpeg)
target_include_directories(ve_core PRIVATE third_party/libjpeg-turbo-3.1.3/src "${VE_JPEG_BINARY}")
target_include_directories(ve_core SYSTEM PRIVATE third_party)
target_include_directories(ve_core SYSTEM PRIVATE "${VE_NATIVE_ROOT}/external/NVIDIA GPU Computing Toolkit/CUDA/v13.0/include")
target_link_libraries(ve_core PUBLIC Qt6::Core Qt6::Gui Qt6::Network Qt6::Concurrent Qt6::Svg ve_libraw)
target_link_libraries(ve_core PRIVATE zlibstatic ve_jpeg)
target_compile_definitions(ve_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    VE_APP_VERSION="${PROJECT_VERSION}")

add_library(ve_desktop STATIC
    src/application/qml_facade.hpp
    src/application/controller.cpp
    src/application/controller_settings.cpp
    src/application/controller_workflows.cpp
    src/presentation/batch_model.cpp
    src/presentation/image_providers.cpp
    src/presentation/preview_player.cpp
    src/presentation/window_chrome.cpp
)
target_link_libraries(ve_desktop PUBLIC ve_core Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Multimedia dwmapi shell32 user32)

function(ve_windows_identity target description filename icon identity)
    set(VE_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/${icon}")
    set(VE_VERSION_NUMERIC "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
    set(VE_VERSION_STRING "${PROJECT_VERSION}.0")
    set(VE_PRODUCT_NAME "Visual Enhancer")
    set(VE_FILE_DESCRIPTION "${description}")
    set(VE_ORIGINAL_FILENAME "${filename}")
    set(VE_ASSEMBLY_IDENTITY "${identity}")
    configure_file(src/presentation/application.rc.in "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc" @ONLY)
    configure_file(src/presentation/application.manifest.in "${CMAKE_CURRENT_BINARY_DIR}/${target}.manifest" @ONLY)
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc")
    target_link_options(${target} PRIVATE /MANIFEST:EMBED
        "/MANIFESTINPUT:${CMAKE_CURRENT_BINARY_DIR}/${target}.manifest")
endfunction()

add_executable(visual_enhancer WIN32 src/application/main.cpp src/launcher/runtime_paths.qrc)
set_target_properties(visual_enhancer PROPERTIES OUTPUT_NAME "Visual Enhancer.app")
target_link_libraries(visual_enhancer PRIVATE ve_desktop)
ve_windows_identity(visual_enhancer "Visual Enhancer" "Visual Enhancer.app.exe"
    src/desktop/app.ico Merserk.VisualEnhancer)

add_executable(ve_cli src/console/main.cpp src/console/cli.cpp src/launcher/runtime_paths.qrc)
set_target_properties(ve_cli PROPERTIES OUTPUT_NAME "VE_CLI.app")
target_link_libraries(ve_cli PRIVATE ve_core shell32)
ve_windows_identity(ve_cli "Visual Enhancer CLI" "VE_CLI.app.exe"
    src/console/cmd.ico Merserk.VisualEnhancer.CLI)

# Public entry points start ordinary executables beside their dependencies in app/.
add_executable(visual_enhancer_launcher WIN32 src/launcher/main.cpp)
set_target_properties(visual_enhancer_launcher PROPERTIES OUTPUT_NAME "Visual Enhancer")
target_compile_definitions(visual_enhancer_launcher PRIVATE "VE_LAUNCH_TARGET=L\"Visual Enhancer.app.exe\"")
add_dependencies(visual_enhancer_launcher visual_enhancer)
ve_windows_identity(visual_enhancer_launcher "Visual Enhancer" "Visual Enhancer.exe"
    src/desktop/app.ico Merserk.VisualEnhancer.Launcher)
add_executable(ve_cli_launcher src/launcher/main.cpp)
set_target_properties(ve_cli_launcher PROPERTIES OUTPUT_NAME "VE_CLI")
target_compile_definitions(ve_cli_launcher PRIVATE VE_CONSOLE_LAUNCHER "VE_LAUNCH_TARGET=L\"VE_CLI.app.exe\"")
add_dependencies(ve_cli_launcher ve_cli)
ve_windows_identity(ve_cli_launcher "Visual Enhancer CLI" "VE_CLI.exe"
    src/console/cmd.ico Merserk.VisualEnhancer.CLI.Launcher)
foreach(target visual_enhancer_launcher ve_cli_launcher)
    set_target_properties(${target} PROPERTIES AUTOMOC OFF AUTORCC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    target_link_libraries(${target} PRIVATE user32 shell32)
endforeach()

if(MSVC)
    foreach(target ve_core ve_desktop visual_enhancer ve_cli visual_enhancer_launcher ve_cli_launcher)
        target_compile_options(${target} PRIVATE /guard:cf /sdl /GS)
    endforeach()
    foreach(target ve_libraw zlibstatic)
        target_compile_options(${target} PRIVATE /guard:cf /GS)
    endforeach()
    foreach(target ve_core ve_desktop visual_enhancer ve_cli)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    endforeach()
    foreach(target visual_enhancer ve_cli visual_enhancer_launcher ve_cli_launcher)
        target_link_options(${target} PRIVATE /GUARD:CF /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT)
    endforeach()
endif()

option(BUILD_TESTING "Build the permanent unit and contract tests" OFF)
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.12...<6.13 REQUIRED COMPONENTS Test)
    find_program(VE_PWSH_EXECUTABLE NAMES pwsh REQUIRED)
    add_executable(ve_launcher_probe "${VE_TEST_ROOT}/launcher_probe.cpp")
    set_target_properties(ve_launcher_probe PROPERTIES AUTOMOC OFF AUTORCC OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(ve_launcher_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_test(NAME launcher_contracts COMMAND "${VE_PWSH_EXECUTABLE}" -NoProfile -File
        "${VE_TEST_ROOT}/launcher_tests.ps1" -Launcher "$<TARGET_FILE:ve_cli_launcher>"
        -Probe "$<TARGET_FILE:ve_launcher_probe>")
    set_tests_properties(launcher_contracts PROPERTIES TIMEOUT 45)
    add_executable(ve_tests "${VE_TEST_ROOT}/native_tests.cpp")
    target_link_libraries(ve_tests PRIVATE ve_desktop Qt6::Test)
    target_compile_definitions(ve_tests PRIVATE VE_TEST_VIDEO_PREVIEW_CACHE)
    get_filename_component(VE_QT_RUNTIME_ROOT "${Qt6_DIR}/../../.." ABSOLUTE)
    add_test(NAME native_contracts COMMAND $<TARGET_FILE:ve_tests> -o -,txt)
    add_test(NAME windows_chrome COMMAND $<TARGET_FILE:ve_tests> nativeMaximizeAndRestore -o -,txt)
    set_tests_properties(windows_chrome PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=${VE_QT_RUNTIME_ROOT}/plugins"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${VE_QT_RUNTIME_ROOT}/bin"
        TIMEOUT 45)
    set_tests_properties(native_contracts PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${VE_QT_RUNTIME_ROOT}/plugins"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${VE_QT_RUNTIME_ROOT}/bin"
        TIMEOUT 60)
endif()

# tools/deploy.ps1 packages only the runnable application and its resources.
