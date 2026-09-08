# SoundSpreader Windows 安装

此 MSIX 使用项目自签名证书签署。首次安装前，需要由管理员将随安装包提供的公开证书加入“本地计算机 > 受信任人”；私钥不会上传到 GitHub。

以管理员身份打开 PowerShell，执行：

```powershell
Import-Certificate `
  -FilePath .\SoundSpreader-v{{VERSION}}.cer `
  -CertStoreLocation Cert:\LocalMachine\TrustedPeople

Add-AppxPackage .\SoundSpreader-v{{VERSION}}-x64.msix
```

不要把测试证书放入“受信任的根证书颁发机构”。不再测试 VoiceSpreader 时，可从 `certlm.msc` 的“本地计算机 > 受信任人”中删除 `Vison2005` 证书。

统一测试包同时提供 `Install SoundSpreader.cmd`：脚本会先核对 SHA-256 和签名指纹，再通过 UAC 导入公开证书、验证 MSIX 签名并安装应用。证书和测试包只应从可信发送者处获取。

安装包仅支持 x64 Windows 10 2004（内部版本 19041）及以上系统。自签名证书只适合私有分发；Windows 仍可能显示未知发布者或信誉提示。
