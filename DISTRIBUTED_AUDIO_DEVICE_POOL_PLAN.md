# VoiceSpreader 下一阶段：跨设备双向音频与同步设备池计划

更新时间：2026-08-04  
状态：调研与实施方案，尚未进入功能开发

## 1. 目标与范围

下一阶段把当前的“Windows 多输出 + Android 远程麦克风”扩展为一个跨设备音频池：

- Windows 电脑和 Android 手机都可以成为音频源、播放端、麦克风或会话协调者。
- 同一会话内可以勾选任意已连接播放端，决定哪些设备同时发声。
- 连接可以走普通局域网、Wi-Fi Direct 或公网；公网优先点对点，无法直连时允许中继。
- 电脑音频可以播放到手机，手机拥有的音频也可以播放到电脑。
- 多台电脑、手机及其本地有线、USB、蓝牙设备可加入同一个逻辑设备池。
- 同一地点的播放端追求听感上的同步；不同地点的播放端追求同一媒体时间线上的同步。

本计划不包含：

- 直接实现 Android 脱离 Windows 后同时驱动两个传统蓝牙耳机。它作为再下一阶段的独立课题保留，本阶段不展开实现方案。
- 绕过 DRM、应用的禁止捕获策略或 Android 平台安全边界。
- 发布、商店上架、正式驱动签名和大规模云服务运维。

## 2. 结论先行

整体方案可行，但需要区分两个经常被混在一起的目标：

1. **应用内音频互通**：VoiceSpreader 自己取得 PCM 后，在电脑和手机之间传输并播放。这一层完全可做，也是下一阶段应优先完成的主线。
2. **系统级音频设备**：让“VoiceSpreader 手机”出现在 Windows 或 Android 的系统输出设备列表中，供任意应用选择。这一层受操作系统驱动和权限模型限制。

建议采用以下总体路线：

- 先把 Windows 和 Android 都抽象为相同的 `Node`，完成应用内双向传输、设备池和跨节点同步。
- Windows 端之后接入基于 SysVAD 的虚拟扬声器，使任意 Windows 应用都能把声音送进设备池。
- Android 端可发布 `MediaRouteProvider`，让支持 Android MediaRouter 的应用看到电脑或设备池；普通 APK 不能等价地向 Audio HAL 注入一个全系统虚拟声卡。
- 公网采用 WebRTC 的 ICE/STUN/TURN 体系解决 NAT 穿透和中继，媒体仍由 VoiceSpreader 自己按时间戳调度，不能把 WebRTC 默认“尽快播放”的语音通话逻辑直接当成多房间同步逻辑。
- 同步必须同时解决网络时钟、播放缓冲、声卡采样时钟和真实声学路径，单纯测网络延迟或单纯使用 NTP 都不够。

## 3. 能力与平台边界

| 目标 | 可行性 | 推荐实现 | 主要限制 |
| --- | --- | --- | --- |
| Windows 系统声音播放到手机扬声器 | 高 | WASAPI Loopback/后续虚拟端点 → 时间戳音频流 → Android `AudioTrack` | 当前物理源仍会在原设备发声；虚拟端点阶段可彻底接管 |
| Windows 系统声音播放到手机连接的耳机 | 高 | 手机收到 PCM 后交给 Android 当前媒体路由 | 最终路由和延迟由 Android/蓝牙栈决定 |
| 手机麦克风回传电脑 | 已具备局域网原型 | 升级现有 48 kHz PCM 协议并复用新传输层 | 需要加密、重连、时钟映射和公网穿透 |
| VoiceSpreader 自己播放的手机音频传到电脑 | 高 | 应用直接把解码前或混音后的 PCM/Opus 送入会话 | 只覆盖 VoiceSpreader 拥有的内容 |
| 任意 Android 应用声音传到电脑 | 部分可行 | 用户授权 `MediaProjection` + Audio Playback Capture | 源应用可禁止捕获；部分用途、DRM 内容和系统声音不可捕获 |
| 手机作为 Windows 系统输出设备 | 高，但需要驱动 | SysVAD 派生的虚拟渲染端点 + 用户态网络桥 | 本机测试需要测试签名/Test Mode；正式发布另需签名 |
| 电脑作为 Android 全系统输出设备 | 普通 APK 不完整 | 先做 VoiceSpreader 内部输出；再做 MediaRouter 路由 | MediaRouter 只对采用该框架的应用生效；真正全系统端点属于 Audio HAL/系统镜像层 |
| 手机把一份音频发到多个 VoiceSpreader 节点 | 高 | 源节点单次编码，向多个接收节点发送带同一呈现时间的帧 | 需要上行带宽或可选中继，不应采用全互传网状媒体流 |
| 手机与电脑通过 Wi-Fi Direct 直连 | 高，但需兼容回退 | Wi-Fi Direct 只负责建链，连接后仍运行统一 IP 媒体协议 | 并非所有 Android 设备都支持；Windows 桌面 API 的配对/组所有者行为有约束 |
| 手机音频通过蓝牙播放到 Windows | 可行 | Windows `AudioPlaybackConnection` 启用 A2DP Sink | 是特殊单链路模式，不适合作为整个同步设备池的核心传输 |
| Windows 音频通过蓝牙把 Android 手机当扬声器 | 普通 Android 应用不可依赖 | 使用 Wi-Fi/IP 媒体链路代替 | Android 公共 API 没有把普通手机稳定变成通用 A2DP Sink 的跨厂商方案 |

依据：

- Android 的 [Audio Playback Capture](https://developer.android.com/media/platform/av-capture) 明确要求 `MediaProjection` 授权，并受源应用捕获策略约束。
- Android [Audio HAL](https://source.android.com/docs/core/audio/implement) 才是 AudioFlinger 与底层驱动、路由之间的系统接口，普通应用不在这一层注册硬件端点。
- Android [MediaRouter](https://developer.android.com/media/routing/mediarouter) 和 [MediaRouteProvider](https://developer.android.com/media/routing/mediarouteprovider) 可以给兼容应用发布远程播放路由，但不是无条件截获所有系统音频。
- 微软的 [SysVAD 官方样例](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/sample-audio-drivers) 展示了 WDM/WaveRT 虚拟音频设备，可作为 Windows 虚拟扬声器起点。
- Windows 提供 [AudioPlaybackConnection](https://learn.microsoft.com/en-in/windows/uwp/audio-video-camera/enable-remote-audio-playback) 来启用 A2DP Sink，使手机等蓝牙源能向电脑播放。

## 4. 为什么不把蓝牙作为设备池的主干

经典 A2DP 很适合“一台主机连接一个耳机”，却不是开放给普通应用的任意多节点低延迟媒体总线：

- Android 公共 [`BluetoothA2dp`](https://developer.android.com/reference/android/bluetooth/BluetoothA2dp) 文档仍明确写明只支持一个已连接的 A2DP 设备。
- A2DP、HFP 的音频编码、缓冲和路由主要由系统蓝牙栈控制，应用很难获得可靠的硬件呈现时间，也难以持续细调单个耳机的采样速率。
- Windows 的 [Bluetooth Classic Audio](https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/bluetooth-classic-audio) 也将 A2DP 定义为主机到音频设备的立体声输出；打开麦克风时可能切换 HFP，并改变采样率与延迟。
- LE Audio 的 Multi-Stream 和 Auracast 天生支持多个同步流，但要求手机、系统、控制器和耳机共同支持。Bluetooth SIG 的 [LE Audio 资料](https://www.bluetooth.com/media/le-audio/) 与 [规格索引](https://www.bluetooth.com/learn-about-bluetooth/feature-enhancements/le-audio/le-audio-specifications/) 可作为未来硬件能力分支，而不能当作当前所有设备的基础能力。

因此本阶段把蓝牙设备视为某个 Windows/Android 节点背后的“本地端点”。节点负责测量并补偿它的实际延迟；节点之间统一走 IP 媒体协议。蓝牙仍可用于发现、配对辅助或“手机 → Windows A2DP Sink”的便利模式。

## 5. 推荐架构

```text
                         可选公网协调服务
                    信令 / 设备发现 / TURN 凭据
                                │
             ┌──────────────────┴──────────────────┐
             │                                     │
       Windows Node  ═══════ 加密媒体链路 ═══════ Android Node
       Qt + C++ Core                           Kotlin UI + C++ Core
             │                                     │
     WASAPI / 虚拟端点                    AudioRecord / AudioTrack
             │                                     │
   有线、USB、HDMI、蓝牙                   扬声器、USB、蓝牙
```

### 5.1 节点角色

每台设备都上报能力，而不是写死“电脑是服务端、手机是麦克风”：

- `Source`：产生或捕获媒体，例如 WASAPI Loopback、虚拟扬声器、手机麦克风、VoiceSpreader 内置播放器。
- `Sink`：播放带时间戳的流，例如 WASAPI 输出、Android `AudioTrack`。
- `CalibrationMic`：在指定听音位置观察多个播放端的真实声学到达时间。
- `Coordinator`：维护会话纪元、路由图、公共媒体时间线和成员状态。
- `Relay`：直连失败或源上行不足时转发加密媒体，不参与解码。

同一节点可以同时拥有多个角色。第一版会话限制为“一个活动节目源、多个播放端”，以避免未经定义的混音、回声和环路；多源混音在协议中预留但不立即开放。

### 5.2 控制面与媒体面分离

控制面负责：

- 配对、设备身份、能力协商和权限确认。
- 创建会话、加入/退出、选择源和播放端。
- 时钟测量、开始时间、路由变更、音量和延迟设置。
- 状态统计、故障转移、版本协商。

媒体面负责：

- 传输带流 ID、序号、源帧号和目标呈现时间的 PCM/Opus 数据。
- 乱序恢复、丢包隐藏、解码和时间线缓冲。
- 不允许媒体包反向改变控制权限。

设备池是逻辑上的任意互联，但媒体不应默认构造完整网状拓扑。一个源向多个 Sink 扇出，或在公网经一个转发节点扇出，可把带宽从 `O(n²)` 控制在接近 `O(n)`。

### 5.3 路由图

每条路由定义：

```text
Route {
  sourceNode/sourcePort
  streamId + epoch
  selectedSinks[]
  format/codec/channels
  targetLatencyClass
  volume/mute
  acousticZone
}
```

提交路由前必须做有向环检测。任何会把播放流再次捕获回原源的路径都要拒绝，除非明确进入带回声消除的通话模式。

## 6. 连接与配对方案

### 6.1 局域网

- 保留二维码和六位码的易用入口。
- 二维码不再只写 IP，而是携带协议版本、会话/设备 ID、公钥指纹、一次性配对材料和可选协调服务地址。
- 同网段使用 mDNS/UDP 发现；二维码中的候选地址仅作为快速路径。
- 现有 TCP 麦克风协议先作为兼容传输，逐步迁移到统一的双向会话协议。

### 6.2 Wi-Fi Direct

Android 官方 [Wi-Fi Direct API](https://developer.android.com/develop/connectivity/wifi/wifip2p) 可以在无接入点时发现并连接设备，之后通过普通 socket 传数据。Windows 的 [Wi-Fi Direct API](https://learn.microsoft.com/en-us/windows/win32/nativewifi/using-the-wi-fi-direct-api) 和 [WinRT/组所有者说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/partnerapps/wi-fi-direct) 可用于桌面程序。

实现原则：

1. Wi-Fi Direct 只建立 IP 可达性，不另造一套音频协议。
2. 优先让 Windows 成为 Group Owner，Android 加入后继续运行统一控制/媒体协议。
3. 检测到 Windows Mobile Hotspot、驱动不支持或配对失败时，自动回退普通 WLAN/公网。
4. Android 13+ 申请 `NEARBY_WIFI_DEVICES`，旧系统按官方要求兼容位置权限。

### 6.3 公网点对点

推荐采用 WebRTC 连接体系：

- 信令服务只交换设备在线状态、SDP/ICE candidate 和授权信息，不接触明文音频。
- ICE 先尝试局域地址和公网打洞；对称 NAT、严格防火墙或运营商 CGNAT 下使用 TURN 中继。
- “点对点”只能作为优先路径，不能承诺每次都直连。IETF 的 [ICE RFC 8445](https://www.rfc-editor.org/info/rfc8445/) 和 [WebRTC Transport RFC 8835](https://www.rfc-editor.org/rfc/rfc8835.html) 都把 TURN 作为无法直连时的必要后备。
- 媒体用 SRTP，控制数据通道用 DTLS；[WebRTC 安全架构 RFC 8827](https://www.rfc-editor.org/info/rfc8827/) 要求媒体和数据通道加密。

实现库先做一个技术验证：

- 方案 A：`libdatachannel + libopus`，C++17、Windows/Android 共用核心，容易保留 VoiceSpreader 自己的媒体时间戳和播放缓冲。
- 方案 B：Google libwebrtc + 自定义 Audio Device Module，网络适应能力成熟，但构建体量大；官方 Android 原生构建目前要求 Linux 环境，见 [WebRTC Android development](https://webrtc.googlesource.com/src/+/main/docs/native-code/android/)。

首选 A 做原型。若公网丢包、拥塞控制或移动网络切换的维护成本明显过高，再切换或组合 B。不能因为“性能开销不重要”而忽略拥塞控制：不受控地发送实时 UDP 会影响同网其他流量，也会放大自身排队延迟。

### 6.4 六位码安全升级

六位码不能直接作为长期密钥，也不能把当前明文 PCM 协议暴露到公网。建议用六位码驱动 PAKE，并在成功后保存设备公钥：

- 采用 [SPAKE2 RFC 9382](https://www.rfc-editor.org/info/rfc9382/) 或由成熟库提供的等价 PAKE。
- 配对码只在短时间内有效，有尝试次数限制。
- 首次配对产生长期设备身份；后续重连使用设备密钥和显式撤销列表。
- 二维码可直接携带高熵一次性材料和公钥指纹，仍需在双方 UI 显示设备名与授权方向。

## 7. 多设备同步的四层模型

真正听到的到达时间可近似拆成：

```text
声学到达时间 = 公共媒体时间
             + 网络/解码缓冲
             + 系统音频缓冲
             + DAC/蓝牙/设备内部延迟
             + 扬声器到听音位置的传播时间
```

必须分层测量和控制，避免让一个控制器同时追逐网络抖动与声卡晶振漂移。

### 7.1 第一层：会话时钟

每个节点都只使用单调时钟，并维护仿射映射：

```text
T_session = alpha_i * T_node + beta_i
```

- `beta_i` 是节点相对会话时钟的偏移。
- `alpha_i` 是节点时钟速率差，也就是长期漂移。
- 每轮采用四时间戳请求/响应，优先保留低 RTT 样本，剔除排队造成的高 RTT 离群值。
- 对最近数分钟样本做鲁棒线性回归或二状态 Kalman 滤波，同时估计 offset 与 skew。
- 局域网每 1 秒小批量更新，公网按网络状态放宽到 2–5 秒；更新映射而不跳变本地系统时间。

[Reference-Broadcast Synchronization](https://www.usenix.org/conference/osdi-02/fine-grained-network-time-synchronization-using-reference-broadcasts) 说明了接收者之间比较同一广播到达时间可消除发送端不确定性；[FTSP](https://archive.isis.vanderbilt.edu/node/3600) 展示了周期同步、漂移回归和失效拓扑更新的价值。消费级 Wi-Fi/公网无法复制它们的 MAC 层时间戳精度，但低 RTT 取样、相对时间尺度和持续估计 skew 的思想适用于本项目。

### 7.2 第二层：带呈现时间的媒体缓冲

每个音频包至少携带：

```text
protocolVersion, sessionId, streamId, epoch,
sequence, sourceFrameIndex, sampleRate,
presentationTimeNs, frameCount, codec, flags
```

播放流程：

1. Coordinator 收集所有 Sink 的最低安全缓冲和端点延迟。
2. 选择共同的 `START_AT`，至少晚于所有节点当前时间加安全余量。
3. 每个 Sink 把媒体帧放入按 `sourceFrameIndex` 排列的时间线环形缓冲。
4. 到达本地映射后的 `presentationTimeNs` 才交给硬件时间线。
5. 迟到节点在下一个块边界淡入，不让整个会话反复停顿。

缓冲不是固定一个值：

- LAN 起始目标可为 30–80 ms。
- Wi-Fi Direct 与拥塞 WLAN 根据抖动提高。
- 公网起始目标可为 120–500 ms，优先保证同步而不是交互延迟。
- 以近期网络延迟变化的高分位数调整，增大可以快，减小必须慢。

RFC 7273 指出，没有共享参考时钟时，RTP 可以在单接收端对齐相关流，却不能让经过不同网络路径的多个接收端紧密同步；共享时钟和媒体时钟映射正是跨接收端同步的基础，见 [RTP Clock Source Signalling](https://www.rfc-editor.org/info/rfc7273)。

### 7.3 第三层：硬件播放时钟与连续速率修正

即使两个节点的系统时钟完全一致，独立声卡的晶振也会让播放位置持续漂移。因此每个 Sink 维护第二个模型：

```text
F_device = gamma_i * T_node + delta_i
```

- Windows 使用 [`IAudioClock::GetPosition`](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition) 同时取得设备位置和对应 QPC。
- Android 使用 [`AudioTrack.getTimestamp`](https://developer.android.com/reference/android/media/AudioTrack#getTimestamp(android.media.AudioTimestamp)) 取得播放帧位置与估计呈现时间；路由切换后要等待时间戳重新稳定。
- 正常误差通过每设备异步重采样器连续修正，建议限制在约 ±300 ppm，必要时可短时扩大到 ±1000 ppm。
- 误差过大或路由突变时，不把几百毫秒硬拉回来；在零交叉附近做短淡出、跳过/补入少量帧、淡入，并重新进入锁定状态。
- Windows 共享模式可评估 [`IAudioClockAdjustment`](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioclockadjustment)，但独占模式不支持它，所以现有每设备重采样器仍应作为统一后备。

这层直接复用当前 VoiceSpreader 已有的缓冲水位控制和每输出重采样思想，并把参考从“本机共享 ring 的水位”提升为“会话媒体帧位置”。

### 7.4 第四层：声学闭环

系统 API 能看到硬件缓冲位置，却看不到蓝牙耳机内部缓冲、DSP、DAC 和空气传播。声学反馈用于慢速校正这部分不可观测误差：

- 同一房间只需一支放在听音位置的手机麦克风，就能比较多个扬声器探针的相对到达帧号；手机到电脑的固定网络延迟不进入相对差。
- 探针必须携带设备/时隙编码，并继续受节目电平硬门控；静音时不发送。
- 每次检测得到 `设备 → 听音位置` 的整体延迟和置信度，以低带宽方式更新 `[delay, driftPpm]` 状态。
- 多支麦克风或多个互相可听节点可形成带权测量图；选一个参考节点，用加权最小二乘求各节点相对偏移，低置信度、反射跳峰和不闭合的边用鲁棒损失降权。
- 不同城市的节点彼此听不到，不能用声学方式直接校准。每个地点只能使用当地麦克风校准本地声学区域，跨地点对齐依赖会话时钟和硬件时间戳。

TU Delft 的 [Smartphone Audio Acquisition and Synchronization Using an Acoustic Beacon](https://repository.tudelft.nl/record/uuid%3A7ed9f0fc-923b-4e70-a136-26dd493e7dad) 使用已知宽带 beacon 与互相关估计时间偏移，实测同步误差低于 6 个采样，并专门讨论了手机采样率偏差。关于长期漂移，[Sampling Rate Offset Estimation and Compensation](https://arxiv.org/abs/2211.02489) 使用 coherence drift 估计异步声学节点的 SRO，并在频域补偿；[Joint Optimization of Sampling Rate Offsets](https://arxiv.org/abs/2206.13014) 则说明多个节点可联合估计，而不必每个节点只独立对一个参考。

对 VoiceSpreader 最合适的融合方式是：

- 网络时钟观测：频繁、低成本，负责会话时间。
- 播放硬件时间戳：频繁，负责声卡实际帧速率。
- 声学观测：稀疏但能覆盖整条物理链路，负责慢速绝对校正。

## 8. 公网同步与低延迟的取舍

公网链路可以做到“多地同时播放”，但不能同时承诺极低端到端延迟和极强抗抖动：

- 同步精度来自共同时间线和足够的预缓冲，不等于源到听众的实时延迟很低。
- TURN 中继、移动网络切换和跨洲路径可以显著增加 RTT；只要音频提前送达，最终呈现时间仍可对齐。
- 实时通话模式和同步播放模式应成为两个延迟档位：
  - `互动`：小缓冲，允许偶发失步/丢包。
  - `同步播放`：大缓冲，优先保持多个 Sink 同相位附近播放。
- 公网默认 Opus。它具备面向交互音频的丢包恢复/FEC 能力，规范见 [RFC 6716](https://www.rfc-editor.org/rfc/rfc6716.html)；局域网调试阶段先保留 PCM，减少变量。

## 9. 协议草案

### 9.1 控制消息

第一版可用长度前缀 CBOR；调试日志保留 JSON 表示。核心消息：

- `HELLO / AUTH / ACCEPT`
- `CAPABILITIES`
- `SESSION_CREATE / JOIN / LEAVE`
- `ROUTE_PROPOSE / ROUTE_COMMIT`
- `CLOCK_PING / CLOCK_PONG / CLOCK_MODEL`
- `STREAM_ANNOUNCE / READY / START_AT / STOP_AT`
- `VOLUME / MUTE / LATENCY_POLICY`
- `CALIBRATION_SCHEDULE / ACOUSTIC_RESULT`
- `STATS / HEALTH / ROUTE_CHANGED`
- `DISCONNECT / REVOKE_DEVICE`

所有改变路由或录音状态的操作必须带用户授权来源、会话 epoch 和递增 revision，避免重连后的旧命令覆盖新状态。

### 9.2 能力描述

节点至少上报：

- 支持的源/播放/麦克风角色。
- PCM 格式、Opus、采样率、声道数和最大包时长。
- 本地端点类型：内置、有线、USB、HDMI、Classic Bluetooth、LE Audio。
- 是否能提供硬件播放时间戳、时间戳最近年龄和稳定状态。
- 最低缓冲、当前路由延迟、重采样范围。
- 是否支持 Wi-Fi Direct、IPv6、ICE、TURN。
- 电池/供电和前后台状态。

### 9.3 连接状态机

```text
Discovered → Pairing → Authenticated → ClockSyncing
           → Ready → Armed → Playing → Recovering
                                └────→ Disconnected
```

UI 不再只显示“已连接”，而应区分：已配对、网络已连、时钟锁定、缓冲就绪、正在播放、降级/异常。

## 10. 分阶段实施计划

### 阶段 A：协议与核心解耦

目标：不改变当前使用方式，先拆掉“手机只能是 TCP 麦克风”的结构限制。

工作项：

- 定义协议 v2、节点 ID、端口 ID、会话/流/epoch 数据结构。
- 抽象 `Transport`、`SessionNode`、`AudioSourcePort`、`AudioSinkPort`。
- 给现有 `PhonePairingServer` 增加协议适配层，保留 v1 兼容。
- 把当前远程麦克风帧改成通用带时间戳媒体帧。
- 增加协议编解码、恶意长度、乱序、重连和版本不匹配测试。

完成标准：现有手机麦克风、桌面同步和声学校准行为不退化；v1/v2 均可本地配对。

### 阶段 B：手机成为局域网播放端

目标：完成第一条“电脑 → 手机”的应用内音频链路。

工作项：

- Android 新增前台播放服务和 `AudioTrack` 流式播放。
- 桌面将当前捕获 PCM 作为一个源流，同时发送给本地 `OutputWorker` 和远程 Sink。
- 手机回报缓冲帧数、`AudioTimestamp`、underrun 和实际路由设备。
- UI 设备池中把手机显示为可勾选输出，保留其麦克风角色开关。
- 先用局域网 PCM16/48 kHz/双声道验证，不同时引入 Opus 和公网变量。

完成标准：电脑节目能连续播放到手机 2 小时；手机退后台、锁屏、Wi-Fi 短断重连后能恢复；无无限增长缓冲。

### 阶段 C：局域网跨节点同步

目标：手机扬声器与 Windows 本地输出按同一媒体时间线播放。

工作项：

- 实现四时间戳时钟交换、低 RTT 过滤、offset/skew 回归。
- 所有媒体帧加入 `presentationTimeNs`，实现 `READY + START_AT` 两阶段启动。
- Android 根据 `AudioTrack` 帧位置建立硬件时钟模型。
- 把当前重采样控制器抽成跨平台 ASRC，Android 通过 NDK/JNI 复用。
- 路由变化、蓝牙切换、来电打断后重新锁定，不沿用失效模型。

完成标准：有线 Windows 输出与手机内置扬声器连续 60 分钟，数字播放位置差 p95 小于 2 ms；发生一次短时网络抖动后 5 秒内回锁。

### 阶段 D：跨节点声学闭环

目标：校正 API 看不到的 DAC、蓝牙和空气路径延迟。

工作项：

- 给探针调度加入全局 stream frame/token，使本地和远程 Sink 都能发出可识别探针。
- 手机录音帧时钟与会话时钟建立单独模型，不能用 TCP 包抵达时间代替录音时间。
- 实现 pairwise 测量图、鲁棒加权求解和闭环残差检查。
- 只在同一 `acousticZone` 内比较声学到达时间。
- 保留节目电平硬门控、低置信度冻结和缓慢延迟调整。

完成标准：同一听音位置的两个有线/手机输出，声学相对误差 p95 小于 2 ms，连续 2 小时无单向累积漂移；无法可靠检测时安全退回网络/硬件时钟同步。

### 阶段 E：Wi-Fi Direct

目标：无路由器也能建立同样的设备池会话。

工作项：

- Android 接入 `WifiP2pManager`，处理权限、广播、Group Owner 和断线清理。
- Windows 接入 WinRT Wi-Fi Direct/Legacy Group Owner 能力验证。
- 连接成功后把获得的网络绑定交给现有 Transport，不复制上层协议。
- 测试设备不支持、热点冲突、系统拒绝配对和用户取消的回退。

完成标准：支持的设备可在无 AP 环境扫码建链；不支持时明确提示并可切回普通 WLAN。

### 阶段 F：公网与加密

目标：不同局域网、移动网络和 CGNAT 下仍能配对、传输与同步。

工作项：

- 搭建最小 HTTPS/WebSocket 信令服务和一次性房间码。
- 部署 STUN/TURN；TURN 凭据短时有效。
- 完成 libdatachannel/libwebrtc 技术验证并固定媒体传输实现。
- 接入 DTLS-SRTP、设备身份、PAKE 配对、撤销和密钥轮换。
- 加入 Opus、丢包隐藏、带宽自适应、路径变更和自动重连。

完成标准：家庭宽带 ↔ 手机 5G、双 CGNAT 与强制 TURN 三种场景均可连接；信令/中继不可读取明文媒体；公网同步播放模式下 30 分钟无持续漂移。

### 阶段 G：反向音频与 Android 路由

目标：手机也可以作为节目源，电脑或其他手机成为输出。

工作项：

- 先支持手机麦克风和 VoiceSpreader 自有 PCM 作为源。
- 可选增加经用户授权的 Audio Playback Capture，并在 UI 明确显示哪些应用不可捕获。
- Windows 输出端继续使用现有 WASAPI workers。
- Android 发布 MediaRouteProvider，使兼容应用能看到 VoiceSpreader 路由；把它标记为“兼容应用路由”，不宣称全系统虚拟声卡。

完成标准：源和 Sink 角色可在会话中互换；路由提交能阻止反馈环；捕获被系统拒绝时不静默失败。

### 阶段 H：Windows 虚拟扬声器

目标：任意 Windows 应用可在声音设置中选择 `VoiceSpreader Virtual Speaker`。

工作项：

- 按现有 `driver/README.md` 从 SysVAD/WaveRT 缩减出单一渲染端点。
- 驱动只暴露格式、缓冲、位置和时钟；网络、混音、同步和声学校准留在用户态。
- 设计可靠的内核到用户态环形缓冲与断开降级策略。
- 个人机器采用测试证书和 Test Mode，不在本阶段申请发布签名。

完成标准：Windows 默认输出切到虚拟扬声器后，本地和远程设备池完整接管声音，不再有物理主源绕过 VoiceSpreader 先发声。

## 11. 观测与 UI

每个设备卡片应显示：

- 角色和当前本地端点。
- 连接路径：LAN、Wi-Fi Direct、ICE Direct、TURN、Bluetooth Sink。
- 网络 RTT、jitter、丢包、目标缓冲。
- 会话时钟 offset、skew、锁定质量。
- 硬件播放位置、估计设备 ppm、underrun。
- 声学相对延迟、置信度、上次测量时间。
- 当前端到端目标延迟和实际呈现误差。

默认界面只给用户“设备池、勾选播放、音量、同步质量”四类信息；完整数字放在可展开诊断面板，避免让正常使用被工程参数淹没。

建议三档同步状态：

- 绿色：时钟锁定、缓冲安全、声学或硬件误差在目标内。
- 黄色：可播放但使用较大缓冲、TURN、时间戳缺失或声学校准较旧。
- 红色：持续 underrun、路由失效、时钟模型发散或声学结果不闭合。

## 12. 验证方案与量化指标

### 12.1 自动化测试

- 协议 fuzz：长度、版本、重复包、乱序、旧 epoch、非法路由图。
- 虚拟时钟测试：固定 offset、±20/100/500 ppm、时钟跳变、休眠恢复。
- 网络模拟：延迟、jitter、随机丢包、突发丢包、重排、带宽骤降。
- ASRC 测试：频率响应、连续性、缓冲闭环和大误差恢复。
- 路由安全：环路、未授权麦克风、撤销设备、重放命令。

### 12.2 实机矩阵

- Windows：共享/独占、不同物理声卡、HDMI、USB、有线、Classic Bluetooth。
- Android：至少两家厂商、不同 API 级别、内置扬声器、USB、蓝牙、锁屏与省电模式。
- 网络：以太网、普通 WLAN、AP 客户隔离、Wi-Fi Direct、家庭 NAT、手机 5G、强制 TURN。
- 会话：2、3、5 个 Sink；路由热切换；源设备离线；Coordinator 重选。

### 12.3 建议验收指标

| 指标 | 局域网同一声学区域 | 公网不同地点 |
| --- | --- | --- |
| 数字播放位置差 p95 | ≤ 2 ms | ≤ 5 ms |
| 声学到达差 p95 | 有线/手机扬声器 ≤ 2 ms；Classic BT 单独记录 | 不适用，按各地点本地校准 |
| 连续运行 | 2 小时无累积漂移 | 30 分钟无累积漂移 |
| 短时网络扰动恢复 | ≤ 5 秒 | ≤ 15 秒 |
| 不可恢复错误 | 有明确状态和重连入口，不允许静默无声 | 同左 |

声学指标应用双通道录音接口或同一手机录音做离线互相关复核，不能只相信程序自己的估计值。

## 13. 主要风险与应对

1. **Android 厂商音频时间戳质量不同**  
   检测 timestamp age、单调性和回归残差；不可靠时增加缓冲并依赖声学闭环。

2. **公网 P2P 不是必然成功**  
   产品逻辑从第一天就接受 TURN 路径，不把“必须直连”写进同步层假设。

3. **蓝牙延迟会在模式切换时突变**  
   监听路由变化，废弃旧校准和时钟模型，重新预缓冲、校准后淡入。

4. **Android 无法捕获所有应用声音**  
   把 VoiceSpreader 自有音频、经授权可捕获音频和不可捕获音频明确分级，不尝试规避平台策略。

5. **多源和双向音频容易形成反馈**  
   第一版每个区域仅允许一个节目源；路由图做环检测；通话模式以后再单独引入 AEC。

6. **声学传播距离不是设备延迟**  
   校准结果绑定 `acousticZone + microphonePosition`；换房间或移动麦克风后标记过期。

7. **协调者失效导致时间线分裂**  
   所有命令带 term/epoch；新协调者必须创建新 epoch，并在未来时间边界重新起播，不能沿用两个并行主时钟。

## 14. 研究依据与对本项目的直接启示

- [Inter-Destination Multimedia Synchronization: Schemes, Use Cases and Standardization](https://riunet.upv.es/entities/publication/804ba925-141c-427d-b107-4a58c4eff132)：跨不同接收端的同步是独立问题，存在集中式、主从式和分布式控制；VoiceSpreader 应把 IDMS 作为协议一级能力。
- [Fine-Grained Network Time Synchronization Using Reference Broadcasts](https://www.usenix.org/conference/osdi-02/fine-grained-network-time-synchronization-using-reference-broadcasts)：共同接收事件可消除发送路径的不确定性；这与手机麦克风比较多个声学探针的相对到达时间高度一致。
- [The Flooding Time Synchronization Protocol](https://archive.isis.vanderbilt.edu/node/3600)：周期同步、时钟 skew 回归与失效恢复比一次性 offset 更重要；对应当前“运行久后设备会飘”的根因。
- [RFC 7273: RTP Clock Source Signalling](https://www.rfc-editor.org/info/rfc7273)：网络媒体时间戳、参考时钟和媒体采样时钟必须显式关联；没有公共参考无法保证多接收端紧同步。
- [Smartphone Audio Acquisition and Synchronization Using an Acoustic Beacon](https://repository.tudelft.nl/record/uuid%3A7ed9f0fc-923b-4e70-a136-26dd493e7dad)：已知宽带序列、互相关和采样率偏差估计在普通手机上可行，支持继续使用手机作为听音位置传感器。
- [Sampling Rate Offset Estimation and Compensation for Distributed Acoustic Nodes](https://arxiv.org/abs/2211.02489)：长期同步要估计并补偿采样率差，而不是不断只加减固定延迟。
- [Joint Optimization of Sampling Rate Offsets](https://arxiv.org/abs/2206.13014)：节点多于两个时可以利用所有观测关系联合求解，为未来多手机/多麦克风测量图提供依据。
- [WebRTC 1.0](https://www.w3.org/TR/webrtc/)、[ICE RFC 8445](https://www.rfc-editor.org/info/rfc8445/) 与 [TURN RFC 8656](https://www.rfc-editor.org/rfc/rfc8656.html)：公网连接应采用“直连优先、可靠中继兜底”的成熟体系。

## 15. 推荐的实际开工顺序

不要一开始同时上公网、Opus、Wi-Fi Direct、虚拟驱动和多源混音。最稳妥的主线是：

1. 协议/节点抽象。
2. 局域网电脑 → 手机 PCM 播放。
3. 会话时钟 + 带时间戳起播。
4. Android 硬件时钟 + 跨平台 ASRC。
5. 分布式声学闭环。
6. Wi-Fi Direct。
7. WebRTC/公网/加密/Opus。
8. 手机成为节目源与 MediaRouter 路由。
9. Windows SysVAD 虚拟扬声器。

完成第 5 步时，设备池最核心的“任意节点可加入、多个端点可同时播放、长时间不漂”就已经能在家庭网络内验证；后面的连接方式和系统端点是在稳定核心外增加入口，不会再次推翻同步模型。

