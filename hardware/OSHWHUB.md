# 立创开源平台发布资料

## 项目标题

SMS Forwarder Board：ESP32-C3 + ML307A 4G 短信转发板

## 简介

一块 USB 棒形的 ESP32-C3 + ML307A Cat.1 短信转发器硬件。板上集成独立 3.8 V 同步降压模组电源、1.8 V UART 电平转换、Nano SIM、LTE/GNSS U.FL、USB 原生烧录与量产测试点，可直接运行本仓库的短信转发固件。

## 说明

本项目从器件数据手册与现有固件接口重新设计，提供 KiCad 源文件、Gerber、BOM、坐标文件、装配与首板验证说明。Rev 1.0 只适配 ML307A 94-pin 封装；ML307C/Y 为 109-pin 封装，需要单独 PCB 变体。

建议首轮打 5 片、只贴 2 片。ML307 发射峰值接近 2 A，必须使用 5 V/2 A 电源；普通电脑 USB 2.0 口限流时会导致掉网和短信异常。

当前 Rev 1.0 已通过 KiCad ERC/DRC，但尚未完成实体首板验证。发布状态应标记为“工程样板/待验证”，实体测试通过前不要标记为量产验证完成。

## 发布附件

- `sms_forwarder_ml307a.kicad_sch`、`sms_forwarder_ml307a.kicad_pcb`、`sms_forwarder_ml307a.kicad_pro`
- `SMS-Forwarder-ML307A-Gerber.zip`
- `BOM_JLCPCB.csv`、`BOM_Full.csv`、`SMS-Forwarder-BOM.xlsx`
- `CPL_JLCPCB.csv`
- `schematic.pdf`、`pcb-layout.pdf`、`pcb-top.png`、`pcb-bottom.png`
- `ASSEMBLY.md`、`NOTICE`、`LICENSE-CERN-OHL-P-2.0.txt`

## PCB 参数

40 × 84 mm、4 层、1.6 mm、外层 1 oz、内层 0.5 oz，推荐 JLC04161H-7628。最小孔 0.60/0.30 mm。LTE/GNSS 按 50 Ω 单端、USB 按 90 Ω 差分在下单阻抗计算器复核。

## 开源协议

CERN Open Hardware Licence Version 2 – Permissive（CERN-OHL-P-2.0）

## 标签

ESP32-C3、ML307A、Cat.1、短信转发、4G、Nano SIM、开源硬件、KiCad
