# VoiceSpreader v1.2.0

此版本开始建设独立于本机多设备同步的“设备互联”功能。

## 当前可用

- 新增同步控制与设备互联双工作区。
- 检测用户自行安装的 VB-CABLE 播放端点。
- 将 Android 手机的 48 kHz 单声道麦克风流实时输出到 `CABLE Input`。
- 通过独立 WASAPI Loopback 将 Windows 声音以 48 kHz PCM16 双声道发送到手机应用内播放。
- 使用有界低延迟发送队列；网络拥塞时丢弃旧音频，避免延迟持续累积。
- 路由拥有独立的启动、停止、音量和缓冲生命周期，不影响本机多设备同步。

## 使用要求

- Android 与 Windows 设备处于可互通的局域网。
- Windows 已从 VB-Audio 官方渠道安装 VB-CABLE。
- 录音软件选择 `CABLE Output` 作为录音输入。
