# Source provenance

The release separates open runtime code from proprietary external inputs. SHA256 in `profiles/runtime-inputs.json` is the authoritative reference binary mapping.

| Runtime | Corresponding source |
|---|---|
| Quick Share WLAN | `kernel/wlan-overlay` + pinned MiCode WLAN commit; kernel overlay/configs/build arguments |
| Legacy independent-send WLAN | Same baseline, with `kernel/legacy-to-framework.patch` reversed |
| `houji_wonder_bridge.ko` | `kernel/bridge` |
| `wonder.ko` | `kernel/wonder` |
| Quick Share `local-stability.so` | `native/shim` |
| Legacy `local-stability.so` | `native/legacy-shim` |
| `wonder-probe` | `native/wonder-probe` |
| `scope-probe`, `service-probe` | `native/scope`, `native/service` |
| BLE DEX | `native/ble` |
| Quick Share control DEX | `native/control` |
| Module state DEX | `module/java` |
| Observer DEX | `observer` |
| Read-only bind helper | `native/bind-ro` |
| Companion APK | `app`; assembled proprietary inputs are excluded from public distribution |

WLAN base: https://github.com/MiCode/vendor_qcom_opensource_wlan at `a4da9e3b43864da13bc978b8cb6385ba5332a4af`.

Kernel base: https://github.com/MiCode/Xiaomi_Kernel_OpenSource at `8c66ad18fc6132fcdcbdf0cf67832f47b33010a4`.

Wonder origin: https://android.googlesource.com/kernel/google-modules/wlan/wonder/ . The repository contains the actual final source snapshot, including its original SPDX notices and the local lifecycle fixes. It is not a claim that the newer upstream HEAD works unchanged on houji.

Data IPA headers: Qualcomm CodeLinaro `platform/vendor/opensource/dataipa`, tag `LA.VENDOR.14.3.0.r1-14500-lanai.0`, commit `c40698e6116eb4f3792c2af7053dae8a5886dec2`, fetched through https://github.com/LineageOS/android_vendor_qcom_opensource_dataipa and checked against the original Git blobs. Only compilation interface headers are used; no replacement ipam module is supplied.

Compiler: Android Clang r487747c, version 17.0.2, LLVM revision `d9f89f4d16663d5012e5c09495f3b30ece3d2362`. The toolchain itself is not redistributed.

Vendor-derived source files preserve their original copyright and licenses, including compatibility code derived from Samsung/Qualcomm public driver material. Firmware is a separate external dependency; availability of driver source does not grant firmware redistribution rights.
