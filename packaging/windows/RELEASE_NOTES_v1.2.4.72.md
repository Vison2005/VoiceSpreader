# VoiceSpreader Windows v1.2.4.72

- 开机自启动改用 MSIX 原生 StartupTask，避免应用升级后旧版 WindowsApps 路径失效。
- 兼容迁移 1.2.4.71 及更早版本已启用的注册表启动项；直接运行未打包程序时保留注册表回退。
- 通过 StartupTask 激活时保持隐藏主窗口，启动后直接驻留系统托盘。
