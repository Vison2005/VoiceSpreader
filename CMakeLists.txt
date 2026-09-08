cmake_minimum_required(VERSION 3.20)

project(VoiceSpreader VERSION 1.2.4.72 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 5.15 REQUIRED COMPONENTS Core Widgets Network)

set(VOICESPREADER_SOURCES
    src/main.cpp
    src/audio_device.h
    src/wasapi_helpers.cpp
    src/wasapi_helpers.h
    src/wasapi_device_manager.cpp
    src/wasapi_device_manager.h
    src/frame_ring_buffer.cpp
    src/frame_ring_buffer.h
    src/audio_clock_model.cpp
    src/audio_clock_model.h
    src/sampling_rate_offset_estimator.cpp
    src/sampling_rate_offset_estimator.h
    src/polyphase_resampler.cpp
    src/polyphase_resampler.h
    src/calibration_signal.cpp
    src/calibration_signal.h
    src/acoustic_alignment.cpp
    src/acoustic_alignment.h
    src/latency_calibrator.cpp
    src/latency_calibrator.h
    src/acoustic_drift_tracker.cpp
    src/acoustic_drift_tracker.h
    src/remote_microphone_buffer.cpp
    src/remote_microphone_buffer.h
    src/remote_microphone_output.cpp
    src/remote_microphone_output.h
    src/system_audio_capture.cpp
    src/system_audio_capture.h
    src/phone_pairing_server.cpp
    src/phone_pairing_server.h
    src/bluetooth_audio_receiver.cpp
    src/bluetooth_audio_receiver.h
    src/pairing_qr_code.cpp
    src/pairing_qr_code.h
    src/output_worker.cpp
    src/output_worker.h
    src/audio_engine.cpp
    src/audio_engine.h
    src/delay_normalization.h
    src/main_window.cpp
    src/main_window.h
    src/level_meter_widget.cpp
    src/level_meter_widget.h
    resources.qrc
    assets/VoiceSpreader.rc
    assets/VoiceSpreader.manifest
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
    Qt5::Network
    ole32
    propsys
    avrt
    windowsapp
)

install(TARGETS VoiceSpreader RUNTIME DESTINATION .)

set(VOICESPREADER_NATIVE_SOURCES
    src/audio_device.h
    src/wasapi_helpers.cpp
    src/wasapi_helpers.h
    src/wasapi_device_manager.cpp
    src/wasapi_device_manager.h
    src/frame_ring_buffer.cpp
    src/frame_ring_buffer.h
    src/audio_clock_model.cpp
    src/audio_clock_model.h
    src/sampling_rate_offset_estimator.cpp
    src/sampling_rate_offset_estimator.h
    src/polyphase_resampler.cpp
    src/polyphase_resampler.h
    src/calibration_signal.cpp
    src/calibration_signal.h
    src/acoustic_alignment.cpp
    src/acoustic_alignment.h
    src/acoustic_drift_tracker.cpp
    src/acoustic_drift_tracker.h
    src/remote_microphone_buffer.cpp
    src/remote_microphone_buffer.h
    src/remote_microphone_output.cpp
    src/remote_microphone_output.h
    src/system_audio_capture.cpp
    src/system_audio_capture.h
    src/output_worker.cpp
    src/output_worker.h
    src/audio_engine.cpp
    src/audio_engine.h
    src/delay_normalization.h
    src/latency_calibrator.cpp
    src/latency_calibrator.h
    src/native_bridge.cpp
    src/native_bridge.h
)

add_library(VoiceSpreaderNative SHARED ${VOICESPREADER_NATIVE_SOURCES})
set_target_properties(VoiceSpreaderNative PROPERTIES OUTPUT_NAME "VoiceSpreader.Native")
target_include_directories(VoiceSpreaderNative PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(VoiceSpreaderNative PRIVATE
    VOICESPREADER_NATIVE_EXPORTS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)
if(MSVC)
    target_compile_options(VoiceSpreaderNative PRIVATE /W4 /permissive- /utf-8)
endif()
target_link_libraries(VoiceSpreaderNative PRIVATE
    Qt5::Core
    ole32
    propsys
    avrt
    windowsapp
)
install(TARGETS VoiceSpreaderNative RUNTIME DESTINATION .)

if(BUILD_TESTING)
    add_executable(DelayNormalizationTest tests/delay_normalization_test.cpp)
    target_include_directories(DelayNormalizationTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(DelayNormalizationTest PRIVATE /W4 /permissive- /utf-8)
    endif()
    target_link_libraries(DelayNormalizationTest PRIVATE Qt5::Core)
    add_test(NAME DelayNormalizationTest COMMAND DelayNormalizationTest)

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

    add_executable(PairingQrCodeTest
        tests/pairing_qr_code_test.cpp
        src/pairing_qr_code.cpp
        src/pairing_qr_code.h
    )
    if(MSVC)
        target_compile_options(PairingQrCodeTest PRIVATE /W4 /permissive- /utf-8)
    endif()
    target_link_libraries(PairingQrCodeTest PRIVATE Qt5::Widgets)
    add_test(NAME PairingQrCodeTest COMMAND PairingQrCodeTest)

    add_executable(PhonePairingServerTest
        tests/phone_pairing_server_test.cpp
        src/phone_pairing_server.cpp
        src/phone_pairing_server.h
        src/remote_microphone_buffer.cpp
        src/remote_microphone_buffer.h
        src/audio_clock_model.cpp
        src/audio_clock_model.h
    )
    if(MSVC)
        target_compile_options(PhonePairingServerTest PRIVATE /W4 /permissive- /utf-8)
    endif()
    target_link_libraries(PhonePairingServerTest PRIVATE Qt5::Network)
    add_test(NAME PhonePairingServerTest COMMAND PhonePairingServerTest)

    add_executable(RemoteMicrophoneBufferTest
        tests/remote_microphone_buffer_test.cpp
        src/remote_microphone_buffer.cpp
        src/remote_microphone_buffer.h
        src/audio_clock_model.cpp
        src/audio_clock_model.h
    )
    if(MSVC)
        target_compile_options(RemoteMicrophoneBufferTest PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME RemoteMicrophoneBufferTest COMMAND RemoteMicrophoneBufferTest)

    add_executable(AudioClockModelTest
        tests/audio_clock_model_test.cpp
        src/audio_clock_model.cpp
        src/audio_clock_model.h
    )
    if(MSVC)
        target_compile_options(AudioClockModelTest PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME AudioClockModelTest COMMAND AudioClockModelTest)

    add_executable(SamplingRateOffsetEstimatorTest
        tests/sampling_rate_offset_estimator_test.cpp
        src/sampling_rate_offset_estimator.cpp
        src/sampling_rate_offset_estimator.h
    )
    if(MSVC)
        target_compile_options(SamplingRateOffsetEstimatorTest PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME SamplingRateOffsetEstimatorTest COMMAND SamplingRateOffsetEstimatorTest)

    add_executable(AcousticAlignmentTest
        tests/acoustic_alignment_test.cpp
        src/acoustic_alignment.cpp
        src/acoustic_alignment.h
    )
    if(MSVC)
        target_compile_options(AcousticAlignmentTest PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME AcousticAlignmentTest COMMAND AcousticAlignmentTest)

    add_executable(PolyphaseResamplerTest
        tests/polyphase_resampler_test.cpp
        src/polyphase_resampler.cpp
        src/polyphase_resampler.h
    )
    if(MSVC)
        target_compile_options(PolyphaseResamplerTest PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME PolyphaseResamplerTest COMMAND PolyphaseResamplerTest)

    add_executable(NativeBridgeTest tests/native_bridge_test.cpp)
    if(MSVC)
        target_compile_options(NativeBridgeTest PRIVATE /W4 /permissive- /utf-8)
    endif()
    target_link_libraries(NativeBridgeTest PRIVATE VoiceSpreaderNative Qt5::Core)
    add_test(NAME NativeBridgeTest COMMAND NativeBridgeTest)
endif()
