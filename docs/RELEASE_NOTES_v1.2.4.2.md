# VoiceSpreader v1.2.4.2

## 最小稳定触摸板

- Windows 与 Android 版本号统一为 `1.2.4.2`。
- Android 端回归纯黑全屏触摸板，仅保留单指移动、单击和双指滚动。
- 触摸板复用已经认证的音频 TCP 连接；Windows 兼容没有 capability 列表的旧版 protocol 2 手机。
- 快捷键、文件、扫码、拍照和应用目录入口暂时不参与握手；输入发送异常不会主动重置连接。
- Windows/Android 的 v1.2.4.1 源码与发布物保存在 `archive/versions/v1.2.4.1`，历史日志继续保留。
