# SMS Forwarder Board

面向 ESP32-C3 与 ML307 系列蜂窝模组的短信网关固件、Web 管理台和 Windows 批量量产工具。

这是由 [sukiyra](https://github.com/sukiyra) 独立维护和发布的工程仓库，GitHub 仓库本身不是 Fork。固件、管理台、设备自检协议、版本发布和量产流程在同一个仓库维护。

## 主要能力

- 自动识别 ML307A、ML307C、ML307R、ML307Y，并按模组能力选择短信上报方式。
- 国内版 ML307C 可在移动、联通、电信网络注册；本产品的短信收发只承诺中国移动和中国联通，电信网络仍可用于数据连接但界面会明确标记短信不可用。
- 支持实体 SIM 与 eSIM/eUICC；白卡初始化错误会自动容错，首页分别显示卡识别、短信配置和蜂窝注册状态。
- 接收和主动发送短信；短信使用固定 50 条循环存储，不会无限占用 Flash。
- 支持企业微信、飞书、Bark、电子邮件、钉钉、PushPlus、Server 酱、Gotify、Telegram 及自定义 HTTP 推送。
- 可扫描运营商、严格手动选网和恢复自动选网；目标网络拒绝时自动恢复可用网络。
- 初始化引导自动扫描附近 Wi-Fi，可选择网络、输入密码并在保存前测试连接；系统设置中也可随时更换主备网络。
- Wi-Fi 断线时自动保活并轮询主备网络；持续无法恢复会自动开启 `sms-forwarder` 热点进入手动配网。
- 人可读通知统一显示北京时间；开放 API 与本地记录继续使用 UTC ISO-8601，便于程序可靠解析和排序。
- 响应式 Web 管理台，适配手机、普通桌面和超宽屏；顶部常驻 Wi-Fi 信号强度，跨页面保留 OTA、选网、切卡和短信发送进度。
- Windows 量产工具支持在线选择 GitHub Release 版本、多串口并行烧录和逐台验收。
- Windows 局域网发现工具可自动列出同网段设备，并一键打开对应管理页面。
- 支持从 GitHub Releases 在线检查和安装 OTA 更新；通过资产 API 绕过部分网络对 GitHub 下载页的连接限制，分段下载时持续回报字节数和进度，并使用双应用分区、HTTPS 证书校验、SHA-256 校验与启动失败自动回滚。
- 每台设备输出 CSV 与 JSON 量产记录，不记录短信正文、Wi-Fi 密码或推送密钥。

## 下载与烧录

打开仓库的 [Releases](https://github.com/sukiyra/sms-forwarder-board/releases)，每个版本提供三个压缩包和一个 OTA 固件：

- `sms-forwarder-firmware-vX.Y.Z.zip`：带 SHA-256 清单的 ESP32-C3 固件。
- `sms-forwarder-production-tool-windows-vX.Y.Z.zip`：Windows 单文件量产工具与 `esptool.exe`。
- `sms-forwarder-lan-scanner-windows-vX.Y.Z.zip`：自动发现局域网设备并打开管理台。
- `sms-forwarder-ota-vX.Y.Z.bin`：设备在线升级使用的应用镜像。

普通用户只需下载 Windows 工具包并解压：

1. 双击 `sms-forwarder-production-tool.exe`。
2. 在“GitHub 版本”中选择要烧录的版本。
3. 插入一块或多块设备，点击“刷新串口”。
4. 选择设备后开始烧录。
5. 只把显示 `PASS` 的设备作为通过品。

在线固件会缓存在 `%LOCALAPPDATA%\SMS Forwarder Production Tool\firmware`。工具会验证 Release 压缩包结构、清单版本和每个镜像的 SHA-256；断网时可以切换到本地固件目录。

## 局域网设备发现

升级到 v1.6.0 或更高版本后，解压并运行 `sms-forwarder-lan-scanner.exe`。工具会扫描电脑的 IPv4 局域网，列出设备 IP、固件版本、模组型号、OTA 能力、MAC 和设备 ID；双击设备即可打开管理台。

发现协议使用 UDP 37888 和每次扫描生成的随机 nonce。设备只回应同一子网请求，并限制回复频率。报文不包含手机号、ICCID、短信、Wi-Fi 密码、登录密码或推送密钥。路由器开启 AP 隔离或访客网络隔离时，设备之间无法互相发现。

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
  --build-property upload.maximum_size=1966080 `
  --output-dir .\build `
  .\code
```

仓库内的 `code/partitions.csv` 定义 4 MB Flash 双应用分区：`app0` 与 `app1` 各 1,920 KB，并保留 NVS、OTA 状态、128 KB LittleFS 短信存储和 64 KB 崩溃转储分区。不要用 Arduino 菜单中的单应用分区覆盖它。

## 在线 OTA

在“系统设置 → 在线固件升级”中检查并安装正式版。设备只接受本仓库最新 GitHub Release 中名称为 `sms-forwarder-ota-vX.Y.Z.bin` 的资产，并验证以下条件：

- HTTPS 证书链有效，设备时间已通过 NTP 同步。
- 目标版本高于当前版本，不允许在线降级。
- 文件名称、Content-Length、Release 资产大小和备用分区容量一致。
- 下载完成后的 SHA-256 与 GitHub Release 资产摘要一致。
- ESP32 镜像头和写入完整性通过 Update/bootloader 校验。

固件始终写入非活动分区。下载中断、摘要不符或写入失败不会切换启动分区；新版本首次启动必须完成配置、WiFi、Web 服务和模组初始化，之后才会确认镜像。此前发生崩溃或看门狗复位时，bootloader 会回滚到旧版本。

从 v1.4.3 及更早的单应用分区升级时，必须先用 v1.5.0 或更高版本的量产包通过 USB 完整烧录一次分区表。完成这次迁移后，后续版本可直接在线升级。需要保留现有配置时，在量产工具中关闭“全片擦除”；NVS 地址保持不变。

## 量产工具命令行

```powershell
# 查询 GitHub 可烧录版本
python .\factory\production_tool.py --list-versions

# 选择指定 Release，并烧录所有自动识别的 ESP32 串口
python .\factory\production_tool.py --all --version v1.6.0 --require-sim --require-network

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
4. 固件清单、USB 烧录 ZIP 与 OTA 应用镜像打包。
5. Windows 单文件量产工具和局域网发现工具构建。
6. 创建 GitHub Release 并上传固件包、OTA 镜像、量产工具和局域网发现工具。

## 许可证与来源

固件、Web 管理台和量产工具使用 [MIT License](LICENSE)。新仓库由 sukiyra 独立维护；早期 MIT 代码的版权声明和来源记录保存在 [NOTICE](NOTICE) 中。提交修复和功能前请确保不包含 SIM 完整 ICCID、手机号、Wi-Fi 密码或推送密钥。
