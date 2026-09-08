# VoiceSpreader Windows v1.2.4.3

- Windows 软件版本号更新为 `1.2.4.3`。
- 继续接收 protocol 2 音频会话上的触摸板 type `30/31/32` 输入帧，并按序列去重。
- `SendInput` 返回失败时在状态提示中显示 Win32 错误码，便于排查目标程序以管理员权限运行等 Windows 输入隔离问题。

