# VoiceSpreader v1.2.4.3

## 本版重点

- 发布 Windows 与 Android `1.2.4.3`，保留 `v1.2.4.2` 源码与安装包作为历史存档。
- 触摸板恢复跟随 Android 应用明暗主题的背景色，不再强制纯黑；仍保持全屏、无装饰输入面。
- 触摸事件继续复用已认证的音频 TCP 长连接，增加 TCP keep-alive 与触摸帧写失败后的统一重连，避免点击触摸板时重建或污染连接。
- Android 握手声明 `audio` 与 `input.touchpad` 能力；Windows 对 protocol 2 音频会话保留触摸板兼容路径。
- Windows `SendInput` 失败时显示节流后的 Win32 错误提示，便于识别管理员权限/UIPI 等系统限制。

## 触摸板协议约定

手机发送大端序长度前缀：`[4-byte bodyLength][1-byte type][payload]`。指针、按键、滚动帧分别使用 type `30`、`31`、`32`；每帧含递增序号，Windows 丢弃重复或倒退序号。该结构与现有音频会话共用认证、生命周期和重连逻辑，避免每次打开触摸板重新握手。

