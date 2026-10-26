"""Profile, archive and APK checks preserved from the validated installer."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import struct
import tarfile
import xml.etree.ElementTree as ET
import zipfile

ID = "houji_qs_system79"
GOOGLE_SHA = "8e087557289d70a6fb29b34fd64c11ff15353e0f07ad89dfebe1dc15e723888a"
CONTEXT_LINE = "com.google.android.moseyservice.IMoseyService/default u:object_r:houji_mosey_service:s0"
DEVICE_PATHS = {
    "wlan_disk_sha256": "/vendor_dlkm/lib/modules/qca_cld3_kiwi_v2.ko",
    "wlan_note_sha256": "/sys/module/qca_cld3_kiwi_v2/notes/.note.gnu.build-id",
    "amss_sha256": "/vendor/firmware_mnt/image/kiwi/amss20.bin",
    "phy_sha256": "/vendor/firmware_mnt/image/kiwi/phy_ucode20.elf",
    "bdf_sha256": "/vendor/firmware_mnt/image/kiwi/bd_n3.elf",
    "regdb_sha256": "/vendor/firmware_mnt/image/kiwi/regdb_xiaomi.bin",
}
SYSTEM_DIRS = {
    "vintf": "etc/vintf/manifest",
    "permissions": "etc/permissions",
    "default_permissions": "etc/default-permissions",
}


def sha(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def need(value: bool, message: str) -> None:
    if not value:
        raise ValueError(message)


def put(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def paths(root: Path):
    need(root.is_dir() and not root.is_symlink(), f"not an ordinary directory: {root}")
    for p in sorted(root.rglob("*"), key=lambda p: p.relative_to(root).as_posix()):
        need(not p.is_symlink(), f"symlink input: {p}")
        need(p.is_dir() or p.is_file(), f"special input: {p}")
        need(not any(c in p.relative_to(root).as_posix() for c in "\n\r\\"), "unsupported filename")
        yield p


def inventory(root: Path) -> tuple[str, str]:
    entries = list(paths(root))
    names = "".join("./" + p.relative_to(root).as_posix() + "\n" for p in entries)
    hashes = "".join(sha(p) + "  ./" + p.relative_to(root).as_posix() + "\n" for p in entries if p.is_file())
    return names, hashes


def tree_digest(root: Path) -> str:
    names, hashes = inventory(root)
    return hashlib.sha256((names + "\0" + hashes).encode()).hexdigest()


def baseline(root: Path, stem: Path) -> None:
    names, hashes = inventory(root)
    put(Path(str(stem) + ".inventory"), names)
    put(Path(str(stem) + ".sha256"), hashes)


def manifest(root: Path, target: Path, excluded: set[str]) -> None:
    lines = []
    for p in paths(root):
        rel = p.relative_to(root).as_posix()
        if p.is_file() and rel not in excluded:
            lines.append(f"{sha(p)}  ./{rel}\n")
    put(target, "".join(lines))


def safe_extract(source: Path, target: Path) -> Path:
    with tarfile.open(source) as tf:
        members = tf.getmembers()
        roots = set()
        seen = set()
        for item in members:
            p = PurePosixPath(item.name)
            need(not p.is_absolute() and ".." not in p.parts and "\\" not in item.name, "unsafe archive path")
            need(item.isdir() or item.isfile(), "archive links/special entries forbidden")
            need(item.name not in seen, "duplicate archive member")
            seen.add(item.name)
            roots.add(p.parts[0])
        need(len(roots) == 1, "archive must have one top directory")
        for item in members:
            dest = target.joinpath(*PurePosixPath(item.name).parts)
            if item.isdir():
                dest.mkdir(parents=True, exist_ok=True)
            else:
                dest.parent.mkdir(parents=True, exist_ok=True)
                with tf.extractfile(item) as src, dest.open("xb") as dst:
                    shutil.copyfileobj(src, dst)
        return target / next(iter(roots))


def apk_package(apk: Path) -> str:
    """Read the manifest package before PackageManager can install any bytes."""
    with zipfile.ZipFile(apk) as z:
        data = z.read("AndroidManifest.xml")
    if data.lstrip().startswith(b"<"):
        return ET.fromstring(data).attrib["package"]
    need(len(data) >= 8, "short APK manifest")
    typ, head, total = struct.unpack_from("<HHI", data)
    need(typ == 3 and head >= 8 and total == len(data), "invalid binary manifest header")
    strings = []
    offset = head
    while offset < len(data):
        need(offset + 8 <= len(data), "truncated manifest chunk")
        typ, hlen, size = struct.unpack_from("<HHI", data, offset)
        need(8 <= hlen <= size and offset + size <= len(data), "invalid manifest chunk")
        if typ == 1:
            count, styles, flags, start, _ = struct.unpack_from("<IIIII", data, offset + 8)
            need(hlen >= 28 and hlen + 4 * (count + styles) <= size and start < size, "invalid string pool")
            def length_at(pos, utf8):
                if utf8:
                    v = data[pos];pos += 1
                    if v & 128:v = ((v & 127) << 8) | data[pos];pos += 1
                else:
                    v = struct.unpack_from("<H", data, pos)[0];pos += 2
                    if v & 32768:v = ((v & 32767) << 16) | struct.unpack_from("<H", data, pos)[0];pos += 2
                return v, pos
            for i in range(count):
                rel = struct.unpack_from("<I", data, offset + hlen + 4 * i)[0]
                pos = offset + start + rel
                need(pos < offset + size, "invalid string offset")
                if flags & 256:
                    _, pos = length_at(pos, True)
                    length, pos = length_at(pos, True)
                    need(pos + length < offset + size, "truncated UTF8 string")
                    strings.append(data[pos:pos + length].decode("utf-8"))
                else:
                    length, pos = length_at(pos, False)
                    need(pos + length * 2 < offset + size, "truncated UTF16 string")
                    strings.append(data[pos:pos + length * 2].decode("utf-16le"))
        elif typ == 0x102:
            need(hlen == 16 and size >= 36, "invalid start element")
            name = struct.unpack_from("<I", data, offset + 20)[0]
            need(name < len(strings), "element string missing")
            if strings[name] == "manifest":
                start, item_size, count = struct.unpack_from("<HHH", data, offset + 24)
                need(item_size >= 20 and 16 + start + item_size * count <= size, "invalid attributes")
                for i in range(count):
                    pos = offset + 16 + start + item_size * i
                    ns, name, raw = struct.unpack_from("<III", data, pos)
                    need(name < len(strings), "attribute name missing")
                    if strings[name] == "package" and ns == 0xffffffff:
                        value = raw if raw != 0xffffffff else struct.unpack_from("<I", data, pos + 16)[0]
                        need(value < len(strings), "package string missing")
                        return strings[value]
                raise ValueError("manifest package missing")
        offset += size
    raise ValueError("manifest root missing")


def validate_profile(profile: dict) -> tuple[Path, Path]:
    required = {"profile_id", "device", "slot_suffix", "fingerprint", "kernel_release", "vendor_dlkm_incremental",
                "ksud_sha256", "rom_zip_sha256", "system_ext_root", "system_ext_checks",
                "app_apk", "app_sha256", "radio_baseline_validation"} | set(DEVICE_PATHS)
    need(required <= profile.keys(), "missing profile fields: " + ", ".join(sorted(required - profile.keys())))
    need(profile["device"] == "houji", "only houji supported")
    need(profile["slot_suffix"] in ("_a", "_b"), "explicit verified slot required")
    need(bool(re.fullmatch(r"[a-zA-Z0-9._-]{3,80}", profile["profile_id"])), "invalid profile id")
    need(profile["radio_baseline_validation"] in ("pending", "verified"), "explicit radio validation status required")
    need(isinstance(profile["vendor_dlkm_incremental"], str) and re.fullmatch(r"[0-9]{8,16}", profile["vendor_dlkm_incremental"]) is not None, "explicit incremental required")
    for key, value in profile.items():
        if key.endswith("_sha256"):
            need(isinstance(value, str) and re.fullmatch("[0-9a-f]{64}", value) is not None, f"invalid hash: {key}")
    for key in ("fingerprint", "kernel_release"):
        need(isinstance(profile[key], str) and profile[key] and "\n" not in profile[key] and "\r" not in profile[key], f"invalid {key}")
    root, app = Path(profile["system_ext_root"]), Path(profile["app_apk"])
    need(app.is_file() and not app.is_symlink(), "missing companion APK")
    need(sha(app) == profile["app_sha256"], "companion APK hash mismatch")
    need(apk_package(app) == "org.houji.airdrop", "companion APK package identity mismatch")
    checks = profile["system_ext_checks"]
    need(set(checks) == set(SYSTEM_DIRS) | {"service_contexts"}, "complete system_ext_checks required")
    for name, rel in SYSTEM_DIRS.items():
        need(tree_digest(root / rel) == checks[name], f"system_ext {name} input mismatch")
    contexts = root / "etc/selinux/system_ext_service_contexts"
    need(contexts.is_file() and not contexts.is_symlink(), "service contexts missing")
    need(sha(contexts) == checks["service_contexts"], "service contexts mismatch")
    need("houji_mosey_service" not in contexts.read_text(), "profile already systemized")
    need(not (root / "etc/vintf/manifest/manifest_mosey.xml").exists(), "existing Mosey manifest")
    need(not (root / "etc/permissions/privapp-permissions-com.google.android.mosey.xml").exists(), "existing Mosey permissions")
    with zipfile.ZipFile(app) as z:
        runtime = z.read("assets/quickshare-runtime/quickshare-runner.sh").decode("utf-8")
        for key in DEVICE_PATHS:
            need(profile[key] in runtime, f"companion radio baseline differs: {key}")
        need(profile["kernel_release"] in runtime and profile["fingerprint"] in runtime, "companion kernel/ROM guard differs")
        need("ro.vendor_dlkm.build.version.incremental" in runtime and profile["vendor_dlkm_incremental"] in runtime, "companion incremental guard differs")
        need(f"= {profile['slot_suffix']}" in runtime, "companion slot guard differs")
        need("/data/adb/modules/houji_qs_system79/manage.sh" in runtime, "companion integration ID differs")
    return root, app

