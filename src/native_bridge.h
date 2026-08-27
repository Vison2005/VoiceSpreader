#pragma once

#include <cstdint>

#ifdef VOICESPREADER_NATIVE_EXPORTS
#define VS_NATIVE_API extern "C" __declspec(dllexport)
#else
#define VS_NATIVE_API extern "C" __declspec(dllimport)
#endif

using VsTextCallback = void(__cdecl*)(void* context, const wchar_t* message);
using VsStateCallback = void(__cdecl*)(void* context, int active);
using VsLevelCallback = void(__cdecl*)(void* context, double levelDbfs, int probeAllowed);
using VsCalibrationCallback = void(__cdecl*)(void* context, const wchar_t* resultsJson);
using VsAcousticCorrectionCallback = void(__cdecl*)(void* context,
                                                    const wchar_t* deviceId,
                                                    int delayMilliseconds,
                                                    double driftPpm,
                                                    const wchar_t* probeMode,
                                                    double confidence);
using VsSystemAudioPcmCallback = void(__cdecl*)(void* context,
                                               std::uint64_t firstFrameIndex,
                                               std::uint32_t sampleRate,
                                               std::uint16_t channels,
                                               const std::int16_t* samples,
                                               int sampleCount);

VS_NATIVE_API void* __cdecl VS_Create(VsTextCallback statusCallback,
                                      VsTextCallback errorCallback,
                                      VsStateCallback runningCallback,
                                      VsLevelCallback levelCallback,
                                      VsCalibrationCallback calibrationCallback,
                                      VsAcousticCorrectionCallback acousticCorrectionCallback,
                                      void* context);
VS_NATIVE_API void __cdecl VS_Destroy(void* handle);

VS_NATIVE_API int __cdecl VS_GetRenderDevicesJson(wchar_t* buffer, int capacity);
VS_NATIVE_API int __cdecl VS_GetCaptureDevicesJson(wchar_t* buffer, int capacity);

VS_NATIVE_API int __cdecl VS_Start(void* handle, const wchar_t* configurationJson);
VS_NATIVE_API void __cdecl VS_Stop(void* handle);
VS_NATIVE_API int __cdecl VS_IsActive(void* handle);
VS_NATIVE_API void __cdecl VS_SetOutputVolume(void* handle,
                                              const wchar_t* deviceId,
                                              int volumePercent);
VS_NATIVE_API void __cdecl VS_SetOutputDelay(void* handle,
                                             const wchar_t* deviceId,
                                             int delayMilliseconds);
VS_NATIVE_API void __cdecl VS_SetSynchronizationMargin(void* handle,
                                                       int marginMilliseconds);

VS_NATIVE_API int __cdecl VS_StartCalibration(void* handle,
                                              const wchar_t* configurationJson);
VS_NATIVE_API void __cdecl VS_StopCalibration(void* handle);
VS_NATIVE_API int __cdecl VS_IsCalibrating(void* handle);

VS_NATIVE_API void __cdecl VS_SetRemoteMicrophoneConnected(void* handle,
                                                           int connected,
                                                           std::uint32_t sampleRate);
VS_NATIVE_API void __cdecl VS_AddRemoteMicrophoneClockSample(
    void* handle,
    std::uint64_t frameIndex,
    std::uint64_t monotonicNanoseconds);
VS_NATIVE_API void __cdecl VS_AppendRemoteMicrophonePcm16(
    void* handle,
    std::uint64_t firstFrameIndex,
    std::uint32_t sampleRate,
    const std::int16_t* samples,
    int sampleCount);
VS_NATIVE_API int __cdecl VS_StartRemoteMicrophoneOutput(
    void* handle,
    const wchar_t* deviceId,
    const wchar_t* deviceName,
    std::uint32_t sampleRate,
    int bufferMilliseconds,
    int volumePercent);
VS_NATIVE_API void __cdecl VS_StopRemoteMicrophoneOutput(void* handle);
VS_NATIVE_API int __cdecl VS_IsRemoteMicrophoneOutputActive(void* handle);
VS_NATIVE_API void __cdecl VS_SetRemoteMicrophoneOutputVolume(void* handle,
                                                              int volumePercent);

VS_NATIVE_API int __cdecl VS_StartSystemAudioCapture(
    void* handle,
    const wchar_t* deviceId,
    const wchar_t* deviceName,
    int volumePercent,
    VsSystemAudioPcmCallback pcmCallback,
    void* callbackContext);
VS_NATIVE_API void __cdecl VS_StopSystemAudioCapture(void* handle);
VS_NATIVE_API int __cdecl VS_IsSystemAudioCaptureActive(void* handle);
VS_NATIVE_API void __cdecl VS_SetSystemAudioCaptureVolume(void* handle,
                                                          int volumePercent);
