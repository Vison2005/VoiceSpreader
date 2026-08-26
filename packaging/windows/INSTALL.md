# VoiceSpreader Windows 安装

此 MSIX 使用项目自签名证书签署。首次安装前，需要先信任随安装包提供的公开证书；私钥不会上传到 GitHub。

```powershell
Import-Certificate `
  -FilePath .\VoiceSpreader-v1.1.0.cer `
  -CertStoreLocation Cert:\CurrentUser\TrustedPeople

Add-AppxPackage .\VoiceSpreader-v1.1.0-x64.msix
```

也可以双击 `.cer`，将证书安装到“当前用户”的“受信任人”存储区，然后双击 `.msix` 安装。

安装包仅支持 x64 Windows 10 2004（内部版本 19041）及以上系统。自签名证书只适合私有分发；Windows 仍可能显示未知发布者或信誉提示。
