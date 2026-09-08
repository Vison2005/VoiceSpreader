# VoiceSpreader protocol 3：输入、文件与扫码接口契约

本文档是 Windows 与 Android 开发的共同契约。实现前必须先按此文档对齐；未定义字段不得自行复用既有音频字段。

## 1. 通用规则

- TCP hello 为 UTF-8 JSON 单行，以 `\\n` 结束。
- hello 至少包含：`type=hello`、`protocol=3`、`session`、`secret`、`deviceId`、`deviceName`、`capabilities`。
- Windows 端仍接受 `protocol=1/2`，但只开放旧音频能力。
- protocol 3 的二进制帧仍为：`u32_be bodyLength` + `body`；body 第 1 字节是 `type`。
- 控制/输入帧上限 256 KiB；文件数据帧上限 1 MiB（专用 file channel）。
- 所有字符串 UTF-8；所有整数网络字节序（大端）；时间使用单调时钟或 UTC，不使用本地格式化时间参与状态判断。
- 每个请求包含 `requestId`；每个输入事件包含单调递增 `sequence`；每个传输包含 `transferId`。
- 当前控制权只有一个 owner。控制连接关闭、认证失败或超时必须撤销 owner。

## 2. Channel

### control channel

承载 hello、功能协商、触摸板、快捷键、扫码、文件元数据和状态。沿用现有配对监听端口。

### file channel

大文件传输建立独立 TCP 连接，仍使用相同 session/secret/deviceId 认证，hello 增加：

```json
{"type":"hello","protocol":3,"channel":"file","transferId":"...","session":"...","secret":"...","deviceId":"...","capabilities":["file"]}
```

file channel 不得发送音频帧；控制连接断开时立即取消该 channel 上的任务。

## 3. capabilities

首版 capability 名称固定为：

```text
audio
input.touchpad
input.shortcut
input.app
file.send
file.receive
camera.capture
scan.camera
scan.gallery
```

双方只使用交集能力。手机未授予 CAMERA、相册或目录权限时，不能在 capabilities 中宣称对应能力。

## 4. 帧类型

| type | 名称 | channel | 说明 |
|---:|---|---|---|
| 30 | `INPUT_POINTER` | control | 指针移动/触摸动作 |
| 31 | `INPUT_BUTTON` | control | 鼠标键按下/释放 |
| 32 | `INPUT_SCROLL` | control | 滚动 |
| 33 | `SHORTCUT_EXECUTE` | control | 快捷键或有限动作 |
| 34 | `SHORTCUT_CATALOG` | control | Windows 白名单应用目录 |
| 40 | `FILE_OFFER` | control | 文件元数据 JSON |
| 41 | `FILE_ACCEPT` | control | 接收端确认 JSON |
| 42 | `FILE_CHUNK` | file | 二进制文件数据 |
| 43 | `FILE_COMPLETE` | control/file | 完成与校验结果 |
| 44 | `FILE_CANCEL` | control/file | 取消传输 |
| 50 | `SCAN_REQUEST` | control | 请求摄像头/相册扫码 |
| 51 | `SCAN_RESULT` | control | 扫码结果 JSON |
| 52 | `SCAN_ACK` | control | 结果处理确认 |
| 53 | `SCAN_CANCEL` | control | 取消扫码 |
| 60 | `CAPTURE_REQUEST` | control | 请求拍照/录像 |
| 61 | `CAPTURE_RESULT` | control | 拍摄任务结果，文件随后走 file channel |

现有音频类型 `1..13` 保持不变，不能重新定义。

## 5. 输入事件

`INPUT_POINTER`、`INPUT_BUTTON`、`INPUT_SCROLL` 使用以下固定二进制 payload（不含外层 `body[0]=type`）：

```text
INPUT_POINTER: sequence(u32_be) + timestamp(u64_be, sender monotonic us)
                + action(u8: 0=DOWN,1=MOVE,2=UP,3=CANCEL)
                + pointerId(u32_be) + deltaX(i32_be, 1/1000 px)
                + deltaY(i32_be, 1/1000 px)
INPUT_BUTTON:  sequence(u32_be) + timestamp(u64_be)
                + button(u8: 1=left,2=right,3=middle,4=back,5=forward)
                + state(u8: 0=up,1=down)
INPUT_SCROLL:  sequence(u32_be) + timestamp(u64_be)
                + deltaX(i32_be, 1/1000 wheel unit)
                + deltaY(i32_be, 1/1000 wheel unit)
```

Windows 必须以 body 长度校验这些布局；非法长度直接丢弃该帧并记录诊断，不得按旧音频帧解析。delta 使用有符号定点整数，Windows 按设备 DPI 和灵敏度换算为 `SendInput` 相对移动。

- 手机文件拖拽开始后，Android 必须暂停发送输入事件。
- 触摸取消、Activity 销毁、控制连接断开时发送释放事件；Windows 端也必须执行本地 panic release。
- 事件应合并高频移动，目标 60–120Hz；丢弃过期移动事件，不丢弃按键按下/释放事件。

## 6. 快捷键

电脑端应用入口使用 `SHORTCUT_CATALOG` 下发 `{ "apps": [{"id":"...","name":"..."}] }`，手机点击后仍复用 `SHORTCUT_EXECUTE`，payload 为 `{ "action":"launchApp", "appId":"..." }`。Windows 端只执行本地设置中存在且路径仍有效的白名单应用，不接受手机直接传入可执行路径。

`SHORTCUT_EXECUTE` 使用 UTF-8 JSON，示例：

```json
{"requestId":"...","name":"切换窗口","modifiers":["ALT"],"key":"TAB","repeat":1}
```

首版只允许白名单 modifier/key；`repeat`、动作数量和总时长受限。Windows 执行成功或拒绝都返回状态；断线时释放所有注入键。

## 7. 文件传输

`FILE_OFFER` 示例（control channel JSON）：

```json
{"transferId":"...","items":[{"itemId":"...","name":"photo.jpg","mime":"image/jpeg","size":1048576,"modifiedAt":0}]}
```

文件名必须去除路径穿越和控制字符，Windows 端重新生成安全目标路径。

`FILE_CHUNK` payload：`itemId(16 bytes)` + `offset(u64_be)` + `data`。chunk 建议 256 KiB–1 MiB；不使用 Base64、不对已压缩媒体重复压缩、不将整个文件读入内存。

完成后发送 SHA-256；接收端校验失败不得报告成功。断点续传从 `offset` 继续；URI 不可 seek 时由 Android 重新打开并跳过已确认字节。

文件拖拽目标是 VoiceSpreader 自己的“共享托盘/触摸板共享区”，不是 Android 系统剪贴板。文件落地 Windows 后，可选把本地文件路径放入 Windows 文件剪贴板供 `Ctrl+V` 使用。

传输顺序固定为：

```text
Android control: FILE_OFFER
Windows control: FILE_ACCEPT {transferId, itemId, acceptedOffset}
Android file channel: hello(channel=file, transferId)
Android file channel: FILE_CHUNK...
Android control: FILE_COMPLETE {transferId, itemId, sha256}
Windows control: FILE_COMPLETE/ACK
```

同一个 `transferId` 可以包含多个 `itemId`，但首版按 item 顺序发送；不得为一个 item 建立多个并行流。`acceptedOffset` 为 0 表示从头开始。

## 8. 扫码

`SCAN_REQUEST`：

```json
{"requestId":"...","mode":"camera|gallery","formats":["QR_CODE"],"autoOpen":false}
```

`SCAN_RESULT`：

```json
{"requestId":"...","rawValue":"https://example.com","format":"QR_CODE","valueType":"url"}
```

手机端优先本地识别；相册扫码默认只发送结果。Windows 只自动打开 `http`/`https`，其他 payload 显示并允许复制。后台请求扫码时通过通知触发用户操作。

## 9. 拍照/录像

`CAPTURE_REQUEST` 只负责授权和拍摄任务，拍摄产生的 JPEG/视频文件通过 file channel 传输。

- 照片使用 CameraX 内存捕获，成功后立即发送。
- 视频首版为实验能力，优先边编码边传输，避免手机保存完整视频。
- 摄像头启动、停止、取消和 Activity 销毁必须幂等。
