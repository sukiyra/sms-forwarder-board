# Windows 批量量产工具

量产工具用于在线选择 GitHub Release 固件、并行烧录多块 ESP32-C3，并通过 USB 工厂协议验收 ESP32、ML307、SIM、短信配置和蜂窝注册。

## 直接使用发布包

1. 从 [GitHub Releases](https://github.com/sukiyra/sms-forwarder-board/releases) 下载 `sms-forwarder-production-tool-windows-vX.Y.Z.zip`。
2. 解压整个压缩包。
3. 双击 `sms-forwarder-production-tool.exe`。
4. 等待“GitHub 版本”下拉框加载，选择要量产的版本。
5. 插入设备并刷新串口，可一次多选并行烧录。
6. 根据产品要求勾选“要求 SIM 就绪”和“要求成功驻网”。
7. 点击“开始所选设备”，只接收显示 `PASS` 的设备。

工具从 `sukiyra/sms-forwarder-board` 的 Releases 下载包含 `firmware` 的 ZIP。下载后会校验压缩包路径、版本号、ESP32-C3 芯片类型以及清单内每个镜像的 SHA-256。

下载缓存位于：

```text
%LOCALAPPDATA%\SMS Forwarder Production Tool\firmware
```

GitHub 暂时不可用时，选择“本地固件目录”即可离线烧录。

## 擦除策略

- 开启“全片擦除”：清除旧 Wi-Fi、推送配置和短信记录，适合正式出厂。
- 关闭“全片擦除”：保留数据分区，适合返修或固件升级。

## 量产记录

记录保存在程序目录的 `records` 文件夹：

- `production.csv`：一行一台，可用 Excel 打开。
- 独立 JSON：保留该设备完整工厂自检结果。

记录包含芯片 ID、固件版本、ML307 完整料号、SIM 类型、ICCID 尾号和网络 PLMN，不包含短信正文、Wi-Fi 密码或推送密钥。

## 从源码启动

```powershell
.\factory\start.ps1
```

首次运行会安装固定版本的 `pyserial`。

## 命令行

```powershell
# 列出串口
.\factory\start.ps1 --list

# 列出 GitHub 版本
.\factory\start.ps1 --list-versions

# 在线选择指定版本量产
.\factory\start.ps1 --all --version v1.3.0 --require-sim --require-network

# 在线选择最新版本并保留配置
.\factory\start.ps1 --ports COM3 COM4 --version latest --keep-data

# 使用本地固件目录
.\factory\start.ps1 --ports COM3 --firmware-dir .\factory\dist

# 只验收已烧录设备
.\factory\start.ps1 --ports COM3 --skip-flash --require-sim --require-network
```

可用 `--github-repo owner/repository` 指定其他兼容仓库；Release 必须包含固件 ZIP，压缩包内必须有唯一的 `manifest.json`。
