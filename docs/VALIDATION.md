# v0.5.1-preview.1 validation

Reference integration acceptance (before public source packaging):

- Phone-only automatic system Quick Share entry, continuous send/receive, ordinary Wi-Fi restoration without routine reboot.
- Image transfers in both directions; Word and ZIP contents opened by the user. 128 MiB ZIP send completed and extracted successfully.
- Google account / Play services update compatibility fix: temporary country-code permission lease and cleanup, plus synchronized Wi-Fi shutdown and pre-driver-change recovery.
- After clearing one stale Google sharing record, consecutive sending and receiving completed in the UI and files opened. The fix does not clear Google account data or routinely force-stop GMS.
- Exact device/ROM/kernel/firmware hashes are recorded in the public profile. At the last fix acceptance Wi-Fi was enabled but disconnected from an AP, so that final observation did not repeat an Internet browsing test.

Public source preparation (2026-10-26, no phone operations):

- Rebuilt all four Java helper DEX files; SHA256 matches the reference for each. Companion app classes.dex also matches.
- Rebuilt all six native tools/libraries (both shims, discovery/scope/Wonder probes, read-only bind helper); SHA256 matches the reference for each.
- Passed 22 Wi-Fi/control scenarios, 3 recovery guard cases, 24 entry-gate tests, 65 rate compatibility cases, 12 station selector cases, and 5 local assembly validation tests.
- Local assembly produced a signed/aligned companion APK and a KSU module. Module file set is identical to the reference. Every runtime executable and shell/policy file is unchanged; differences are the local APK signature, equivalent runtime checksum-list ordering, profile validation label and regenerated hashes incorporating the new APK.
- Source files were scanned for private keys, access tokens, private device identifiers and local user paths. Runtime archive uses an explicit open-component allowlist; no Google binaries, firmware, ROM or init_boot are in public assets.

The freshly signed local assembly has **not** been installed on the phone. Actual phone acceptance belongs to the reference integration; binary/runtime equality supports the packaging check but is not a substitute for testing a new installation.

Kernel sources were exported from the accepted build trees, with fixed upstream references, complete modified files, configurations and provider tables. A fresh full kernel/driver build on a second host was not repeated for this publication. See KERNEL.md before replacing a reference .ko.
