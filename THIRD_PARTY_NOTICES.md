# Licenses and third-party boundaries

New project-authored code without a separate notice is licensed under GPL-2.0-only; see LICENSE. Existing copyright notices and SPDX expressions in individual files take precedence. In particular, WLAN files include ISC, BSD and GPL licensed code, and the Wonder/bridge sources retain their own notices. This repository does not relicense upstream code.

| Component | Source / licensing boundary |
|---|---|
| Linux kernel baseline | MiCode/Xiaomi_Kernel_OpenSource, commit `8c66ad18fc6132fcdcbdf0cf67832f47b33010a4`; kernel licensing and exceptions apply. Repository includes local overlay and configurations, not the whole kernel. |
| Qualcomm/Xiaomi WLAN | MiCode/vendor_qcom_opensource_wlan, commit `a4da9e3b43864da13bc978b8cb6385ba5332a4af`; preserve per-file licenses. Full changed files and a final source manifest are supplied. |
| Wonder | Android public kernel/google-modules/wlan/wonder sources with local lifecycle changes; see included source notices and docs/SOURCES.md. |
| Data IPA interface headers | Qualcomm CodeLinaro dataipa `c40698e6116eb4f3792c2af7053dae8a5886dec2`, obtained from the LineageOS mirror with matching Git blobs. Header notices retained. Only the headers required for WLAN compilation are included. |
| KernelSU | External project, https://github.com/tiann/KernelSU. Manager, patched boot images and daemon are not distributed here. Follow its licensing and official releases. |
| Google Mosey APK, service and FFI | Proprietary external inputs. NOT included in source, open-runtime ZIP, or public install bundle. Input hashes are identifiers, not a redistribution grant. Decompiled APK code is not included. |
| Samsung kiwi_v2 AMSS firmware | External proprietary input. NOT included. No claim that all kiwi_v2 firmware is interchangeable. |
| Author PixelOS ROM / init_boot | External inputs, NOT included. Obtain from their author and follow applicable terms. |

`open-runtime-v0.5.1-preview.1.zip` contains only project/open-source executables and their license notices. Corresponding modified sources are in the same release's source archive/repository; pinned unchanged baselines and reconstruction instructions are supplied. No private signing key or pre-generated private AirDrop identity is shipped.

Local assembly combines these with the user's own dependencies. The resulting APK/ZIP is not offered for public redistribution by this project. Rights to use an input do not by themselves grant rights to redistribute it.
