# 更新与恢复

| 更新对象 | 操作 |
|---|---|
| 作者 ROM | 先结束会话、暂停自动入口，正常卸载模块、重启并归档；按作者说明升级，用新 ROM 对应 init_boot 恢复 Root。确认原版 Wi-Fi 正常后，只安装明确适配新基线的模块。不能复用旧 Root 镜像或跳过驱动/槽位校验。 |
| Google Play 服务 | 登录账户或更新后做一次连续双向小文件和恢复测试。当前修复已覆盖 26.41.31 的国家码获取变化，无需清账户/清数据/降级。未来配置下发仍可能改变行为。 |
| Google Mosey 扩展 | 本版固定版本和 APK 哈希，变动后暂停自动入口并记录版本，等待兼容核验。不要自行修改在线配置或强行替换 APK。 |
| 本项目模块 | 同 ROM 下也先结束会话、暂停自动入口；同签名新 App 可先覆盖安装。KSU 正常卸载旧模块、重启、归档，再安装新模块并重启、自检、开启入口、双向验收。当前不支持直接覆盖旧模块及账本。 |
| KSU | 同时核验 ksud 哈希，不能假设管理器/内核组件升级后仍兼容。先停止分享并确认匹配支持。 |

首次自建 APK 的签名与参考开发包不同，不能直接覆盖；以后请保留自己的 `private/` 签名资料。不要把签名密钥上传 GitHub。

## 标准卸载与归档

1. 结束分享、等 Wi-Fi 恢复，在配套 App 暂停自动接入。
2. KSU 卸载 `houji_qs_system79`，完整重启。暂时保留 KSU、配套 App、Google 扩展供回滚收尾。
3. 解锁，维护时打开 USB 调试和 KSU ADB Root，授权电脑。`adb shell id -u` 应为 0；否则停止。
4. 使用本仓库工具归档，不手工删 `/metadata`：

```powershell
$Serial = 'YOUR_DEVICE_SERIAL'
adb -s $Serial shell id -u
adb -s $Serial push tools/archive-retired.sh /data/local/tmp/houji-archive-retired.sh
adb -s $Serial shell sh /data/local/tmp/houji-archive-retired.sh
```

必须看到 `ARCHIVED_METADATA` 和 `ARCHIVE_RECEIPT` 才继续。工具核对卸载收据、回滚及残留；刚重启未收尾时等待。其他拒绝原因应保留日志排查，不反复覆盖。

ROM 升级可能清除 metadata，把工具输出的实际归档路径 `adb pull` 到电脑保存。卸载/回滚完成后，若不再使用才卸载配套 App 或 Google 扩展。

## 最少排障

- 无法发现：检查 Wi-Fi、蓝牙、iPhone“所有人，10 分钟”，退出旧页、等待恢复、重新进入；持续失败就暂停自动入口并留日志。
- 已保存却卡“正在验证”：取消旧记录并退出。本项目曾在无线恢复后重启一次 GMS 进程清掉旧状态，没有清数据；不把每次强停 GMS 当作常规流程。
- 准备/恢复异常：点 App“结束会话并恢复 Wi-Fi”；仍异常则 KSU 停用模块后完整重启。不要强制卸载无线模块或刷不匹配 vendor_dlkm。
- 无法正常启动：先走 KSU 官方安全模式/禁用模块恢复机制；保留原作者匹配镜像。只有确认仍是对应 ROM 与槽位，才用它自己的原始 init_boot 撤销 Root。

当前不承诺 ROM OTA 自动保留 AirDrop。新 ROM 没有匹配适配时，先保留普通 Wi-Fi 使用。
