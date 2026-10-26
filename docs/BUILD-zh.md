# 本地组装 v0.5.1-preview.1

这条路径使用 Release 提供的已验收开放运行组件，在本地编译 App、生成 KSU 模块。它不操作手机，也不修改驱动或 ROM 兼容条件。

## 1. 准备

- 下载本版本源码及 `open-runtime-v0.5.1-preview.1.zip`，校验 Release 的 SHA256SUMS。
- Python 3.11+、JDK 17、Android SDK Platform 36 与 Build Tools 36.0.0。
- 解压开放运行组件到项目 `local/open-runtime/`，该目录下应直接看到 `runtime/`、`quickshare-runtime/`、`module/`。
- 第三方依赖放到 `local/dependencies/`，只需四个文件，名称如下：

| 文件 | 来源要求 |
|---|---|
| `MoseyApp.apk` | 自行合法取得的原版 Google 扩展，20065 / 1.0.922413470 |
| `mosey_server` | 与本项目验收哈希匹配的 Google 原生服务 |
| `libmosey_daemon_ffi.so` | 配套 Google 原生库 |
| `amss20.bin` | 验收使用的三星 kiwi_v2 PASSTHRU 固件；不是小米原版同名文件 |

准确字节数、SHA256 在 [runtime-inputs.json](../profiles/runtime-inputs.json)。本项目没有这些闭源文件的公开再分发许可，不能附送或承诺存在可公开下载的渠道；不能合法取得匹配输入时，此版本无法组装。不要用“同名”“相似型号”的文件替代。

另从匹配作者 ROM 的 `system_ext.img` 提取以下原始内容，保留目录层级和文件名到 `local/system_ext/`：

    etc/vintf/manifest/
    etc/permissions/
    etc/default-permissions/
    etc/selinux/system_ext_service_contexts

已核验 ROM 的此镜像是 EROFS。Linux/WSL 可用 erofs-utils 的 `fsck.erofs --extract=目标目录 system_ext.img` 提取，再复制上述四项。必须从原版镜像取，不能从已经挂载本模块的运行系统复制。工具核对完整目录清单与哈希。

## 2. 检查，再组装

以下 PowerShell 命令在源码根目录执行；SDK/JDK 路径换成自己的安装目录。`JAVA_HOME` 指 JDK 根目录，不含 `bin`。

```powershell
$env:ANDROID_SDK_ROOT = 'C:\Android\Sdk'
$env:JAVA_HOME = 'C:\Java\jdk-17'
python tools/assemble.py --open-runtime local/open-runtime --dependencies local/dependencies --system-ext local/system_ext --check-only
```

看到 `INPUTS_VERIFIED` 再执行：

```powershell
python tools/assemble.py --open-runtime local/open-runtime --dependencies local/dependencies --system-ext local/system_ext
```

结果在 `build/local-release/`：`KSU-module.zip`、`companion.apk`、`BUILD-RESULT.json`。构建会验证 APK 签名、对齐和 profile；输出目录已存在时拒绝覆盖，重试需指定一个新 `--output`。

本地签名密钥和口令保存在忽略的 `private/`，不要上传。后续覆盖更新 App 必须继续使用同一密钥；这把密钥不同于参考设备的开发签名，所以不能直接覆盖参考设备现有 App。签名不同需要先按卸载/归档流程结束旧集成，再处理旧 App，重新授权 Root。

生成物包含你提供的闭源依赖，只供在你的授权范围内使用；不能因为构建成功就公开转发整个 ZIP/APK。

## 3. 从源码重新编译开放运行组件

Java 源码分别在 `native/control`、`native/ble`、`module/java`、`observer`。用 JDK 17 的 `javac --release 8` 和 SDK 36 的 D8（`--min-api 36`）编译。`tools/build_java.py` 可完成并比较参考哈希。

`native/shim` 是当前 Quick Share 参数兼容层；`native/legacy-shim` 对应保留的独立发送路径。`tools/build_native.sh` 使用指定的 Android Clang 17 r487747c 和使用者自行提供的 Android 链接库构建。所有预编译输入对应关系见 [源码来源](SOURCES.md)。

内核构建见 [内核重建](KERNEL.md)。首版提供完整修改源码和配置，不宣称在任何发行版上都能一条命令复刻原二进制。重新编译的驱动必须再做 ELF/版本符号、生命周期、双向实机验收；不要把自己的新二进制哈希填进 profile 来绕过兼容审查。

无需手机的测试：

```bash
python3 tests/test_controls.py
python3 tests/test_recovery_guards.py
python3 -m unittest discover -s tests -p 'test_assembly.py'
```
