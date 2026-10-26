# v0.5.1-preview.1 — 首个公开预览版

基于已实机验收的 Xiaomi 14 PixelOS Quick Share / AirDrop 整合：系统入口自动启停、无需电脑连续双向互传、正常结束恢复原厂 Wi-Fi，支持大文件。已验证图片、Word 和 128 MiB ZIP。

包含本次 Google 更新兼容修复：真实国家码的会话临时权限、权限回滚账本，以及 Wi-Fi 被系统重新开启时的竞争保护。

**仅支持指定 houji / PixelOS 20261021-0252 / B 槽 / KSU 3.3.1 基线。** 请先阅读仓库 README、安装和更新教程。

附件：

- `open-runtime-v0.5.1-preview.1.zip`：可再分发的开放运行组件，供本地组装；不能直接刷入 KSU。
- `corresponding-sources-v0.5.1-preview.1.zip`：项目源码、完整 WLAN 源码快照及固定上游内核源码，供审阅/重建；不能刷机。
- `SHA256SUMS`：附件校验。
- GitHub 自动生成的 Source code：项目源码及教程，内核上游完整源码另在 corresponding-sources 附件。

**不附 Google 闭源组件、三星固件、作者 ROM/init_boot 或私钥。** 使用者自行合法提供四个第三方依赖及原版 system_ext 内容，按教程本地生成 KSU 模块和 APK；无法取得匹配依赖则无法组装。

已重复验证本地编译/组装：4 组 Java DEX、6 个原生组件与参考哈希一致，131 个离线测试通过。新本地签名 APK 未再上机，实机结论对应整理前的参考整合；详见 VALIDATION。分享期间普通 Wi-Fi 会暂时中断，异常恢复仍可能保护重启，吞吐未达到 Apple 原生设备水平。
