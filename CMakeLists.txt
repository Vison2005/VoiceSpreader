cmake_minimum_required(VERSION 3.20)

project(VoiceSpreader VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets)

set(VOICESPREADER_SOURCES
    src/main.cpp
    src/audio_device.h
    src/wasapi_helpers.cpp
    src/wasapi_helpers.h
    src/wasapi_device_manager.cpp
    src/wasapi_device_manager.h
    src/frame_ring_buffer.cpp
    src/frame_ring_buffer.h
    src/calibration_signal.cpp
    src/calibration_signal.h
    src/latency_calibrator.cpp
    src/latency_calibrator.h
    src/acoustic_drift_tracker.cpp
    src/acoustic_drift_tracker.h
    src/output_worker.cpp
    src/output_worker.h
    src/audio_engine.cpp
    src/audio_engine.h
    src/main_window.cpp
    src/main_window.h
)

add_executable(VoiceSpreader WIN32 ${VOICESPREADER_SOURCES})

target_compile_definitions(VoiceSpreader PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(VoiceSpreader PRIVATE /W4 /permissive- /utf-8)
endif()

target_link_libraries(VoiceSpreader PRIVATE
    Qt5::Widgets
    ole32
    propsys
    avrt
)

install(TARGETS VoiceSpreader RUNTIME DESTINATION .)

if(BUILD_TESTING)
    add_executable(FrameRingBufferTest
        tests/frame_ring_buffer_test.cpp
        src/frame_ring_buffer.cpp
        src/frame_ring_buffer.h
    )
    target_compile_definitions(FrameRingBufferTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(FrameRingBufferTest PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME FrameRingBufferTest COMMAND FrameRingBufferTest)

    add_executable(CalibrationSignalTest
        tests/calibration_signal_test.cpp
        src/calibration_signal.cpp
        src/calibration_signal.h
    )
    if(MSVC)
        target_compile_options(CalibrationSignalTest PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME CalibrationSignalTest COMMAND CalibrationSignalTest)
endif()
