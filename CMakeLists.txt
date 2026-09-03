cmake_minimum_required(VERSION 3.21)

project(WebcamPerspectiveCorrection VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "WebcamPerspectiveCorrection requires Windows 11 or later")
endif()

find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc highgui videoio)

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(SPOUT_BUILD_CMT OFF CACHE BOOL "Use the same dynamic MSVC runtime as the application" FORCE)
set(SPOUT_BUILD_LIBRARY OFF CACHE BOOL "SpoutLibrary DLL is not required" FORCE)
set(SPOUT_BUILD_SPOUTDX ON CACHE BOOL "Build the Direct3D 11 Spout sender" FORCE)
set(SPOUT_NO_GLOBAL_WARNING ON CACHE BOOL "Keep third-party warnings local" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "Do not install downloaded Spout system-wide" FORCE)
set(SPOUT2_URL https://github.com/leadedge/Spout2/archive/refs/tags/2.007.017.zip)
set(SPOUT2_URL_HASH SHA256=1CC0C958EE14AF614744AC40C054C2FD7DF17F8C5B94EB8ECFEACB37F5B06460)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    FetchContent_Declare(
        Spout2
        URL ${SPOUT2_URL}
        URL_HASH ${SPOUT2_URL_HASH}
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(Spout2)
else()
    FetchContent_Declare(
        Spout2
        URL ${SPOUT2_URL}
        URL_HASH ${SPOUT2_URL_HASH}
    )
    FetchContent_GetProperties(Spout2)
    if(NOT spout2_POPULATED)
        FetchContent_Populate(Spout2)
    endif()
    if(NOT TARGET SpoutDX_static)
        add_subdirectory("${spout2_SOURCE_DIR}" "${spout2_BINARY_DIR}" EXCLUDE_FROM_ALL)
    endif()
endif()

if(MSVC)
    target_compile_options(Spout_static PRIVATE /EHsc)
    target_compile_options(SpoutDX_static PRIVATE /EHsc)
endif()

add_executable(WebcamPerspectiveCorrection
    src/main.cpp
    src/Application.cpp
    src/CameraManager.cpp
    src/CameraDeviceEnumerator.cpp
    src/FrameProcessor.cpp
    src/OutputManager.cpp
    src/PerspectiveCorrector.cpp
    src/RecordingOutput.cpp
    src/SettingsManager.cpp
    src/SpoutOutput.cpp
    src/VirtualCameraOutput.cpp
)

target_include_directories(WebcamPerspectiveCorrection
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(WebcamPerspectiveCorrection PRIVATE
    ${OpenCV_LIBS}
    mf
    mfplat
    mfreadwrite
    mfuuid
    mfsensorgroup
    ole32
    advapi32
    shell32
    SpoutDX_static
)
target_compile_features(WebcamPerspectiveCorrection PRIVATE cxx_std_17)

add_library(PerspectiveCameraMediaSource SHARED
    virtual_camera/MediaSource.cpp
    virtual_camera/MediaSourceDll.cpp
    virtual_camera/SharedFrameReader.cpp
    virtual_camera/PerspectiveCameraMediaSource.def
)

target_include_directories(PerspectiveCameraMediaSource PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/virtual_camera"
)
target_link_libraries(PerspectiveCameraMediaSource PRIVATE
    mf
    mfplat
    mfuuid
    mfsensorgroup
    ole32
    advapi32
)
target_compile_features(PerspectiveCameraMediaSource PRIVATE cxx_std_17)
set_target_properties(PerspectiveCameraMediaSource PROPERTIES
    PREFIX ""
    OUTPUT_NAME "PerspectiveCameraMediaSource"
)

add_dependencies(WebcamPerspectiveCorrection PerspectiveCameraMediaSource)
add_custom_command(TARGET WebcamPerspectiveCorrection POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:PerspectiveCameraMediaSource>"
        "$<TARGET_FILE_DIR:WebcamPerspectiveCorrection>"
)

enable_testing()
add_executable(PerspectiveCameraMediaSourceSmokeTest
    tests/MediaSourceSmokeTest.cpp
)
target_include_directories(PerspectiveCameraMediaSourceSmokeTest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(PerspectiveCameraMediaSourceSmokeTest PRIVATE
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
)
target_compile_features(PerspectiveCameraMediaSourceSmokeTest PRIVATE cxx_std_17)
add_dependencies(PerspectiveCameraMediaSourceSmokeTest PerspectiveCameraMediaSource)
add_test(NAME PerspectiveCameraMediaSourceSmokeTest
    COMMAND PerspectiveCameraMediaSourceSmokeTest "$<TARGET_FILE:PerspectiveCameraMediaSource>"
)
set_tests_properties(PerspectiveCameraMediaSourceSmokeTest PROPERTIES TIMEOUT 15)

add_executable(VideoOutputsSmokeTest
    tests/OutputSmokeTest.cpp
    src/RecordingOutput.cpp
    src/SpoutOutput.cpp
)
target_include_directories(VideoOutputsSmokeTest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(VideoOutputsSmokeTest PRIVATE
    ${OpenCV_LIBS}
    SpoutDX_static
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
)
target_compile_features(VideoOutputsSmokeTest PRIVATE cxx_std_17)
add_test(NAME VideoOutputsSmokeTest
    COMMAND VideoOutputsSmokeTest "$<TARGET_FILE_DIR:VideoOutputsSmokeTest>"
)
set_tests_properties(VideoOutputsSmokeTest PROPERTIES TIMEOUT 30)

if(MSVC)
    target_compile_definitions(WebcamPerspectiveCorrection PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
    )
    target_compile_options(WebcamPerspectiveCorrection PRIVATE
        /EHsc
        /W4
        /permissive-
        /utf-8
        "$<$<CONFIG:Release>:/O2>"
    )

    target_compile_definitions(PerspectiveCameraMediaSource PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
    )
    target_compile_options(PerspectiveCameraMediaSource PRIVATE
        /EHsc
        /W4
        /permissive-
        /utf-8
        "$<$<CONFIG:Release>:/O2>"
    )
    target_compile_definitions(PerspectiveCameraMediaSourceSmokeTest PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
    )
    target_compile_options(PerspectiveCameraMediaSourceSmokeTest PRIVATE
        /EHsc
        /W4
        /permissive-
        /utf-8
        "$<$<CONFIG:Release>:/O2>"
    )
    target_compile_definitions(VideoOutputsSmokeTest PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
    )
    target_compile_options(VideoOutputsSmokeTest PRIVATE
        /EHsc
        /W4
        /permissive-
        /utf-8
        "$<$<CONFIG:Release>:/O2>"
    )

    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        PROPERTY VS_STARTUP_PROJECT WebcamPerspectiveCorrection)
    set_target_properties(WebcamPerspectiveCorrection PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
