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

VS_NATIVE_API void* __cdecl VS_Create(VsTextCallback statusCallback,
                                      VsTextCallback errorCallback,
                                      VsStateCallback runningCallback,
                                      VsLevelCallback levelCallback,
                                      VsCalibrationCallback calibrationCallback,
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
