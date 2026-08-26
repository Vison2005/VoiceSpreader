# Windows 多设备音频同步评估

## 结论

Windows 当前没有把多块独立声卡组合成同一硬件时钟域的通用用户态 API。每个 USB、蓝牙或板载音频端点仍可能使用独立晶振，因此仅仅“同时调用播放”不能消除长期漂移。

VoiceSpreader v1.1.0 继续使用每端点独立 WASAPI 流，并在应用层完成缓冲水位控制、重采样和声学校准。这比改用 `AudioGraph` 更适合多设备同步：`AudioGraph.CreateDeviceOutputNodeAsync` 的设备输出节点使用该图的 `PrimaryRenderDevice`，不能在一个图中把多个独立端点自动锁到同一时钟。

## 已采用的 Windows 接口

- `IAudioClient3`：查询端点支持的共享模式周期，并优先请求最小周期。
- WASAPI 事件驱动流：每个输出由自己的事件和实时线程驱动。
- `IAudioClock`：采集设备位置与 QPC 的相关样本，估计端点时钟漂移。
- MMCSS：提高输出线程的调度优先级，减少系统负载造成的抖动。
- WASAPI Loopback：从选定渲染端点捕获系统声音。

## 未直接采用的接口

`IAudioClockAdjustment` 可以在带 `AUDCLNT_STREAMFLAGS_RATEADJUST` 的共享模式流上改变采样率，但它依赖 Windows 音频引擎的重采样，而且微软明确要求不能从实时处理线程调用。VoiceSpreader 已经需要在不同通道数、格式和采样率之间转换，并使用高质量多相重采样器闭环控制各端点缓冲；同时启用两套速率调整会形成相互干扰的控制环，因此当前不作为默认路径。

`IAudioClock2` 可以直接读取共享模式端点的硬件帧位置，适合后续增加更细的诊断数据。但硬件帧率可能不同于客户端混音格式，不能在没有端点标定的情况下直接替代现有缓冲与声学闭环。

## 下一阶段

1. 增加端点热插拔通知和按设备 ID 自动恢复。
2. 记录 `IAudioClock2` 硬件位置，用于分析驱动层时钟质量，不直接驱动实时控制环。
3. 增加多输出启动屏障，缩小第一次非静音块的调度分散。
4. 继续以声学探针校正驱动、无线传输、扬声器和空间传播的总延迟。

## 微软文档

- [IAudioClient3](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioclient3)
- [IAudioClock](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioclock)
- [IAudioClock2::GetDevicePosition](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock2-getdeviceposition)
- [IAudioClockAdjustment](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioclockadjustment)
- [WASAPI 流标志](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audclnt-streamflags-xxx-constants)
- [AudioGraph 设备输出节点](https://learn.microsoft.com/en-us/uwp/api/windows.media.audio.audiograph.createdeviceoutputnodeasync)
