# VoiceSpreader 多平台与 Android 多蓝牙耳机可行性报告

更新时间：2026-08-04
适用项目：VoiceSpreader / VoiceSpreaderAndroid
文档性质：技术调研、平台边界、开发建议与阶段规划；不代表已经实现

## 1. 调研目标

本文回答以下问题：

1. Android 手机能否让多个蓝牙耳机同时播放，以及普通应用能控制到什么程度。
2. VoiceSpreader 能否移植到 HarmonyOS 手机、鸿蒙电脑、iOS、macOS 和 Ubuntu 22.04 及以上版本。
3. 各平台能否捕获系统音频、创建虚拟音频端点、独立驱动多个输出并长期保持同步。
4. 现有 Windows + Android 原型应如何演进，才能避免为每个平台重写同步、网络和校准算法。
5. 下一阶段应该按什么顺序开发，以及每个阶段的验收条件是什么。

除非特别注明，本文讨论的是普通第三方应用能够使用的公开 API。系统应用、自定义 ROM、厂商 Audio HAL、内核驱动和特殊签名能力会单独列出，不能与普通安装包的能力混为一谈。

## 2. 结论先行

### 2.1 总体判断

- **Android 一台手机向任意多个传统蓝牙耳机独立输出：普通应用不可通用实现。** Android 的公开 `BluetoothA2dp` 文档仍写明同一时间只支持一个已连接的 A2DP 设备。Android 12 的多设备组合路由由系统 API、特权应用和厂商 Audio HAL 共同实现，不是普通 APK 可直接调用的通用能力。
- **Android 同时播放到两个或更多耳机：在部分系统组合上可行。** Samsung Dual Audio 可以同时选择两个蓝牙设备；Pixel 8 及更新机型的 Audio Sharing 面向 LE Audio 耳机。此时 VoiceSpreader 只需播放一条普通媒体流，由系统负责复制和底层同步。
- **LE Audio 是长期最合理的蓝牙方向，但不能当作所有 Android 手机和耳机已经具备的基础能力。** LE Audio 的 Multi-Stream 和 Broadcast/Auracast 从协议层支持一个源向一个或多个接收端发送同步音频，但实际能力仍取决于手机控制器、系统、厂商实现和接收设备。
- **HarmonyOS 手机可做 VoiceSpreader 节点，但任意多个本地蓝牙输出的公开能力不足。** OHAudio 提供 C/C++ PCM 播放、录制、音量和低时延模式；公开路由接口可枚举/观察设备。当前公开资料不能证明普通应用能对多个本地蓝牙端点分别打开渲染流和单独调度。
- **鸿蒙电脑可开发原生应用，但应先做能力验证。** 华为已经提供鸿蒙电脑开发入口、ArkUI 多端部署和 C/C++ 调试；Qt 也正在推进 HarmonyOS 适配。不过其音频路由、系统回环捕获和虚拟音频端点能力不应在真机验证前视为与 Windows、macOS 或 Linux 等价。
- **iOS 可作为网络播放/麦克风节点，也能使用系统 Share Audio 给两副兼容 AirPods/Beats 播放；普通应用不能把任意两副蓝牙耳机当成两条独立输出。** `AVAudioSession` 的 MultiRoute 面向 USB/HDMI/有线组合，不是任意双 A2DP 路由。
- **macOS 是高可行性目标。** Core Audio 可以直接访问设备和硬件时间，系统自带 Multi-Output/Aggregate Device 和漂移校正；系统音频可通过 ScreenCaptureKit 或 Core Audio Process Tap 获取。真正发布虚拟声卡则涉及 AudioDriverKit、系统扩展、签名和 entitlement。
- **Ubuntu 22.04+ 是最适合优先移植的非 Windows 桌面平台。** PipeWire/PulseAudio 都有组合输出和虚拟 sink；PipeWire 还能在用户态建立虚拟设备、回环和延迟补偿。多个蓝牙耳机能否稳定同时连接仍受蓝牙控制器和设备影响，但软件路由开放度最高。

### 2.2 可行性总表

| 平台 | VoiceSpreader 作为网络节点 | 多个本地物理输出 | 多个蓝牙耳机 | 系统音频捕获 | 虚拟音频输出端点 | 推荐等级 |
| --- | --- | --- | --- | --- | --- | --- |
| Android 普通 APK | 高 | 受系统路由限制 | 厂商/LE Audio 条件可行，不能通用逐路控制 | 部分可行，需用户授权且源应用可拒绝 | 普通 APK 不可作为全系统 Audio HAL 端点 | 继续作为移动节点，不能承诺通用双蓝牙 |
| Android 系统应用/自定义 AOSP | 高 | 高，取决于 HAL | 中到高，取决于硬件/HAL | 高 | 可做，但维护成本极高 | 仅实验室路线 |
| HarmonyOS 手机 | 高 | 中，需实机验证路由能力 | 低到中，系统能力不透明 | 中，AVScreenCapture 路线需验证限制 | 未发现普通应用公开方案 | 做原生节点 PoC，不先做多蓝牙承诺 |
| 鸿蒙电脑 | 中到高 | 中，需 SDK/真机验证 | 未知到中 | 中，需验证 | 高风险/资料不足 | 在有目标真机后进入 |
| iOS | 高 | 中，MultiRoute 有组合限制 | Share Audio 条件可行；任意双 A2DP 不可通用 | 部分可行，强调用户选择和权限 | 普通应用不可提供通用系统虚拟输出 | 做网络节点和系统共享适配 |
| macOS | 高 | 高 | 中到高，按系统可见设备与实际链路测试 | 高 | 可行但需 DriverKit、签名与 entitlement | 第二个桌面移植目标 |
| Ubuntu 22.04+ | 高 | 高 | 中到高，受控制器/协议影响 | 高 | 高，PipeWire 用户态即可 | 第一个非 Windows 桌面目标 |

说明：表中的“高”表示公开能力和工程路径明确，不代表所有硬件组合都能达到同一延迟。“系统音频捕获”也不等于可以绕过 DRM 或应用主动设置的禁止捕获策略。

## 3. 必须先区分的三个概念

### 3.1 系统复制与应用独立输出

系统复制是指应用只提交一条音频流，由操作系统或蓝牙栈复制到多个设备。Samsung Dual Audio、Pixel Audio Sharing、Apple Share Audio 和 macOS Multi-Output Device 都属于这一类。

应用独立输出是指 VoiceSpreader 能分别拿到每个设备的：

- 独立 PCM 写入通道；
- 独立音量；
- 播放帧位置或硬件时间戳；
- 独立延迟缓冲；
- 独立重采样比率；
- 路由变化和 underrun 状态。

只有第二种模式才能完整复用当前 Windows 端的逐设备补偿和 ASRC 漂移控制。系统复制模式应在 UI 中显示为一个“系统组合端点”，不能伪装成两个可独立校正的输出。

### 3.2 应用自己的声音与系统所有声音

VoiceSpreader 自己产生或已经收到的 PCM，在所有目标平台都相对容易发送和播放。捕获其他应用或整个系统的声音则受隐私、DRM、用户授权和平台安全模型限制。

因此跨平台核心应把源能力分级：

```text
OwnStream        VoiceSpreader 自己拥有的 PCM
UserAuthorized   经系统选择器/录屏权限授权的系统或应用音频
SystemEndpoint   通过虚拟声卡、Audio HAL 或系统驱动接管的全局输出
Unavailable      平台或源应用明确禁止捕获
```

### 3.3 数字播放同步与人耳位置的声学同步

设备报告同时播放，并不代表声音同时到达耳边：

```text
听音位置到达时间
  = 公共呈现时间
  + 网络与解码缓冲
  + 操作系统/驱动缓冲
  + DAC、蓝牙和设备内部缓冲
  + 扬声器到听音位置的空气传播时间
```

macOS Aggregate Device、PipeWire combine stream 和 LE Audio 可以帮助解决设备时钟与数字流同步，但最终仍应保留 VoiceSpreader 已有的手机麦克风声学校准。对于系统不开放单个耳机控制的组合端点，声学测量只能报告组合的实际效果，不能保证应用有能力修正其中一个成员。

## 4. Android 多蓝牙耳机详细分析

### 4.1 传统 Bluetooth Classic/A2DP

Android 的公开 `BluetoothA2dp` API 明确写明 Android 同时只支持一个已连接的 A2DP 设备。这是普通 Android 应用必须采用的保守兼容基线。

Android 12 开始，AOSP 的组合音频设备路由移除了部分框架限制，可支持 BLE 音频组播和多个音频设备。但官方同时说明：

- 多个首选设备由**特权应用**通过系统 API 选择；
- 设备能力由厂商 Audio HAL 上报；
- Android 12 的多 USB 同时路由仍有限制，Android 14 才在设备类型不同且内核/厂商支持时扩展；
- 是否真正可用取决于厂商实现，而不是只检查 Android API 等级。

因此，普通 VoiceSpreader APK 不应尝试通过反射调用隐藏 API，也不应申请 `BLUETOOTH_PRIVILEGED` 后假设能够获批；该权限不是普通第三方应用权限。

### 4.2 厂商的双音频功能

Samsung Dual Audio 是当前最明确的传统蓝牙双输出路径。三星官方说明支持同时选择两个蓝牙音频设备，第三个设备会触发二选一断开，并可在媒体面板中分别调整音量。

对 VoiceSpreader 的意义是：

- 应用播放一条 `AudioTrack` 即可；
- 用户在系统媒体输出面板中选择两个设备；
- VoiceSpreader 不能获得两个独立 A2DP 编码器的精确呈现时间；
- 独立音量由系统面板管理，应用侧只应提供总音量；
- 两副耳机之间的残余延迟应标记为“系统管理”，不能承诺应用自动修正。

这条路径适合“方便地让两个人听到”，不适合要求两个物理扬声器在同一房间达到亚毫秒声学同步的场景。

### 4.3 LE Audio、Multi-Stream 与 Auracast

Android 13 内置 LE Audio 支持。Android 官方把音频共享描述为一个或多个 sink 同时接收同步流；Bluetooth SIG 则将 Multi-Stream 定义为一个源与一个或多个接收端之间的多个独立同步流，并用 Auracast 提供广播音频。

需要注意三个边界：

1. 一对真无线耳机的左右耳通常是一个 Coordinated Set。它包含两个物理成员，但对应用可能只表现为一个逻辑设备。
2. Android 的公开 `BluetoothLeAudio` 文档仍以一个已连接的 LE Audio set 为基础；一个 set 可以包含左右耳等多个成员。面向多个独立听众的 Audio Sharing/Broadcast 是另一层系统能力，不能用“已连接 set 数量”简单推断。
3. 手机支持 LE Audio 不等于支持广播源。应检测系统公布的 LE Audio 和 broadcast-source 能力。
4. 即使底层能同步多个接收端，普通媒体应用通常仍是把一条流交给系统，不能逐个接收端设置 VoiceSpreader 的软件延迟和重采样器。

Pixel 官方的 Audio Sharing 当前要求 Pixel 8 或更新机型，并面向 LE Audio 配件。它比传统双 A2DP 更适合作为同步耳机组，但仍应作为“系统组合端点”接入。

### 4.4 Android 推荐实现模式

#### 模式 A：系统组合端点

适用：Samsung Dual Audio、Pixel Audio Sharing、未来厂商 LE Audio/Auracast 共享。

实现建议：

- VoiceSpreader 使用一个 `AudioTrack` 输出；
- Android 16/API 36 及以上可用 `AudioTrack.getRoutedDevices()` 观察实际多路路由，旧版本根据厂商特征和用户确认显示状态；
- 监听路由变化，任何设备增减都令当前校准失效；
- UI 显示“系统组合：2 个设备”，不渲染两个可独立延迟滑块；
- 音量显示“应用总音量”，并引导用户到系统媒体面板调成员音量；
- 将系统组整体作为 VoiceSpreader 设备池中的一个 `SinkGroup`。

#### 模式 B：一台手机一个耳机/输出节点

适用：任意品牌、需要 VoiceSpreader 自己长期同步、系统没有双音频功能。

实现建议：

- 每台手机只连接一个本地耳机或扬声器；
- PC 或协调节点向每台手机发送同一时间线上的音频；
- 每台手机通过 `AudioTimestamp` 建模播放时钟并独立 ASRC；
- 放在听音位置的校准手机比较各节点声学到达时间；
- 蓝牙端到端延迟很大也没有关系，只要预缓冲足够且长期漂移可跟踪。

这是跨品牌、跨协议最可控的方案，也是 VoiceSpreader 应作为产品主线支持的模型。

#### 模式 C：自定义 AOSP/系统应用

适用：只控制少量自有测试设备，接受刷机和厂商适配。

实现建议：

- 使用 AOSP 组合路由系统 API；
- 修改 Audio Policy 和 Audio HAL，确保多设备 profile、时钟和延迟可观测；
- 为不同设备创建独立混音/重采样路径；
- 对蓝牙控制器并发、编码器资源和射频带宽做实机验证。

这不是普通 VoiceSpreaderAndroid 的发布路径。即使不发布，它仍需要可解锁设备、系统镜像构建链、恢复方案和长期版本维护。

#### 模式 D：外置双发射器

USB-C 双蓝牙发射器可把两个耳机隐藏在一个 USB 音频设备后面，兼容性往往优于软件改系统，但 VoiceSpreader 同样无法逐耳机校正。它适合作为低工程成本的个人使用备选。

### 4.5 Android 能力检测与 UI 建议

每次开始播放前生成能力报告：

```text
AndroidAudioCapabilities {
  sdkLevel
  manufacturer / model
  classicA2dpConnectedCount
  leAudioSupported
  leAudioBroadcastSourceSupported
  routedDevices[]
  routeControl = AppIndependent | SystemCombined | SingleOnly
  memberVolumeControl = App | SystemUI | Unavailable
  memberTimingControl = App | SystemManaged | Unavailable
}
```

建议状态文案：

- `单设备`：当前只有一个可用输出。
- `系统双输出`：系统正在复制到两个设备，VoiceSpreader 不能逐路补偿。
- `LE Audio 共享`：由系统/LE Audio 同步，成员数和独立音量按平台能力显示。
- `独立 VoiceSpreader 节点`：可显示逐节点延迟、ppm、置信度和校准结果。
- `路由未知`：API 无法确认实际设备，禁止显示“已同步”。

## 5. HarmonyOS 移植可行性

### 5.1 HarmonyOS 手机/平板

#### 可直接利用的能力

华为 OHAudio 是面向 Native 层的 C API，可完成 PCM 播放与录制，并提供：

- 回调式音频数据写入；
- 当前音频流音量；
- 48 kHz 条件下的低时延模式；
- 音频设备枚举和设备变化监听；
- C/C++ 代码与 ArkTS/ArkUI 界面的组合开发。

这足以实现一个 VoiceSpreader 网络节点：接收带呈现时间的 PCM/Opus、建立抖动缓冲、播放、回传统计，并用麦克风做声学校准。

#### 路由与多设备边界

公开的 OHAudio Routing Manager 能枚举设备、监听变化并查询首选输入/输出设备。OpenHarmony 的公开资料中，多设备 AVSession 投播和选择输出的接口带有系统应用权限；公开选择接口历史上也以单个首选输出为主。

因此本文作以下工程判断：

- 普通应用可安全依赖“当前系统路由”播放；
- 普通应用能否为两个本地蓝牙设备分别创建可控输出，标记为**未证实**；
- 分布式设备/跨端投播不能因为系统宣传“超级终端”就假设对普通应用开放全部底层时间戳和路由控制；
- 必须在目标华为手机、耳机和 HarmonyOS 版本上做 P0 真机验证。

#### 系统音频捕获

HarmonyOS/OpenHarmony 提供 AVScreenCapture 的 ArkTS 和 C/C++ 取流路线，公开文档显示可取得屏幕和音频流。实现 VoiceSpreader 系统音源时应把它视为“用户主动授权的屏幕/音频捕获”，并验证：

- 是否能只取音频而不持续处理视频；
- 是否包含其他应用的播放声音；
- DRM/受保护内容如何处理；
- 锁屏、后台和多窗口时是否继续；
- 鸿蒙电脑与手机的权限提示是否一致。

在这些结果明确前，不应把 AVScreenCapture 描述成 Windows WASAPI Loopback 的完全替代品。

#### 推荐 UI 与核心技术

- 第一选择：ArkUI/ArkTS 做平台壳，复用 C++17 的协议、DSP、时钟、ASRC 和校准核心。
- 音频后端：OHAudio C API，不依赖 Qt Multimedia。
- 网络：优先复用 C++ 传输层；系统权限、前后台服务和设备发现由 ArkTS 适配。
- Qt 路线：作为平行 PoC，不作为第一个可用版本的唯一前提。

### 5.2 鸿蒙电脑

华为官方已经提供鸿蒙电脑应用开发入口，并明确支持 ArkUI、ArkTS、C/C++ 调试、多窗口、多端部署和媒体能力。因此“做一个鸿蒙电脑版 VoiceSpreader 应用”在工程上是可行目标。

不过，需要拆开评估：

| 能力 | 当前判断 | 原因与验证方式 |
| --- | --- | --- |
| 运行桌面 UI | 高 | 官方已有鸿蒙电脑应用模型和窗口适配指南 |
| 复用 C++ 同步/网络核心 | 高 | 官方工具链支持 ArkTS + C/C++ 跨语言调试 |
| 播放 VoiceSpreader 自己的 PCM | 高 | OHAudio Native 路线明确 |
| 枚举当前音频设备 | 高 | Routing Manager 有公开查询接口 |
| 同时独立打开多个本地输出 | 未证实 | 需检查鸿蒙电脑 SDK 并在真机测试 |
| 捕获系统其他应用音频 | 中 | AVScreenCapture 有路线，但权限和范围需验证 |
| 注册全系统虚拟扬声器 | 低/高风险 | 未找到与 PipeWire 虚拟 sink 或成熟桌面虚拟声卡等价的普通应用公开接口 |
| 多蓝牙耳机逐路控制 | 低到中 | 取决于系统组合路由、蓝牙栈和公开 API |

#### Qt 在 HarmonyOS 上的现状

Qt 官方 Wiki 已有 Qt for HarmonyOS、Qt 6 构建说明和已知限制页面，Qt 官方也公开表示正在推进 HarmonyOS 移植。与此同时，Qt 6.10 的正式支持平台表列出了 Windows、macOS、Linux、Android 和 iOS，但没有把 HarmonyOS 列入同等级的正式支持配置。当前 Qt for HarmonyOS 文档还处于快速变化阶段，Qt 6.12.0 Beta 路径和较新的 HarmonyOS SDK 要求也说明了这一点。

建议：

- 不直接把当前 Qt 5.15 Widgets 工程当作鸿蒙正式移植方案；
- 先把业务核心去 Qt 化，再做 ArkUI 壳；
- 同时建立 `qt-harmony-spike` 分支验证 Qt 6 Widgets/Quick、网络、字体、HiDPI 和 Native OHAudio 桥接；
- 如果 Qt 6 Harmony 在目标设备上稳定，再决定是否用它统一鸿蒙电脑 UI；
- 无论 UI 用什么，音频后端都应直接调用 OHAudio，避免受 Qt Multimedia 后端覆盖程度限制。

## 6. Apple 平台可行性

### 6.1 iOS/iPadOS

#### 多蓝牙耳机

Apple Share Audio 能把 iPhone/iPad 当前声音共享给两副兼容的 AirPods 或 Beats，并允许分别设置音量。这与 Samsung Dual Audio 类似，是系统管理的组合端点。

普通应用层面的 `AVAudioSession.MultiRoute` 虽然支持同时把不同流送到不同输出，但 Apple 列出的经典有效组合是：

- USB + 有线耳机；
- HDMI + 有线耳机；
- LineOut + 有线耳机。

它不能被解释为“任意两副 A2DP 蓝牙耳机”。Apple 的 A2DP 选项文档还说明，传统 MultiRoute 会清除 A2DP 选项；新版本的 dual-route 模式主要面向内置扬声器/麦克风和一个支持输入输出的辅助设备，也不是任意双 A2DP 媒体输出。

因此 iOS 推荐两种端点：

1. `AppleSharedAudioGroup`：用户在系统界面启用 Share Audio，VoiceSpreader 只输出一条流。
2. `VoiceSpreaderIOSNode`：iPhone/iPad 自己作为一个网络 sink，可连接其当前系统输出设备。

#### 系统音频与虚拟设备

- VoiceSpreader 自己收到或生成的音频可以用 AVAudioEngine/Audio Unit 播放。
- ScreenCaptureKit 在新系统上提供经系统内容选择器授权的屏幕/音频捕获能力，但版本要求和用户交互较强，应作为可选源，不作为透明全局回环的基础。
- 普通 iOS 应用不能像桌面驱动一样向整个系统注册一个任意应用都能选择的虚拟扬声器。
- 后台持续播放需配置音频后台模式，并正确处理来电、Siri、路由变化和媒体服务重置。

#### UI 技术选择

虽然 Qt 正式支持 iOS，但对于权限、系统内容选择器、后台音频、Share Audio 引导和路由状态，Swift/SwiftUI 壳更自然。建议保留 C++17 核心，通过 Objective-C++ 桥接 AVFAudio/Core Audio，而不是强行复用桌面 Widgets UI。

### 6.2 macOS

macOS 对 VoiceSpreader 的适配价值很高：

- Core Audio 可以枚举多个设备，为每个设备建立 I/O callback；
- `AudioDeviceGetCurrentTime`、`AudioDeviceStartAtTime` 等接口提供设备时间和按时间启动能力；
- Audio MIDI Setup 可以创建 Multi-Output Device；
- Aggregate Device 支持选择主时钟并为其他设备启用 Drift Correction，即重采样补偿；
- ScreenCaptureKit 可在用户授权下捕获桌面/应用音频；
- Core Audio Process Tap 可以构造进程音频 tap；
- Qt 6 正式支持 macOS，桌面 UI 迁移路径清晰。

#### 推荐的两级实现

##### 第一级：无驱动版本

- Qt 6 Widgets/Quick 负责 UI；
- Core Audio HAL 负责逐设备输出；
- ScreenCaptureKit 或 Process Tap 负责系统源；
- VoiceSpreader 保留独立 gain、delay、ASRC 和声学校准；
- 可读取系统 Multi-Output/Aggregate Device 作为一个端点，也可由程序分别打开设备。

这个版本已经能覆盖绝大多数个人使用需求。

##### 第二级：虚拟扬声器

AudioDriverKit 支持用户态 DriverKit 音频扩展，并能发布音频设备、流、时钟和音量控制。但 Apple 官方样例明确需要系统扩展、DriverKit Audio Family 等 entitlement、App ID、provisioning profile 和代码签名。

因此：

- “不发布”不能消除 macOS/iPadOS 驱动扩展的签名与 entitlement 要求；
- 应先完成无驱动捕获版本；
- 只有当用户确实需要在所有应用的输出列表中看到 `VoiceSpreader Virtual Speaker` 时，再申请相应权限并实现 AudioDriverKit 后端。

#### macOS 蓝牙注意事项

系统能把多个可见输出加入 Multi-Output/Aggregate Device，但蓝牙耳机的连接数、编解码器、麦克风启用后是否切换到通话 profile、延迟和稳定性仍取决于硬件与系统。VoiceSpreader 应：

- 把系统组合设备作为基线；
- 对可独立打开的设备优先使用自己的每设备后端；
- 检测 sample rate/profile 变化并废弃旧校准；
- 蓝牙麦克风开启导致音质/延迟突变时重新建模。

## 7. Linux / Ubuntu 22.04+ 可行性

### 7.1 音频栈版本边界

Ubuntu 22.10 开始默认采用 PipeWire 作为音频系统；因此 Ubuntu 22.04 LTS 需要兼容两种实际环境：

- 默认或传统安装中的 PulseAudio；
- 用户手动切换或桌面环境已经采用的 PipeWire/PipeWire-Pulse。

Ubuntu 24.04 LTS 及更新版本已经是更明确的 PipeWire 基线。项目可以声明支持 Ubuntu 22.04+，但应把 PipeWire 作为主后端、PulseAudio 作为 22.04 兼容后端。

### 7.2 多输出与同步

PipeWire 的 `libpipewire-module-combine-stream` 能创建一个向多个 sink 转发的虚拟 sink，并有 `combine.latency-compensate` 延迟补偿选项。PulseAudio 的 `module-combine-sink` 也会周期性重算从设备采样率，用重采样补偿不同时钟晶振的偏差。

这意味着 Ubuntu 上有三种实现层级：

1. 直接使用系统 combine sink，最快得到“同时出声”。
2. VoiceSpreader 枚举 PipeWire sink，并为每个 sink 创建独立 stream，实现独立音量、延迟和 ASRC。
3. 发布一个名为 `VoiceSpreader` 的 PipeWire 虚拟 sink，接收任意应用音频，再由 VoiceSpreader 分发到本地和网络节点。

第 3 种不需要内核驱动。PipeWire 的 loopback、example sink、pipe tunnel 和 filter-chain 文档都给出了用户态虚拟 sink/source 路径，明显比 Windows SysVAD 和 macOS AudioDriverKit 更适合早期验证。

### 7.3 多蓝牙耳机

WirePlumber/PipeWire 使用 BlueZ monitor 为已连接蓝牙设备创建节点，并支持 A2DP、HFP 以及 BAP/LE Audio 角色。只要蓝牙控制器和 BlueZ 能让两副耳机保持活动，每副耳机就可以作为 sink 进入 combine-stream 或 VoiceSpreader 独立输出图。

主要风险是：

- 单个蓝牙控制器的并发链路和射频带宽；
- 两副耳机使用不同 codec/sample rate；
- HFP 麦克风启用导致 profile 切换；
- 某些设备被 WirePlumber 策略暂停或抢占；
- 蓝牙缓冲不公开精确硬件呈现时间。

所以 Ubuntu 的“多个蓝牙耳机”软件上可行度很高，但仍必须建立硬件兼容矩阵。长期同步继续使用数字时钟模型 + ASRC + 手机麦克风声学闭环，而不是只相信 combine sink 的静态 latency。

### 7.4 推荐实现

- UI：Qt 6，保留当前视觉设计并统一 HiDPI/font pipeline。
- 主后端：直接使用 `libpipewire-0.3`，不要只调用 `pactl` 命令拼装产品逻辑。
- 兼容后端：libpulse，用于 Ubuntu 22.04 默认 PulseAudio 环境。
- 虚拟端点：首版通过用户级 PipeWire 配置或内嵌 client node 创建，不安装内核模块。
- 打包：先提供普通 CMake/Ninja 构建；验证稳定后再考虑 AppImage/Flatpak。Flatpak 会引入 portal 和音频访问权限边界，不能作为第一个调试环境。

## 8. 跨平台架构建议

### 8.1 不以 Qt Multimedia 作为音频核心

Qt 适合统一 Windows、macOS、Linux 的桌面 UI，但精细同步需要各平台的原生音频时钟、路由和设备 API。推荐结构：

```text
apps/
  windows-qt/          Qt 6 + WASAPI
  linux-qt/            Qt 6 + PipeWire/PulseAudio
  macos-qt/            Qt 6 + Core Audio
  android/             Kotlin UI + JNI/NDK
  ios/                 SwiftUI + Objective-C++
  harmony/             ArkUI/ArkTS + NAPI

core/
  audio/               ring buffer、format、mixer、gain、delay
  sync/                session clock、timestamp model、ASRC、drift controller
  acoustic/            probe、相关检测、置信度、闭环控制
  transport/           framing、encryption、LAN/P2P/relay
  session/             node、port、route graph、capability negotiation

platform/
  windows-wasapi/
  linux-pipewire/
  linux-pulse/
  macos-coreaudio/
  android-aaudio/
  ios-coreaudio/
  harmony-ohaudio/
```

现有工程使用 C++17，可以继续作为共享核心基线。桌面 UI 可独立迁移到 Qt 6，不必为了移动平台把所有 UI 都改成 Qt。

### 8.2 平台后端统一接口

```cpp
class IAudioPlatformBackend {
public:
    virtual std::vector<AudioEndpoint> enumerateEndpoints() = 0;
    virtual std::unique_ptr<ICaptureStream> openCapture(const CaptureConfig&) = 0;
    virtual std::unique_ptr<IRenderStream> openRender(const RenderConfig&) = 0;
    virtual RouteCapabilities queryRouteCapabilities() = 0;
    virtual ClockObservation queryClock(const EndpointId&) = 0;
};
```

`IRenderStream` 至少应提供：

- 写入/回调帧数；
- 已提交和已呈现帧位置；
- 单调时间戳及其年龄；
- 当前估计延迟；
- underrun/xrun；
- 动态 gain；
- 路由与格式变化事件；
- 是否允许独立 delay/ASRC。

对系统组合端点，后两项能力必须明确返回 `SystemManaged`，不能伪造独立成员控制。

### 8.3 统一端点模型

```text
EndpointKind
  PhysicalIndependent   可独立打开和调度的物理端点
  SystemCombinedGroup   系统复制/同步的组合端点
  RemoteNode            VoiceSpreader 网络节点
  VirtualSink           系统可选择的 VoiceSpreader 虚拟输入端点
```

```text
ControlLevel
  Full          独立音量、延迟、ASRC、时间戳
  GainOnly      只有软件总音量
  SystemOnly    成员音量/路由只能在系统 UI 调整
  ObserveOnly   只能观察当前实际路由
```

这个模型能同时表达：

- Windows/macOS/Linux 多声卡：`PhysicalIndependent + Full`；
- Samsung/Pixel/Apple 双耳机：`SystemCombinedGroup + SystemOnly`；
- 手机网络播放：`RemoteNode + Full`；
- Android/Harmony 当前系统输出：通常是 `PhysicalIndependent + GainOnly/ObserveOnly`。

### 8.4 同步策略

跨平台仍使用四层同步：

1. **会话时钟**：节点之间估计 offset 和 skew。
2. **网络呈现时间**：每帧携带 `presentationTimeNs`，接收端按同一未来时间播放。
3. **设备时钟闭环**：根据硬件帧位置估计 ppm，以 ASRC 缓慢修正。
4. **声学闭环**：手机麦克风在听音位置定期修正不可见的 DAC、蓝牙和空气路径延迟。

对 `SystemCombinedGroup` 只能控制第 1、2 层和整个组的缓冲；第 3、4 层的成员差异只能测量和报警。对 `PhysicalIndependent` 和 `RemoteNode` 才能完整闭环。

## 9. 推荐开发阶段

以下工期按一名开发者集中工作的相对量级估算，只用于安排先后，不是交付承诺。

### 阶段 0：平台能力实机验证（1～2 周）

目标：在重构前先把最可能推翻架构的系统边界测清。

任务：

- Samsung：Dual Audio 下播放一条 `AudioTrack`，记录实际路由、成员音量和两耳残余延迟。
- Pixel 8+：用两组 LE Audio 接收端测试 Audio Sharing、广播能力和路由 API。
- iPhone/iPad：Share Audio 与 MultiRoute 分别测试，确认应用能观察到什么。
- macOS：两个有线/USB 输出和两个可用蓝牙输出分别测试 Multi-Output、Aggregate、Core Audio 时间戳。
- Ubuntu 22.04、24.04：PulseAudio combine-sink、PipeWire combine-stream、两个蓝牙 sink 和虚拟 sink。
- HarmonyOS 手机/鸿蒙电脑：OHAudio 播放、时间信息、路由枚举、AVScreenCapture 音频取流、后台行为和多设备尝试。

交付物：

- `platform-capabilities.json` 样例；
- 每个平台一份原始日志和 30 分钟漂移曲线；
- 明确标记 `Full/SystemManaged/Unavailable`；
- 任何未验证功能保持“实验性”，不进入主 UI 承诺。

退出条件：至少确认 Android、macOS、Ubuntu 三个平台的端点模型不会冲突。

### 阶段 1：抽离可移植 C++17 核心（3～5 周）

目标：让 Windows 现有功能继续工作，同时移除核心对 QWidget、WASAPI 类型和 Windows 时钟类型的直接依赖。

任务：

- 建立 `core/audio`、`core/sync`、`core/acoustic`、`core/session`；
- 定义 `IAudioPlatformBackend` 和 capability schema；
- 把 Qt signal/slot 限制在 app/controller 层；
- 把协议中的时间单位统一为单调纳秒和音频帧号；
- 用模拟时钟测试 offset、skew、ASRC、丢包和路由热切换；
- Windows WASAPI 后端成为第一个接口实现，确保无功能回退。

退出条件：现有 Windows 多设备同步、实时延迟调整和手机麦克风校准全部通过回归测试。

### 阶段 2：Android 系统组合端点适配（1～2 周）

目标：先正确表达现有双音频/LE Audio 能力，不尝试越权实现通用双 A2DP。

任务：

- 增加 LE Audio/broadcast-source 能力检测；
- Android 16+ 读取 `getRoutedDevices()`；
- 将多个实际路由显示为一个 `SystemCombinedGroup`；
- 路由变化立即冻结旧声学校准；
- 增加 Samsung/Pixel 的系统设置引导和明确的能力提示；
- 不为组成员显示不可用的独立延迟滑块。

退出条件：Samsung 双输出、Pixel LE Audio 共享、普通单 A2DP 三种模式都不会误报控制能力。

### 阶段 3：Ubuntu 后端与虚拟 sink（3～5 周）

目标：完成第一个非 Windows 桌面版本，并验证平台抽象。

任务：

- Qt 6 桌面壳；
- PipeWire 设备枚举、捕获、逐设备播放和时间信息；
- 创建 `VoiceSpreader` 虚拟 sink；
- 对 22.04 增加 PulseAudio fallback；
- 支持系统 combine 与 VoiceSpreader 独立调度两种模式；
- 测试有线 + USB + 蓝牙和双蓝牙。

退出条件：两个独立有线/USB 输出连续 2 小时无累计漂移；虚拟 sink 可承接普通桌面应用声音；蓝牙失败时提供明确降级而非静默无声。

### 阶段 4：macOS 无驱动版（4～6 周）

目标：完成 Core Audio 多输出和经授权系统音频捕获。

任务：

- Qt 6 macOS UI；
- Core Audio HAL 设备和时钟后端；
- ScreenCaptureKit/Process Tap 源适配；
- 识别 Multi-Output/Aggregate Device；
- 路由、采样率、睡眠唤醒和蓝牙 profile 变化恢复；
- 保留逐设备与系统组合两种模式。

退出条件：两种不同物理设备连续 2 小时稳定；系统捕获有清晰权限状态；不安装驱动即可完成主要功能。

### 阶段 5：iOS 播放/麦克风节点（4～6 周）

目标：让 iPhone/iPad 加入设备池，而不是先追求全系统虚拟设备。

任务：

- SwiftUI 壳 + Objective-C++ C++ 核心桥；
- AVAudioEngine/Audio Unit 播放和麦克风采集；
- 后台音频、路由变化、来电/中断恢复；
- Share Audio 作为系统组合端点显示；
- 可选 ScreenCaptureKit 用户授权源，按系统版本分级。

退出条件：锁屏/后台 2 小时播放稳定；与 Windows/Linux 节点按会话时间线起播；Share Audio 不误显示逐耳机补偿。

### 阶段 6：HarmonyOS 手机节点（5～8 周）

目标：在原生 HarmonyOS 上完成与 Android 节点等价的网络播放和校准能力。

任务：

- ArkUI/ArkTS 应用壳；
- NAPI 调用共享 C++17 核心；
- OHAudio render/capture 后端；
- 后台长时任务、麦克风权限、网络切换和路由变化；
- AVScreenCapture 作为实验性用户授权源；
- 对系统分布式投播只使用公开且普通应用可获得的能力。

退出条件：目标 HarmonyOS 真机连续 2 小时稳定，锁屏/后台可恢复，手机麦克风校准精度达到 Android 当前水平。

### 阶段 7：鸿蒙电脑与 Qt Harmony 评估（6～10+ 周）

目标：依据阶段 0 的真机结果决定采用 ArkUI 桌面壳还是 Qt 6 Harmony。

两条路线：

- 稳健路线：ArkUI 多端布局 + OHAudio + 共享 C++ 核心。
- 复用路线：Qt 6 Harmony UI + Native OHAudio 后端；只在模块、HiDPI、输入法、网络和长期稳定性均通过后采用。

退出条件：至少两个独立本地输出或一个本地输出 + 一个远程节点稳定工作；未开放的虚拟端点能力不得阻塞基础版本。

### 阶段 8：可选系统级端点（独立项目）

按投入回报排序：

1. Linux PipeWire 虚拟 sink：阶段 3 一并完成。
2. Windows SysVAD：延续现有计划，个人使用可采用测试模式/测试证书。
3. macOS AudioDriverKit：只有确定需要系统输出列表端点并能取得 entitlement 时开始。
4. Android 自定义 AOSP/HAL：仅维护自有测试手机，不合入普通 APK 主线。
5. HarmonyOS 虚拟音频设备：等待公开 DDK/系统能力证实，不以资料缺失作出可交付承诺。

## 10. 优先级与资源建议

### 建议优先级

```text
现有 Windows 稳定性
  → 跨平台核心抽离
  → Android 系统组合端点识别
  → Ubuntu/PipeWire
  → macOS/Core Audio
  → iOS 网络节点
  → HarmonyOS 手机
  → 鸿蒙电脑
  → 各平台系统级虚拟端点
```

优先 Ubuntu 的原因不是用户数量，而是它能以最低系统权限验证三个最关键的跨平台抽象：多独立设备、用户态虚拟 sink 和跨时钟重采样。macOS 随后验证另一套成熟硬件时钟 API。完成这两个桌面后端后，再进入移动平台，核心接口会稳定得多。

### 不建议作为近期主线的事项

- 通过隐藏 API 或反射让普通 Android APK 强行控制两个 A2DP 耳机；
- 自己在应用层实现完整的 LE Audio Controller/Broadcast Source 并期望绕过系统蓝牙栈；
- 在 HarmonyOS 真机验证之前，直接把当前 Qt 5 Widgets 工程整体编译过去；
- 一开始就同时开发 Windows、macOS、Linux 三种虚拟声卡；
- 把系统组合端点拆成虚假的独立滑块；
- 用一次开机校准替代长期 sample-rate drift 控制。

## 11. 验证矩阵与指标

### 11.1 设备组合

| 平台 | 最低测试组合 |
| --- | --- |
| Android | 单 A2DP；Samsung 双 A2DP；Pixel LE Audio 两接收端；USB + 本机扬声器；路由热切换 |
| HarmonyOS 手机 | 本机扬声器；USB；单蓝牙；系统可见多设备；锁屏；后台；AVScreenCapture |
| 鸿蒙电脑 | 内置 + USB；USB + 蓝牙；系统捕获；窗口/睡眠恢复 |
| iOS | 本机/有线；单蓝牙；Share Audio 两副兼容耳机；USB + 有线 MultiRoute；后台中断 |
| macOS | 内置 + USB；双 USB；Multi-Output；Aggregate + drift correction；双蓝牙可用组合；睡眠恢复 |
| Ubuntu 22.04 | PulseAudio 与手动 PipeWire 两套环境；双声卡；双蓝牙；虚拟 sink |
| Ubuntu 24.04+ | PipeWire；双声卡；双蓝牙；WirePlumber 路由变化；虚拟 sink |

### 11.2 指标分层

| 指标 | 独立可控端点 | 系统组合端点 |
| --- | --- | --- |
| 数字呈现位置差 p95 | 有线/USB/远程节点目标 ≤ 2 ms | 只观测，按系统实测报告 |
| 声学到达差 p95 | 同一听音区目标 ≤ 2 ms | 能测量但不保证可修正成员差 |
| 长期漂移 | 连续 2 小时无单向累计 | 记录系统组合是否漂移 |
| 路由变化恢复 | 5 秒内重新建模或明确暂停 | 立即标记旧校准失效 |
| 音量 | 每端点软件 gain | 总音量；成员音量跳转系统 UI |
| 能力显示 | 必须与真实 API 能力一致 | 必须标明“由系统同步/管理” |

### 11.3 日志必须记录

- OS、版本、设备型号、音频后端；
- endpoint/group ID 与实际路由；
- codec、sample rate、channels、buffer size；
- 单调时钟、设备帧位置、timestamp age；
- estimated latency、underrun、ppm、ASRC ratio；
- 网络 RTT/jitter/loss；
- 声学相关峰值、SNR、置信度和校准年龄；
- 路由/profile/采样率变化事件；
- 当前控制等级 `Full/SystemOnly/ObserveOnly`。

## 12. 风险登记

1. **把厂商功能误认为 Android 标准能力**
   通过能力检测和厂商矩阵解决；普通 Android 基线仍是一条 A2DP 路由。

2. **系统组合双耳机之间仍有可闻延迟**
   记录声学结果并提示；应用没有成员控制时不自动来回调整总延迟。

3. **LE Audio 名义支持但无法作为广播源**
   分开检测 LE Audio 和 Broadcast Source，不能只看 Android 版本。

4. **HarmonyOS API 与设备实际开放范围不一致**
   把 HarmonyOS 手机和电脑都设置 P0 门槛；以目标真机权限和返回值为准。

5. **移动系统后台限制导致节点消失**
   Android 前台服务、iOS 后台音频、HarmonyOS 长时任务分别实现，并在 UI 显示“系统挂起”而非普通断网。

6. **蓝牙 profile 切换改变延迟和音质**
   监听设备/profile/sample-rate 变化，销毁旧 clock model 和声学校准后重新锁定。

7. **不同平台的系统捕获受 DRM 或用户授权限制**
   源能力分级；不把拒绝捕获当作程序错误，也不尝试绕过系统策略。

8. **Qt 跨平台造成“看似复用，实际平台能力难接入”**
   只统一桌面 UI；音频后端坚持原生 API；移动端使用平台 UI 壳和共享 C++ 核心。

9. **虚拟音频端点拖慢主线**
   虚拟端点单列阶段，基础应用先通过公开捕获 API 和自己的媒体流工作。

## 13. 最终建议

VoiceSpreader 不应把下一阶段定义为“让每个操作系统都直接驱动任意多个蓝牙耳机”，因为这是系统权限、厂商 HAL 和硬件共同决定的目标，普通应用没有统一控制面。

更稳健的产品定义是：

> VoiceSpreader 把所有可独立控制的本地音频设备、系统管理的组合输出和远程手机/电脑统一放入一个设备池；对可控端点执行逐路时间戳、延迟、ASRC 和声学校准，对系统组合端点使用系统同步并如实显示控制边界。

按这个模型：

- Android/iOS 的系统双耳机功能可以立即获得便利性；
- 任意品牌的设备可以通过“一台节点一个本地输出”获得可控同步；
- macOS/Linux 可以发挥原生多声卡和虚拟端点能力；
- HarmonyOS 可以先成为可靠网络节点，再根据实际开放 API 增加本地多输出；
- 现有 Windows 的同步、漂移跟踪、手机麦克风校准不会被推翻，而会成为所有平台共享的核心竞争力。

## 14. 官方资料与一手来源

### Android 与 Bluetooth

- [Android BluetoothA2dp API](https://developer.android.com/reference/android/bluetooth/BluetoothA2dp)
- [Android BluetoothLeAudio API](https://developer.android.com/reference/android/bluetooth/BluetoothLeAudio)
- [AOSP Combined audio device routing](https://source.android.com/docs/core/audio/combined-audio-routing)
- [Android Bluetooth Low Energy Audio](https://developer.android.com/develop/connectivity/bluetooth/ble-audio/overview)
- [Android AudioTrack API](https://developer.android.com/reference/android/media/AudioTrack)
- [Android Manifest permissions](https://developer.android.com/reference/android/Manifest.permission)
- [Google Pixel Audio Sharing](https://support.google.com/pixelphone/answer/16483797?hl=en)
- [Samsung Dual Audio](https://www.samsung.com/us/support/answer/ANS10003436/)
- [Bluetooth SIG: LE Audio](https://www.bluetooth.com/learn-about-bluetooth/feature-enhancements/le-audio/)
- [Bluetooth SIG: Coordinated Set Identification Profile](https://www.bluetooth.com/specifications/specs/coordinated-set-identification-profile-1-0-1/)

### HarmonyOS / OpenHarmony / Qt Harmony

- [HarmonyOS 应用设计与多端部署](https://developer.huawei.com/consumer/cn/app/planning)
- [鸿蒙电脑应用开发入门](https://developer.huawei.com/consumer/cn/multidevice/pc/get-started/)
- [HarmonyOS 文档中心：Audio Kit、Media Kit、NDK](https://developer.huawei.com/consumer/cn/doc/)
- [OHAudio C/C++ 播放指导](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides-V5/using-ohaudio-for-playback-V5)
- [OpenHarmony Native Audio Routing Manager](https://gitee.com/openharmony/docs/blob/193b97d744efe5fa2588a62811bda5009ddf2319/en/application-dev/reference/apis-audio-kit/native__audio__routing__manager_8h.md)
- [OpenHarmony Media Kit 开发能力索引](https://gitee.com/openharmony/docs/blob/2faa5b48479b16be679f55b49ad56ba9dcb0a2db/en/application-dev/media/media/media-kit-quick-overview.md)
- [OpenHarmony AVSession 系统接口](https://gitee.com/openharmony/docs/blob/43726785b4033887cd1a838aaaca5e255897a71e/en/application-dev/reference/apis-avsession-kit/js-apis-avsession-sys.md)
- [Qt for HarmonyOS](https://wiki.qt.io/Qt_for_HarmonyOS)
- [Building Qt 6 for HarmonyOS](https://wiki.qt.io/Building_Qt6_for_HarmonyOS)
- [Qt 官方：使用 vcpkg 为 HarmonyOS 构建库](https://www.qt.io/blog/building-libraries-for-harmonyos-with-vcpkg)

### Apple

- [Apple Share Audio with AirPods and Beats](https://support.apple.com/en-ie/guide/airpods/dev3786f35c8/web)
- [AVAudioSession MultiRoute](https://developer.apple.com/documentation/avfaudio/avaudiosession/category-swift.struct/multiroute)
- [Apple Audio Session Programming Guide: MultiRoute](https://developer.apple.com/library/archive/documentation/Audio/Conceptual/AudioSessionProgrammingGuide/AudioSessionBasics/AudioSessionBasics.html)
- [AVAudioSession A2DP option](https://developer.apple.com/documentation/avfaudio/avaudiosession/categoryoptions-swift.struct/allowbluetootha2dp)
- [macOS Multi-Output Device](https://support.apple.com/en-in/guide/audio-midi-setup/ams7c093f372/mac)
- [macOS Aggregate Device 与 Drift Correction](https://support.apple.com/en-mide/guide/audio-midi-setup/ams094c7edb4/mac)
- [Apple ScreenCaptureKit](https://developer.apple.com/documentation/screencapturekit)
- [Core Audio Process Tap](https://developer.apple.com/documentation/coreaudio/audiohardwarecreateprocesstap%28_%3A_%3A%29)
- [Apple AudioDriverKit](https://developer.apple.com/documentation/audiodriverkit)
- [Creating an audio device driver](https://developer.apple.com/documentation/AudioDriverKit/creating-an-audio-device-driver)

### Linux / Ubuntu / Qt

- [Ubuntu 22.10 默认采用 PipeWire](https://ubuntu.com/blog/whats-new-in-ubuntu-desktop-22-10-kinetic-kudu)
- [Ubuntu 24.04 LTS Release Notes](https://discourse.ubuntu.com/t/ubuntu-24-04-lts-noble-numbat-release-notes/39890)
- [PipeWire Combine Stream](https://pipewire.pages.freedesktop.org/pipewire/page_module_combine_stream.html)
- [PipeWire Loopback 与虚拟 sink/source](https://pipewire.pages.freedesktop.org/pipewire/devel/page_module_loopback.html)
- [WirePlumber Bluetooth configuration](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/bluetooth.html)
- [PulseAudio module-combine-sink](https://wiki.freedesktop.org/www/Software/PulseAudio/Documentation/User/Modules/)
- [Qt 6.10 Supported Platforms](https://doc.qt.io/qt-6.10/supported-platforms.html)
