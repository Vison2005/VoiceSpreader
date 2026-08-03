# VoiceSpreader 虚拟音频驱动阶段

当前环境先构建和验证用户态 WASAPI 引擎。虚拟音频驱动在同步算法稳定后接入，避免同时调试两个最复杂的部分。

计划结构：

```text
Windows 应用
    ↓
VoiceSpreader Virtual Speaker
    ↓ WASAPI Loopback
VoiceSpreader 用户态引擎
    ↓
多个物理输出设备
```

驱动阶段拟采用：

- Visual Studio 2022
- Windows Driver Kit（WDK）
- SysVAD/PortCls/WaveRT 作为官方参考实现
- 本机自签名测试证书
- Windows Test Mode

驱动只负责暴露虚拟渲染端点、格式、缓冲位置和时钟。多设备分发、延迟补偿、声学校准与自适应重采样全部保留在用户态，以缩小内核代码范围。

在开始该阶段前需要额外安装完整 WDK；当前机器已检测到 Windows SDK，但没有检测到 WDK 驱动构建目录。

