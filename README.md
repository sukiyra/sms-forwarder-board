# SMS Forwarder Board

面向 ESP32-C3 与 ML307 系列蜂窝模组的短信网关固件、Web 管理台和 Windows 批量量产工具。

这是由 [sukiyra](https://github.com/sukiyra) 独立维护和发布的工程仓库，GitHub 仓库本身不是 Fork。固件、管理台、设备自检协议、版本发布和量产流程在同一个仓库维护。

## 主要能力

- 自动识别 ML307A、ML307C、ML307R、ML307Y，并按模组能力选择短信上报方式。
- 支持实体 SIM 与 eSIM/eUICC，首页显示号码、卡类型、ICCID 尾号、PLMN、漫游与注册状态。
- 接收和主动发送短信；短信使用固定 50 条循环存储，不会无限占用 Flash。
- 支持企业微信、飞书、Bark、电子邮件、钉钉、PushPlus、Server 酱、Gotify、Telegram 及自定义 HTTP 推送。
- 可扫描运营商、严格手动选网和恢复自动选网；目标网络拒绝时自动恢复可用网络。
- 初始化引导自动扫描附近 Wi-Fi，可选择网络、输入密码并在保存前测试连接；系统设置中也可随时更换主备网络。
- Wi-Fi 断线时自动保活并轮询主备网络；持续无法恢复会自动开启 `sms-forwarder` 热点进入手动配网。
- 人可读通知统一显示北京时间；开放 API 与本地记录继续使用 UTC ISO-8601，便于程序可靠解析和排序。
- 响应式 Web 管理台，适配手机、普通桌面和超宽屏。
- Windows 量产工具支持在线选择 GitHub Release 版本、多串口并行烧录和逐台验收。
- 每台设备输出 CSV 与 JSON 量产记录，不记录短信正文、Wi-Fi 密码或推送密钥。

## 下载与烧录

打开仓库的 [Releases](https://github.com/sukiyra/sms-forwarder-board/releases)，每个版本提供两个压缩包：

- `sms-forwarder-firmware-vX.Y.Z.zip`：带 SHA-256 清单的 ESP32-C3 固件。
- `sms-forwarder-production-tool-windows-vX.Y.Z.zip`：Windows 单文件量产工具与 `esptool.exe`。

普通用户只需下载 Windows 工具包并解压：

1. 双击 `sms-forwarder-production-tool.exe`。
2. 在“GitHub 版本”中选择要烧录的版本。
3. 插入一块或多块设备，点击“刷新串口”。
4. 选择设备后开始烧录。
5. 只把显示 `PASS` 的设备作为通过品。

在线固件会缓存在 `%LOCALAPPDATA%\SMS Forwarder Production Tool\firmware`。工具会验证 Release 压缩包结构、清单版本和每个镜像的 SHA-256；断网时可以切换到本地固件目录。

## 支持的硬件

| 部件 | 要求 |
| --- | --- |
| 主控 | ESP32-C3，4 MB Flash |
| 开发板配置 | MakerGO ESP32 C3 SuperMini |
| 蜂窝模组 | ML307A / ML307C / ML307R / ML307Y AT 固件 |
| 串口 | ESP32 GPIO3 → ML307 RX；GPIO4 ← ML307 TX |
| 模组使能 | ESP32 GPIO5 → ML307 EN |
| 供电 | 需满足模组发射瞬时电流，主控与模组共地 |

不同 ML307 完整料号支持的频段和运营商不同。固件能够适配 AT 指令差异，但不能为硬件增加缺失的射频频段。采购和量产前必须按完整丝印、模组规格书、SIM 套餐及当地网络实测。

## 首次启动

设备没有可用 Wi-Fi 配置时会开启 `sms-forwarder` 配置热点。连接后访问 `http://192.168.4.1`，初始化引导会自动扫描附近网络；选择 Wi-Fi、输入密码并完成连接测试后保存。设备接入局域网后，通过路由器分配的 IP 打开管理台。

运行期间断线时，设备先使用 ESP32 自动重连保活。持续 2 分钟仍未恢复会按顺序非阻塞尝试最多 5 个已保存网络，每个网络最多等待 12 秒；全部失败后自动恢复 `sms-forwarder` 配置热点。此时重新连接热点即可修改 Wi-Fi，不需要清空其他系统配置。

默认管理员账号为 `admin`，初始密码为 `admin123`。首次登录后请立即在“系统设置”中修改密码。管理页面使用局域网 HTTP，不应直接映射到公网。

## 手动编译

需要 Arduino CLI、ESP32 Arduino Core 3.3.11，以及以下库：

- `pdulib@0.5.11`
- `ReadyMail@0.4.2`
- `ArduinoJson@7.4.2`

PowerShell：

```powershell
arduino-cli compile `
  --fqbn esp32:esp32:makergo_c3_supermini `
  --board-options PartitionScheme=no_ota `
  --output-dir .\build `
  .\code
```

4 MB Flash 必须使用 `PartitionScheme=no_ota`，为嵌入式 Web 管理台和 LittleFS 留出空间。

## 量产工具命令行

```powershell
# 查询 GitHub 可烧录版本
python .\factory\production_tool.py --list-versions

# 选择指定 Release，并烧录所有自动识别的 ESP32 串口
python .\factory\production_tool.py --all --version v1.4.1 --require-sim --require-network

# 使用最新 Release，保留设备已有配置
python .\factory\production_tool.py --ports COM3 --version latest --keep-data

# 只运行工厂自检
python .\factory\production_tool.py --ports COM3 --skip-flash --require-sim --require-network
```

更多操作见 [量产工具说明](factory/README.md)。协议与模块结构见 [开发文档](dev_doc/README.md)。

## 发布流程

推送 `v*` 标签会自动执行：

1. Python 与 C++ 测试。
2. 嵌入式 JavaScript 语法检查。
3. ESP32-C3 固件编译。
4. 固件清单与 ZIP 打包。
5. Windows 单文件量产工具构建。
6. 创建 GitHub Release 并上传两个版本包。

## 许可证与来源

项目使用 [MIT License](LICENSE)。新仓库由 sukiyra 独立维护；早期 MIT 代码的版权声明和来源记录保存在 [NOTICE](NOTICE) 中。提交修复和功能前请确保不包含 SIM 完整 ICCID、手机号、Wi-Fi 密码或推送密钥。
