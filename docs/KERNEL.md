# Kernel source and rebuild boundary

Use Linux/WSL, AArch64 Android Clang 17 `clang-r487747c`, LLVM tools and the normal kernel build prerequisites. The reference build used genuine pahole 1.30 where BTF was needed. No dummy pahole or fabricated function implementation is required.

1. Obtain the kernel archive pinned by `kernel/kernel-source.json` and verify SHA256 before extraction into a disposable build directory.
2. Obtain the WLAN commit pinned by `kernel/wlan-source.json` (including the complete unchanged source tree).
3. Apply and check the published changes:

```bash
python3 tools/prepare_kernel_sources.py --wlan /work/wlan --kernel /work/kernel
```

This verifies the unchanged WLAN inputs, applies the complete 115-file overlay, restores relative links, and checks every final source file. `kernel/wonder/` and `kernel/bridge/` are complete final module source snapshots. The reference standalone sending path uses the pre-framework WLAN revision: reverse `kernel/legacy-to-framework.patch` from `/work/wlan/qcacld-3.0/core/hdd` in a separate copy; do not reverse it in the Quick Share build.

4. Prepare separate kernel output directories with `kernel/configs/wlan.config`, `bridge.config`, and `wonder.config`. Kernel `modules_prepare` alone does **not** produce all version symbol information. The supplied `kernel.symvers` and `wlan-extra.symvers` are the reference ABI/provider tables, not substitutes for matching source/configuration. Preserve the reference kernel release `6.1.138-android14-11-g0c3d559bcd85-ab14529422` and compare symbol CRCs and struct-module layout before using new binaries.
5. `kernel/build-commands.json` records the exact reference module build arguments with portable placeholders. Expand `${KERNEL}`, `${KERNEL_OUT}`, `${WLAN}`, `${BRIDGE}`, `${WONDER}`, `${EXTRA_SYMVERS}`, `${IPA_INCLUDE}`, `${EXTERNAL_INCLUDE}`, `${PAHOLE}` for your prepared directories; invoke the argument arrays directly, preserving arguments containing spaces. For Wonder, set `${WONDER_PROVIDERS}` to `kernel/configs/wonder-providers.symvers`, `${KCONFIG_WONDER}` to `kernel/configs/Kconfig.wonder`, and export `WONDER_SOURCE` as the absolute Wonder source directory. Build the bridge before WLAN and include its exported symbols. `kernel/dataipa-include` and `external-include` supply the interface headers.

The kernel source is a public reconstruction of the reference module ABI, not the closed ROM author's original full build system. Reference WLAN output has 567 versioned imports; its loaded section hashes were checked before/after debug stripping. Stripping debug data does not make an arbitrary rebuilt module compatible.

The first public release does not claim an independently repeated, clean full-kernel build on a new host. The published overlay has been compared against the exact sources used for accepted binaries. Java/native userspace rebuilds and local APK/module assembly have been repeated for this release. Kernel rebuilds need their own ELF/CRC/lifecycle and phone acceptance; use the hash-pinned reference open-runtime asset for the local assembly path.

Keep Xiaomi PHY/BDF and the stock on-disk driver. The runtime temporarily loads the adapted WLAN + bridge + Wonder and binds the user-supplied AMSS, then restores stock state. Do not permanently flash the Samsung firmware or replace vendor_dlkm from this repository.
