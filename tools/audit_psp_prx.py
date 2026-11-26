#!/usr/bin/env python3
"""Inspect PSP firmware resources on disk and optionally compare them with a VPK."""

import argparse
import hashlib
from pathlib import Path
from xml.etree import ElementTree
import re
import struct
import sys
import zipfile
import zlib


PSP_HEADER_SIZE = 0x150
MAX_ENTRY_SIZE = 16 * 1024 * 1024
FLASH0_LAYOUT = {
    "kd": (
        "galaxy", "inferno", "janus", "kermit_idstorage", "libpsardumper",
        "march33", "minimal_edition", "pentazemin", "popcorn", "systemctrl", "vshctrl",
    ),
    "vsh/module": ("recovery", "satelite", "satelite_classic", "xmbctrl"),
}
LOOSE_MODULES = ("pentazemin", "systemctrl")
ORIGINAL_PATH = re.compile(rb"PSPEMUCFW|ux0:/?pspemu", re.IGNORECASE)
ORIGINAL_VITA_PATH = re.compile(
    rb"ux0:/?(?:pspemu|(?:app|data)/PSPEMUCFW)(?=[/\0])|"
    rb"(?:ux0|ur0):/?data/AdrenalineRA(?=[/\0])", re.IGNORECASE
)
INTERESTING_PATH = re.compile(
    rb"PSPEMUCFW|ux0:/?pspemu|__ADRENALINE__|RA_POLICY\.TXT", re.IGNORECASE
)


def digest(data):
    return hashlib.sha256(data).digest()


def unpack_pspgz(data):
    if (len(data) <= PSP_HEADER_SIZE or data[:4] != b"~PSP" or
            struct.unpack_from("<I", data, 0x2C)[0] != len(data) or
            not data[PSP_HEADER_SIZE:].startswith(b"\x1f\x8b\x08")):
        raise ValueError("unsupported or invalid pspgz PRX")
    gz = zlib.decompressobj(wbits=31)
    payload = gz.decompress(data[PSP_HEADER_SIZE:], MAX_ENTRY_SIZE + 1)
    if not gz.eof or len(payload) > MAX_ENTRY_SIZE or payload[:4] != b"\x7fELF":
        raise ValueError("invalid or oversized PSP ELF payload")
    return payload


def fself_segments(data):
    if data[:4] != b"SCE\0":
        raise ValueError("module is not a Vita FSELF")
    elf, phdr, segments = (struct.unpack_from("<Q", data, n)[0] for n in (64, 72, 88))
    if data[elf:elf + 6] != b"\x7fELF\x01\x01":
        raise ValueError("unsupported Vita ELF header")
    entry_size, count = struct.unpack_from("<HH", data, elf + 42)
    if entry_size != 32 or not 0 < count <= 16:
        raise ValueError("invalid Vita program header table")
    for index in range(count):
        expected = struct.unpack_from("<I", data, phdr + entry_size * index + 16)[0]
        offset, length, compression, encryption = struct.unpack_from(
            "<QQQQ", data, segments + 32 * index
        )
        if (encryption != 2 or compression not in (1, 2) or
                expected > MAX_ENTRY_SIZE or offset + length > len(data)):
            raise ValueError("unsupported Vita segment")
        segment = data[offset:offset + length]
        if compression == 2:
            gz = zlib.decompressobj()
            segment = gz.decompress(segment, expected + 1)
            if not gz.eof:
                raise ValueError("invalid compressed Vita segment")
        else:
            segment = segment[:expected]
        if len(segment) != expected:
            raise ValueError("unexpected Vita segment length")
        yield segment


def read_package_entry(package, name):
    entry = package.getinfo(name)
    if entry.file_size > MAX_ENTRY_SIZE:
        raise ValueError(f"{name} exceeds the size limit")
    return package.read(entry)


def read_sfo_strings(data):
    magic, version, keys, values, count = struct.unpack_from("<5I", data)
    if magic != 0x46535000 or version != 0x101 or not 20 + count * 16 <= keys <= values <= len(data):
        raise ValueError("invalid VPK param.sfo header")

    fields = {}
    for index in range(count):
        key_offset, kind, length, capacity, value_offset = struct.unpack_from(
            "<HHIII", data, 20 + index * 16
        )
        key_start = keys + key_offset
        key_end = data.find(b"\0", key_start, values)
        start = values + value_offset
        if key_end <= key_start or length > capacity or start + capacity > len(data):
            raise ValueError("invalid VPK param.sfo entry")
        key = data[key_start:key_end].decode("ascii")
        if key in fields:
            raise ValueError(f"duplicate VPK param.sfo key {key}")
        if kind == 0x0204:
            value = data[start:start + length]
            if not value.endswith(b"\0") or b"\0" in value[:-1]:
                raise ValueError(f"invalid VPK param.sfo string {key}")
            fields[key] = value[:-1].decode("utf-8")
        else:
            fields[key] = None
    return fields


def audit_package_layout(package, expect_test):
    expected = {
        "eboot.bin", "eboot.pbp", "frame.xml", "sce_sys/param.sfo", "sce_sys/icon0.png",
        "sce_sys/livearea/contents/template.xml", "sce_sys/livearea/contents/bg.png",
        "sce_sys/livearea/contents/startup.png", "sce_module/adrenaline_kernel.skprx",
        "sce_module/adrenaline_user.suprx", "sce_module/adrenaline_vsh.suprx",
        "sce_module/usbdevice.skprx", "ra_sounds/message.wav", "ra_sounds/unlock.wav",
        "ra_sounds/lbsubmit.wav", *(f"{name}.prx" for name in LOOSE_MODULES),
    }
    names = package.namelist()
    if len(names) != len({name.casefold() for name in names}):
        raise ValueError("VPK has duplicate entry names")
    missing = sorted(expected - set(names))
    unexpected = sorted(set(names) - expected)
    if missing or unexpected:
        raise ValueError(f"VPK manifest differs (missing: {missing}; unexpected: {unexpected})")

    for entry in package.infolist():
        if not 0 < entry.file_size <= MAX_ENTRY_SIZE:
            raise ValueError(f"invalid size for VPK entry {entry.filename}")
    bad_crc = package.testzip()
    if bad_crc is not None:
        raise ValueError(f"VPK CRC mismatch in {bad_crc}")

    title_id, title = ("ADRA00001", "AdrenalineRA Test") if expect_test else ("PSPEMUCFW", "Adrenaline")
    sfo = read_sfo_strings(read_package_entry(package, "sce_sys/param.sfo"))
    for key, value in {"TITLE_ID": title_id, "TITLE": title, "STITLE": title, "CATEGORY": "gd"}.items():
        if sfo.get(key) != value:
            raise ValueError(f"VPK param.sfo {key} must be {value!r}; got {sfo.get(key)!r}")
    version = sfo.get("APP_VER")
    if not version or not re.fullmatch(r"[0-9]{2}\.[0-9]{2}", version):
        raise ValueError("invalid VPK param.sfo APP_VER")

    for name in ("frame.xml", "sce_sys/livearea/contents/template.xml"):
        root = ElementTree.fromstring(read_package_entry(package, name))
        if expect_test and not any(node.text == title for node in root.iter("str")):
            raise ValueError(f"{name} does not display {title}")
    print(f"VPK identity {title_id} ({title} {version}); {len(names)} entries passed the CRC checks.")
    return title_id


def audit_vita_binaries(package, title_id, expect_test):
    suffix = "Test" if expect_test else ""
    bridge = f"AdrenalineKubridge{suffix}"
    expected = {
        "eboot.bin": (f"ux0:app/{title_id}/sce_module/adrenaline_kernel.skprx",),
        "sce_module/adrenaline_kernel.skprx": (f"AdrenalineKernel{suffix}", bridge),
        "sce_module/adrenaline_user.suprx": (f"AdrenalineUser{suffix}", bridge),
        "sce_module/adrenaline_vsh.suprx": (f"AdrenalineVsh{suffix}", bridge),
        "sce_module/usbdevice.skprx": (f"VitaShellUsbDevice{suffix}",),
    }
    for name, markers in expected.items():
        segments = tuple(fself_segments(read_package_entry(package, name)))
        for marker in markers:
            exact = re.compile(rb"(?<![a-zA-Z0-9_])" + re.escape(marker.encode("ascii")) + rb"\0")
            if not any(exact.search(segment) for segment in segments):
                raise ValueError(f"{name} is missing its Vita identity {marker!r}")
        # Only launcher and kernel stat the exact original app directory as a startup guard.
        if expect_test and name in {"eboot.bin", "sce_module/adrenaline_kernel.skprx"}:
            path_segments = (segment.replace(b"ux0:app/PSPEMUCFW\0", b"\0") for segment in segments)
        elif expect_test and name == "sce_module/adrenaline_user.suprx":
            path_segments = (segment.replace(b"ux0:pspemu/ISO\0", b"\0") for segment in segments)
        else:
            path_segments = iter(segments)
        if expect_test and any(ORIGINAL_VITA_PATH.search(segment) for segment in path_segments):
            raise ValueError(f"{name} still contains a path to the original installation")
        if name == "sce_module/adrenaline_user.suprx":
            user_segments = segments
    print("VPK launcher path, module names and bridge names match the expected profile.")
    return user_segments


def audit_package(vpk, modules, expect_test=False):
    with zipfile.ZipFile(vpk) as package:
        title_id = audit_package_layout(package, expect_test)
        parts = audit_vita_binaries(package, title_id, expect_test)
        for name in LOOSE_MODULES:
            if digest(read_package_entry(package, f"{name}.prx")) != modules[name]:
                raise ValueError(f"loose {name}.prx differs from flash0")
        embedded = set()
        for segment in parts:
            for match in re.finditer(rb"~PSP", segment):
                start = match.start()
                if segment[start + PSP_HEADER_SIZE:start + PSP_HEADER_SIZE + 3] != b"\x1f\x8b\x08":
                    continue
                length = struct.unpack_from("<I", segment, start + 0x2C)[0]
                if length > PSP_HEADER_SIZE and start + length <= len(segment):
                    embedded.add(digest(segment[start:start + length]))
        for name, module_hash in modules.items():
            if module_hash not in embedded:
                raise ValueError(f"embedded {name}.prx missing or different from flash0")
        if embedded - set(modules.values()):
            raise ValueError("unexpected PSP PRX embedded in adrenaline_user.suprx")
    print(f"VPK matches flash0 for {len(modules)} embedded and {len(LOOSE_MODULES)} loose PRX files.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("flash0", type=Path, help="directory containing kd and vsh/module")
    parser.add_argument("--vpk", type=Path, help="check the VPK identity, manifest and PSP resources")
    parser.add_argument("--expect-test", action="store_true", help="require Test package identity and reject original PSP paths")
    args = parser.parse_args()

    modules = {}
    stale_paths = []
    try:
        for directory, names in FLASH0_LAYOUT.items():
            folder = args.flash0 / directory
            extras = sorted(path.name for path in folder.glob("*.prx") if path.stem not in names)
            if extras:
                raise ValueError(f"unexpected PRX files in {folder}: {', '.join(extras)}")
            for name in names:
                path = folder / f"{name}.prx"
                if path.stat().st_size > MAX_ENTRY_SIZE:
                    raise ValueError(f"{path} exceeds the size limit")
                data = path.read_bytes()
                try:
                    payload = unpack_pspgz(data)
                except (ValueError, zlib.error) as exc:
                    raise ValueError(f"{path}: {exc}") from exc
                modules[name] = digest(data)
                print(f"{path.relative_to(args.flash0).as_posix()} {modules[name].hex()}")
                for value in sorted(set(re.findall(rb"[ -~]{5,}", payload))):
                    if INTERESTING_PATH.search(value):
                        print(f"  {value.decode('ascii')}")
                if ORIGINAL_PATH.search(payload):
                    stale_paths.append(path.relative_to(args.flash0).as_posix())
                elif (args.expect_test and name == "recovery" and
                      b"ms0:/__ADRENALINE__/flash0.installing\0" not in payload):
                    raise ValueError("Test recovery.prx is missing its firmware staging path")
        if args.vpk:
            audit_package(args.vpk, modules, expect_test=args.expect_test)
    except (OSError, ValueError, KeyError, RuntimeError, struct.error, zipfile.BadZipFile, zlib.error, ElementTree.ParseError) as exc:
        print(f"PSP PRX audit failed: {exc}", file=sys.stderr)
        return 2

    if stale_paths:
        print(f"Original paths found in {', '.join(stale_paths)}.", file=sys.stderr)
    return 1 if args.expect_test and stale_paths else 0


if __name__ == "__main__":
    sys.exit(main())
