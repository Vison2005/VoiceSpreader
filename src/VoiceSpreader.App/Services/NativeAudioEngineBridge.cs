using System.Runtime.InteropServices;
using System.Text.Json;
using VoiceSpreader.App.Models;

namespace VoiceSpreader.App.Services;

public interface IRemoteMicrophoneSink
{
    void SetRemoteMicrophoneConnected(bool connected, uint sampleRate = 48_000);

    void AddRemoteMicrophoneClockSample(ulong frameIndex, ulong monotonicNanoseconds);

    void AppendRemoteMicrophonePcm16(ulong firstFrameIndex, uint sampleRate, short[] samples);
}

public sealed record DeviceEnumerationResult(IReadOnlyList<AudioEndpoint> Devices, string? Error);

public sealed record OutputEngineSettings(AudioEndpoint Device, int VolumePercent, int DelayMilliseconds);

public sealed record EngineConfiguration(
    AudioEndpoint Capture,
    IReadOnlyList<OutputEngineSettings> Outputs,
    int BufferMilliseconds,
    bool ExclusiveMode,
    bool AutomaticLatencyCompensation,
    AudioEndpoint? Microphone,
    bool ContinuousAcousticTracking,
    bool UseRemoteMicrophone);

public sealed record CalibrationConfiguration(AudioEndpoint Microphone, IReadOnlyList<AudioEndpoint> Outputs);

public sealed record CalibrationResult(
    AudioEndpoint Device,
    bool Detected,
    double MeasuredLatencyMilliseconds,
    double Confidence,
    int RecommendedDelayMilliseconds);

public sealed class ProgramLevelEventArgs(double levelDbfs, bool probeAllowed) : EventArgs
{
    public double LevelDbfs { get; } = levelDbfs;

    public bool ProbeAllowed { get; } = probeAllowed;
}

public enum CalibrationOutcome
{
    Completed,
    Cancelled,
    Failed,
}

public sealed class CalibrationCompletedEventArgs(
    IReadOnlyList<CalibrationResult> results,
    CalibrationOutcome outcome) : EventArgs
{
    public IReadOnlyList<CalibrationResult> Results { get; } = results;

    public CalibrationOutcome Outcome { get; } = outcome;
}

public sealed class AcousticCorrectionEventArgs(
    string deviceId,
    int delayMilliseconds,
    double driftPpm,
    string probeMode,
    double confidence) : EventArgs
{
    public string DeviceId { get; } = deviceId;

    public int DelayMilliseconds { get; } = delayMilliseconds;

    public double DriftPpm { get; } = driftPpm;

    public string ProbeMode { get; } = probeMode;

    public double Confidence { get; } = confidence;
}

public sealed class NativeAudioEngineBridge : IDisposable, IRemoteMicrophoneSink
{
    private const string NativeLibrary = "VoiceSpreader.Native";
    private static readonly JsonSerializerOptions SerializerOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
    };

    private readonly TextCallback _statusCallback;
    private readonly TextCallback _errorCallback;
    private readonly StateCallback _runningCallback;
    private readonly LevelCallback _levelCallback;
    private readonly CalibrationCallback _calibrationCallback;
    private readonly AcousticCorrectionCallback _acousticCorrectionCallback;
    private nint _handle;
    private bool _disposed;

    public NativeAudioEngineBridge()
    {
        _statusCallback = OnStatus;
        _errorCallback = OnError;
        _runningCallback = OnRunning;
        _levelCallback = OnLevel;
        _calibrationCallback = OnCalibration;
        _acousticCorrectionCallback = OnAcousticCorrection;

        try
        {
            _handle = VS_Create(
                _statusCallback,
                _errorCallback,
                _runningCallback,
                _levelCallback,
                _calibrationCallback,
                _acousticCorrectionCallback,
                nint.Zero);
            IsAvailable = _handle != nint.Zero;
            if (!IsAvailable)
            {
                AvailabilityError = "原生音频引擎初始化失败。";
            }
        }
        catch (Exception exception) when (exception is DllNotFoundException or BadImageFormatException or EntryPointNotFoundException)
        {
            AvailabilityError = $"无法加载原生音频引擎：{exception.Message}";
        }
    }

    public event EventHandler<string>? StatusChanged;

    public event EventHandler<string>? ErrorOccurred;

    public event EventHandler<bool>? RunningChanged;

    public event EventHandler<ProgramLevelEventArgs>? ProgramLevelChanged;

    public event EventHandler<CalibrationCompletedEventArgs>? CalibrationCompleted;

    public event EventHandler<AcousticCorrectionEventArgs>? AcousticCorrectionChanged;

    public bool IsAvailable { get; }

    public string? AvailabilityError { get; }

    public bool IsActive => IsAvailable && VS_IsActive(_handle) != 0;

    public bool IsCalibrating => IsAvailable && VS_IsCalibrating(_handle) != 0;

    public static DeviceEnumerationResult GetRenderDevices() => Enumerate(VS_GetRenderDevicesJson);

    public static DeviceEnumerationResult GetCaptureDevices() => Enumerate(VS_GetCaptureDevicesJson);

    public bool Start(EngineConfiguration configuration)
    {
        EnsureAvailable();
        var payload = new
        {
            capture = configuration.Capture,
            outputs = configuration.Outputs,
            configuration.BufferMilliseconds,
            configuration.ExclusiveMode,
            configuration.AutomaticLatencyCompensation,
            microphone = configuration.Microphone,
            configuration.ContinuousAcousticTracking,
            configuration.UseRemoteMicrophone,
        };
        return VS_Start(_handle, JsonSerializer.Serialize(payload, SerializerOptions)) != 0;
    }

    public void Stop()
    {
        if (IsAvailable)
        {
            VS_Stop(_handle);
        }
    }

    public void SetOutputVolume(string deviceId, int volumePercent)
    {
        if (IsAvailable)
        {
            VS_SetOutputVolume(_handle, deviceId, volumePercent);
        }
    }

    public void SetOutputDelay(string deviceId, int delayMilliseconds)
    {
        if (IsAvailable)
        {
            VS_SetOutputDelay(_handle, deviceId, delayMilliseconds);
        }
    }

    public void SetSynchronizationMargin(int marginMilliseconds)
    {
        if (IsAvailable)
        {
            VS_SetSynchronizationMargin(_handle, marginMilliseconds);
        }
    }

    public bool StartCalibration(CalibrationConfiguration configuration)
    {
        EnsureAvailable();
        return VS_StartCalibration(
                   _handle,
                   JsonSerializer.Serialize(configuration, SerializerOptions))
               != 0;
    }

    public void StopCalibration()
    {
        if (IsAvailable)
        {
            VS_StopCalibration(_handle);
        }
    }

    public void SetRemoteMicrophoneConnected(bool connected, uint sampleRate = 48_000)
    {
        if (IsAvailable)
        {
            VS_SetRemoteMicrophoneConnected(_handle, connected ? 1 : 0, connected ? sampleRate : 0);
        }
    }

    public void AddRemoteMicrophoneClockSample(ulong frameIndex, ulong monotonicNanoseconds)
    {
        if (IsAvailable)
        {
            VS_AddRemoteMicrophoneClockSample(_handle, frameIndex, monotonicNanoseconds);
        }
    }

    public void AppendRemoteMicrophonePcm16(ulong firstFrameIndex, uint sampleRate, short[] samples)
    {
        ArgumentNullException.ThrowIfNull(samples);
        if (IsAvailable && samples.Length > 0)
        {
            VS_AppendRemoteMicrophonePcm16(
                _handle,
                firstFrameIndex,
                sampleRate,
                samples,
                samples.Length);
        }
    }

    private static unsafe DeviceEnumerationResult Enumerate(JsonBufferCallback callback)
    {
        var required = callback(nint.Zero, 0);
        if (required <= 1)
        {
            return new DeviceEnumerationResult([], "原生音频引擎没有返回设备数据。");
        }

        var buffer = new char[required];
        fixed (char* pointer = buffer)
        {
            callback((nint)pointer, buffer.Length);
        }
        var response = JsonSerializer.Deserialize<DeviceResponse>(
            new string(buffer, 0, required - 1),
            SerializerOptions);
        return new DeviceEnumerationResult(
            response?.Devices ?? [],
            string.IsNullOrWhiteSpace(response?.Error) ? null : response.Error);
    }

    private void EnsureAvailable()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!IsAvailable)
        {
            throw new InvalidOperationException(AvailabilityError ?? "原生音频引擎不可用。");
        }
    }

    private void OnStatus(nint context, nint message) =>
        StatusChanged?.Invoke(this, Marshal.PtrToStringUni(message) ?? string.Empty);

    private void OnError(nint context, nint message) =>
        ErrorOccurred?.Invoke(this, Marshal.PtrToStringUni(message) ?? "未知音频错误");

    private void OnRunning(nint context, int active) => RunningChanged?.Invoke(this, active != 0);

    private void OnLevel(nint context, double levelDbfs, int probeAllowed) =>
        ProgramLevelChanged?.Invoke(this, new ProgramLevelEventArgs(levelDbfs, probeAllowed != 0));

    private void OnCalibration(nint context, nint resultsJson)
    {
        var json = Marshal.PtrToStringUni(resultsJson);
        var response = string.IsNullOrWhiteSpace(json)
            ? null
            : JsonSerializer.Deserialize<CalibrationResponse>(json, SerializerOptions);
        var outcome = response?.Outcome?.ToLowerInvariant() switch
        {
            "cancelled" => CalibrationOutcome.Cancelled,
            "failed" => CalibrationOutcome.Failed,
            _ => CalibrationOutcome.Completed,
        };
        CalibrationCompleted?.Invoke(
            this,
            new CalibrationCompletedEventArgs(response?.Results ?? [], outcome));
    }

    private void OnAcousticCorrection(
        nint context,
        nint deviceId,
        int delayMilliseconds,
        double driftPpm,
        nint probeMode,
        double confidence) =>
        AcousticCorrectionChanged?.Invoke(
            this,
            new AcousticCorrectionEventArgs(
                Marshal.PtrToStringUni(deviceId) ?? string.Empty,
                delayMilliseconds,
                driftPpm,
                Marshal.PtrToStringUni(probeMode) ?? string.Empty,
                confidence));

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        if (_handle != nint.Zero)
        {
            VS_Destroy(_handle);
            _handle = nint.Zero;
        }
    }

    private sealed record DeviceResponse(IReadOnlyList<AudioEndpoint> Devices, string? Error);

    private sealed record CalibrationResponse(
        IReadOnlyList<CalibrationResult> Results,
        string? Outcome);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void TextCallback(nint context, nint message);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void StateCallback(nint context, int active);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void LevelCallback(nint context, double levelDbfs, int probeAllowed);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void CalibrationCallback(nint context, nint resultsJson);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void AcousticCorrectionCallback(
        nint context,
        nint deviceId,
        int delayMilliseconds,
        double driftPpm,
        nint probeMode,
        double confidence);

    private delegate int JsonBufferCallback(nint buffer, int capacity);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl)]
    private static extern nint VS_Create(
        TextCallback statusCallback,
        TextCallback errorCallback,
        StateCallback runningCallback,
        LevelCallback levelCallback,
        CalibrationCallback calibrationCallback,
        AcousticCorrectionCallback acousticCorrectionCallback,
        nint context);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl)]
    private static extern void VS_Destroy(nint handle);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private static extern int VS_GetRenderDevicesJson(nint buffer, int capacity);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private static extern int VS_GetCaptureDevicesJson(nint buffer, int capacity);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private static extern int VS_Start(nint handle, string configurationJson);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl)]
    private static extern void VS_Stop(nint handle);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl)]
    private static extern int VS_IsActive(nint handle);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private static extern void VS_SetOutputVolume(nint handle, string deviceId, int volumePercent);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private static extern void VS_SetOutputDelay(nint handle, string deviceId, int delayMilliseconds);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl)]
    private static extern void VS_SetSynchronizationMargin(nint handle, int marginMilliseconds);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private static extern int VS_StartCalibration(nint handle, string configurationJson);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl)]
    private static extern void VS_StopCalibration(nint handle);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl)]
    private static extern int VS_IsCalibrating(nint handle);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl)]
    private static extern void VS_SetRemoteMicrophoneConnected(nint handle, int connected, uint sampleRate);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl)]
    private static extern void VS_AddRemoteMicrophoneClockSample(
        nint handle,
        ulong frameIndex,
        ulong monotonicNanoseconds);

    [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl)]
    private static extern void VS_AppendRemoteMicrophonePcm16(
        nint handle,
        ulong firstFrameIndex,
        uint sampleRate,
        [In] short[] samples,
        int sampleCount);
}
