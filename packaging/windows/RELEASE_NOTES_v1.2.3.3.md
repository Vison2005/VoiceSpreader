# VoiceSpreader Windows v1.2.3.3

## 局域网主动唤醒
- MSIX 增加 `privateNetworkClientServer` 能力，允许 Windows 在家庭/工作网络上接收 Android TCP 连接并发送 UDP 唤醒包。
- 保留已保存手机的名称、地址和最近连接时间，重启后可从已保存设备直接发起连接。
- 主动唤醒同时发送到保存地址和本机各网卡定向广播地址，兼容 DHCP 地址变化。

## 版本
- 应用内版本号统一为 `1.2.3.3`。
