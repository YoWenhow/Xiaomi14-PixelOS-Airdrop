# 从 PixelOS、KSU 到 AirDrop

当前已装好并验收的手机不需要重刷。本教程只覆盖 README 列出的 houji / 20261021-0252 / B 槽基线。安装前备份需要保留的数据，保留作者原版镜像与回退工具。

## 1. 原作者 PixelOS

自行取得 `Fastboot_PixelOS_houji-17.0-20261021-0252.zip`，SHA256 必须为：

    d0fd69144afd03bff062b5605cd5717a2903ab16aaa4a45bd0d11cb9fc7d8da6

Bootloader 已解锁，在系统开启 USB 调试、连接并授权电脑。使用 Android platform-tools，先核对 `adb devices` 中的序列号；下面 `$Serial` 必须替换为自己的设备，不要照抄他人的序列号。

```powershell
$Serial = 'YOUR_DEVICE_SERIAL'
$Rom = 'C:\ROM\Fastboot_PixelOS_houji-17.0-20261021-0252.zip'
if ((Get-FileHash $Rom -Algorithm SHA256).Hash.ToLower() -ne 'd0fd69144afd03bff062b5605cd5717a2903ab16aaa4a45bd0d11cb9fc7d8da6') { throw 'ROM不匹配' }
adb -s $Serial reboot bootloader
fastboot -s $Serial getvar product
fastboot -s $Serial getvar current-slot
fastboot -s $Serial getvar unlocked
fastboot -s $Serial getvar snapshot-update-status
```

逐项确认 `houji`、`b`、`yes`、`none`。任何一项不同就停止；本教程不提供强行切槽或跨 ROM 的办法。

以下是**清空全新安装**，`-w` 清除 userdata 和 metadata。保留数据升级不使用这条命令：

```powershell
fastboot -s $Serial --slot=b --skip-secondary --skip-reboot --disable-super-optimization -w update $Rom
if ($LASTEXITCODE -ne 0) { throw '刷机失败，不继续' }
```

过程中会进入 fastbootd，需要电脑能识别。失败时处理驱动/连接并检查日志，不额外乱刷分区；此方案不需要永久刷入旧实验 C2 vendor_dlkm。

## 2. KernelSU Root

使用 KernelSU 官方 **3.3.1**，并用本版作者单独提供的原始 `init_boot` 修补。原始输入 SHA256：

    d16f72b8a4a47cab5f41017310b5de063167b35f4e5d37ae521f81f86d8a3f0f

Google、三星固件与作者 init_boot 均未附在公开 Release。到 [KernelSU 官方项目](https://github.com/tiann/KernelSU/releases/tag/v3.3.1) 取得管理器，按 [官方 LKM 安装说明](https://kernelsu.org/guide/installation.html) 选择文件修补，在管理器中选择上述 `init_boot` 生成修补镜像，保留原内核并备份原始输入。若先启动原版系统完成修补，需要再进入 bootloader。

本项目参考修补产物 SHA256 为 `0a3aee32d20d049c66a9bdabc022116b76aa73b996bdc0ec53fe06965476f28d`，公开包不附该镜像。自己重建的镜像可能因构建元数据不同而改变哈希，必须核对输入、KSU 版本及启动结果，不能直接视为同一文件。

确认 ROM 刷写成功、`current-slot` 仍是 `b` 后才执行；把路径替换为自己的修补产物：

```powershell
fastboot -s $Serial getvar current-slot
fastboot -s $Serial flash init_boot_b 'C:\ROM\ksu-patched-init_boot.img'
if ($LASTEXITCODE -ne 0) { throw 'Root镜像刷写失败，不继续' }
fastboot -s $Serial reboot
```

完成初始设置、连上 Wi-Fi，安装同版本 KSU 管理器，首页必须显示“工作中 / Working”。先确认原版 Wi-Fi 和网页正常，再继续。

## 3. 模块安装

1. 按 [本地组装](BUILD-zh.md) 生成 `KSU-module.zip`。它才是可在 KSU 安装的文件。
2. KSU → 模块 → 安装 → 选择该 ZIP，安装成功后完整重启。
3. 解锁，在 KSU 超级用户中只给“AirDrop 实验分享”（`org.houji.airdrop`）授予 Root。
4. 打开配套 App，点“检查 Root / 恢复状态”，通过后点“开启系统 Quick Share 自动接入”。

模块会安装配套 App 和用户提供的原版 Google 扩展。若拒绝已有模块、metadata、APK 或哈希不匹配，先按 [升级/卸载](UPDATES-zh.md) 处理，不跳过校验、不手工删除账本。

日常使用无需电脑、USB 调试或 ADB Root，可关闭这些调试入口；需保留配套 App 和它的 KSU Root 授权。App 显示 0.5.1-preview.1，模块保留 0.5.1-candidate.1 的参考内部版本名。

## 4. 使用与首次验收

- 小米 → iPhone：iPhone 解锁，AirDrop 设为“所有人，10 分钟”；小米在文件管理器/相册选择文件 → 分享 → 系统 Quick Share → iPhone → 对端接受。
- iPhone → 小米：小米打开系统 Quick Share“接收”页；iPhone 分享文件 → AirDrop → 小米 → 小米接受。

无需先打开辅助 App。每轮完成后退出分享页，等普通 Wi-Fi 恢复，再开始下一轮。

首次至少验证：图片、普通文档/ZIP能实际打开；不重启再反向传一次；至少一次拔掉 USB；确认结束后 Wi-Fi 恢复并能打开网页。仅进度到 100% 不能算完成。

若 iPhone 文档进度走满却失败，检查 iPhone“设置 → App → Safari → 下载”是否指向有效的“我的 iPhone/下载”。参考设备曾由修正错误下载目录解决，照片路径不受同一问题影响。
