# VoiceSpreader 当前实现说明

本文档记录当前桌面端和 Android 端已经实现的功能、音频同步方法、测试结果以及仍然存在的限制。

## 一句话结论

VoiceSpreader 当前采用“统一数字音频源 + 设备硬件时钟观测 + ASRC 微调 + 声学残差校准”的分层同步方案。

主同步链路不是单纯依靠声学检测，而是先用数字时钟和缓冲控制保证长期稳定，再用声学探针补偿扬声器、空气传播和麦克风路径造成的端到端差异。

## 已实现功能

### Windows 桌面端

- 枚举 Windows 中可用的渲染设备和录音设备。
- 使用 WASAPI Loopback 捕获系统正在播放的音频。
- 将同一音频源同时分发到多个输出设备。
- 每个输出设备使用独立的事件驱动 WASAPI 播放线程和环形缓冲区。
- 输出模式按优先级尝试：
  - WASAPI 独占低延迟模式；
  - `IAudioClient3` 共享低延迟模式；
  - 普通共享模式回退。
- 输出端支持独立音量、基础同步余量、手动延迟和自动声学延迟补偿。
- 支持实时电平显示、设备状态、欠载统计和同步状态显示。
- 支持托盘后台运行、最小化到托盘和开机自启动。
- 支持 Android 手机配对、手机麦克风回传和连接状态控制。
- 支持通过 Windows Bluetooth `AudioPlaybackConnection` 接收移动设备系统音频。

### Android 端

- 支持二维码或配对码连接 Windows 桌面端。
- 使用前台服务保持连接和麦克风回传。
- 使用 `AudioRecord` 采集单声道 PCM16 音频，目标采样率为 48 kHz。
- 以连续采样帧序号传输音频，桌面端可处理网络抖动、重复帧和缺帧。
- Android 7.0 及以上定期采集 `AudioTimestamp`，将手机录音硬件时钟样本发送到桌面端。
- 支持手机端启用/停止麦克风，并在异常或停止时释放录音设备。

## 音频同步使用的方法

### 1. 统一数字源时间线

桌面端先从一个系统播放设备获取 WASAPI Loopback 音频，再把同一批采样分发给各个输出设备。这样各输出设备接收到的是同一数字源，而不是分别播放各自独立的音频流。

每个输出设备拥有自己的环形缓冲区和播放线程，输出线程通过 WASAPI 事件驱动持续填充可写缓冲区。

相关实现：

- `src/audio_engine.cpp`
- `src/output_worker.cpp`
- `src/frame_ring_buffer.cpp`

### 2. WASAPI 设备时钟观测

输出流启动后，程序尝试取得 `IAudioClock`，周期性读取：

- 输出设备累计播放帧位置；
- 与之对应的 QPC 计时器位置。

程序用滑动窗口拟合“设备帧位置—QPC 时间”的关系，估计设备真实采样率和相对于 nominal sample rate 的漂移 ppm。

`AudioClockModel` 使用以下保护措施：

- 只接受帧位置和 QPC 均单调递增的样本；
- 至少积累 3 个样本后才认为模型有效；
- 先使用 Theil-Sen 估计初始斜率；
- 再使用 Huber 权重降低异常时间戳对结果的影响；
- 限制异常模型对实时比率的影响范围。

相关实现：

- `src/audio_clock_model.h`
- `src/audio_clock_model.cpp`
- `src/output_worker.cpp`

### 3. ASRC 音频速率微调

输出端的输入采样率和输出设备采样率不一致时，使用自适应采样率转换（ASRC）。当前 ASRC 包含两层控制：

1. 设备时钟前馈修正：根据 `IAudioClock` 测得的漂移 ppm 微调输入消耗比率。
2. 缓冲水位闭环：根据环形缓冲区距离目标水位的误差，缓慢提高或降低输入消耗速度。

缓冲闭环在大幅水位偏差时允许较快追赶，接近目标后切换到低带宽慢速修正，避免频繁丢帧、补帧和明显音高变化。

### 4. 窗口化多相重采样

原先的线性插值已经替换为窗口化 sinc 多相重采样器：

- 16-tap FIR 窗口；
- 256 个分数延迟相位；
- 每个相位预计算归一化系数；
- 保留滤波器历史帧，避免环形缓冲压缩后产生边界突变。

相关实现：

- `src/polyphase_resampler.h`
- `src/polyphase_resampler.cpp`

### 5. 声学端到端校准

声学校准测量的是完整路径：

```text
程序输出 → 声卡/驱动 → 扬声器 → 空气传播 → 麦克风 → 录音输入
```

当前支持两类探针：

- 近超声扫频探针，约 20.4–22 kHz；
- 低电平扩频探针，约 15–15.8 kHz。

检测流程为：

1. 对录音和参考探针做可选 FIR 带通；
2. 将信号降采样到约 16 kHz 以降低计算量；
3. 使用 FFT 计算 GCC-PHAT 互谱相关；
4. 在预期时间窗口内寻找相关峰；
5. 对峰值左右三个点做抛物线拟合，得到亚采样级延迟；
6. 使用归一化时域相关重新计算置信度；
7. 每台设备保留最近 3 次有效结果，并用中位数降低偶发反射和噪声影响。

声学校准得到的是相对延迟，因此多个输出设备可以通过“较慢设备为参考”建立初始补偿，然后在运行中以小步长继续跟踪变化。

相关实现：

- `src/calibration_signal.cpp`
- `src/acoustic_drift_tracker.cpp`
- `src/latency_calibrator.cpp`

### 6. Android 远端时钟同步

Android 端除了发送 PCM 和连续帧序号，还会定期发送 `AudioTimestamp` 的单调时钟样本。桌面端使用相同的时钟模型估计手机实际采样率，并在远程麦克风快照中使用估计值，降低手机声卡长期采样率漂移造成的声学延迟累计误差。

相关实现：

- Android：`app/src/main/java/com/voicespreader/remote/AudioStreamer.kt`
- Windows：`src/phone_pairing_server.cpp`
- Windows：`src/remote_microphone_buffer.cpp`

## 同步链路示意

```text
WASAPI Loopback
      │
      ▼
统一数字源时间线
      │
      ├── 输出设备 A：IAudioClock + 缓冲 PLL + 多相 ASRC
      ├── 输出设备 B：IAudioClock + 缓冲 PLL + 多相 ASRC
      └── Android 手机：PCM 帧序号 + AudioTimestamp

声学探针 ──► 麦克风录音 ──► FFT/GCC-PHAT ──► 亚采样延迟
                                      │
                                      ▼
                         每台设备的端到端延迟补偿
```

## 测试和构建状态

### Windows

桌面端 Release 构建成功，CTest 当前共有 6 个测试，全部通过：

- `FrameRingBufferTest`
- `CalibrationSignalTest`
- `PairingQrCodeTest`
- `PhonePairingServerTest`
- `AudioClockModelTest`
- `PolyphaseResamplerTest`

声学测试覆盖：

- 极性反转探针；
- 背景噪声下的探针检测；
- 超声探针；
- 扩频探针；
- 分数采样延迟定位。

### Android

以下任务均构建成功：

- `compileDebugKotlin`
- `testDebugUnitTest`
- `assembleDebug`

当前 Android 交付包是 Debug APK，不是正式发布签名包。

## 当前仍未完成或存在限制的部分

### 虚拟音频驱动

项目尚未包含自研 Windows 虚拟音频驱动。当前仍是用户态模式：先捕获一个实际输出设备，再把音频分发到其他设备。

虚拟驱动可以让 Windows 把 VoiceSpreader 视为统一虚拟扬声器，从结构上减少“源设备自身播放路径”的限制，但需要安装 WDK/WDF、实现 WaveRT/SysVAD 类驱动，并完成签名和安装流程。当前开发机没有 WDK/WDF 构建环境，因此这一部分没有伪造实现。

### 生产发布签名

- Windows 当前是便携版程序，包含运行库，但没有正式安装器和代码签名。
- Android 当前是 Debug APK，需要正式发布时配置 release signing。

### 实机验证

自动化测试使用合成音频和协议模拟，已经验证算法和数据链路，但不同声卡、蓝牙设备、扬声器、房间混响和手机型号仍需要实际设备测试。

### 网络安全

当前手机配对协议使用会话密钥校验，但音频 TCP 流本身未加密，适合可信局域网或个人网络环境。若用于不可信网络，需要增加 TLS 或其他端到端加密层。

## 关键文件索引

| 模块 | 文件 |
|---|---|
| 输出线程和 ASRC | `src/output_worker.cpp` |
| 设备时钟模型 | `src/audio_clock_model.cpp` |
| 多相重采样器 | `src/polyphase_resampler.cpp` |
| 声学探针和 GCC-PHAT | `src/calibration_signal.cpp` |
| 声学漂移跟踪 | `src/acoustic_drift_tracker.cpp` |
| 手机音频协议 | `src/phone_pairing_server.cpp` |
| 手机远端缓冲及时钟 | `src/remote_microphone_buffer.cpp` |
| Android 录音和时钟上报 | `VoiceSpreaderAndroid/app/src/main/java/com/voicespreader/remote/AudioStreamer.kt` |
