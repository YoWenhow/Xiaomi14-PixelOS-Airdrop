# Xiaomi 14 PixelOS AirDrop

让小米 14（houji）在指定 PixelOS 移植版上，通过系统 **Quick Share** 与 iPhone **AirDrop** 双向分享文件。

首个公开版本：**v0.5.1-preview.1**。它对应已完成实机双向验收的整合版本，仍是精确基线的预览版，不是任意 Android 手机或任意 PixelOS 的通用模块。

## 已实现

- 从系统 Quick Share 发送、接收页面自动启动，退出会话后恢复原厂 Wi-Fi，正常结束不重启。
- 脱离电脑连续双向传输；图片、Word、ZIP 已验收，128 MiB ZIP 可解压并打开其中的 Word。
- 不再受 64 KiB 或整场 3 分钟限制：等待设备最多 3 分钟，实际传输最多 30 分钟。
- 会话期间临时取得真实国家码所需权限，结束/开机/卸载按账本恢复；未修改 Google APK 或在线配置。
- 对系统重新开启 Wi-Fi 的竞争进行同步检查；尚未动驱动的失败原地恢复，无法安全恢复时仍保留保护重启。

## 精确支持范围

| 项目 | 已验收基线 |
|---|---|
| 手机 | Xiaomi 14 / houji，已解锁 Bootloader |
| ROM | `Fastboot_PixelOS_houji-17.0-20261021-0252.zip` |
| 槽位 | B（不是要求所有手机强行切 B） |
| 内核 | `6.1.138-android14-11-g0c3d559bcd85-ab14529422` |
| vendor_dlkm 增量 | `1792522354` |
| KernelSU | 3.3.1，另核验 ksud 哈希 |
| Google AirDrop 扩展 | `com.google.android.mosey` 20065 / 1.0.922413470 |
| 最近验收的 Play 服务 | 26.41.31 |
| 对端 | 用户报告的 iOS 27.0.1，未据此保证其他版本 |

设备指纹可能经过 ROM 伪装，不能单凭指纹认定兼容。完整哈希见 [profile](profiles/pixelos-houji-20261021-0252-b.json)。

## 下载与使用

先读 [从 PixelOS、KSU 到模块安装](docs/INSTALL-zh.md)，再看 [本地组装](docs/BUILD-zh.md)。

公开 Release 提供源码和仅含可再分发代码的运行组件。**不提供含 Google 闭源组件、三星固件或作者 init_boot 的一键刷入整合包**。这些文件由使用者自行合法取得；工具只接受列明的 SHA256，不下载、不替换、不刷机。

在本地补齐依赖后，组装工具生成 `KSU-module.zip` 和 `companion.apk`。Release 的源码包、运行组件包均不能直接刷入 KSU。

## 项目结构

| 目录 | 内容 |
|---|---|
| `app/` | 配套 Android App、Quick Share/独立发送会话控制脚本 |
| `module/` | KSU 安装、系统接入、权限账本、回滚与卸载模板 |
| `observer/` | 系统 Quick Share 前台入口检测 |
| `native/` | Binder/无线参数兼容层、控制及发现工具 |
| `kernel/` | WLAN 相对固定上游的完整改动、Wonder、桥接、内核配置与 ABI 基线 |
| `profiles/` | ROM 与依赖哈希 |
| `tools/` | 本地组装、校验、源码还原、归档工具 |
| `tests/` | 不接触手机的失败回滚与控制流程测试 |

## 使用边界

分享时暂时占用无线组件，普通 Wi-Fi 会中断，结束后恢复；吞吐尚未达到 Apple 原生设备水平。异常可能触发保护重启。请先在可恢复的设备上验证。

本次开源整理验证了本地组装与源码/产物对应关系。**本地重签名生成的新 APK/模块没有再次上机测试**；已有实机结论对应整理前的参考整合包。区别及验证记录见 [验收说明](docs/VALIDATION.md)。

ROM、KSU、Mosey 更新不能假设自动兼容；升级流程见 [更新与恢复](docs/UPDATES-zh.md)。不依赖关闭 SELinux，不承诺规避应用的 Root 或环境检测。

## 许可与贡献

项目新增、未另行标注的代码以 GPL-2.0-only 发布；已有 SPDX、版权与上游许可保持不变，详见 [许可与来源](THIRD_PARTY_NOTICES.md)。Google、Apple、Xiaomi、Samsung 商标属于各权利人，本项目不代表这些厂商。

提交问题时提供 ROM/内核/模块版本、方向、复现步骤和已脱敏的错误片段；不要上传私钥、完整 bugreport、账户标识或文件内容。源码发布不包含开发者的设备日志、签名密钥或第三方反编译源码。
