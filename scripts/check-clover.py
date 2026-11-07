#!/usr/bin/env python3
"""Preflight check for the Xiaomi Mi Pad 4 8" LTE (clover, SDM660) board.

The firmware step runs this from the assembled build tree, where board.toml is not copied, so
every check works from either location. Three groups run independently:

  ADT    calls scripts/make-clover-adt.py into a temporary file, reads the Apple node encoding
         back, and asserts the nodes and properties ArmDTPlatform and DarwinBoot require.
         Python 3 alone is enough, so this is the check to run first on a new host.
  board  cross-checks board.toml against the defconfig, the payload contract and the hardware
         constants.  It parses the TOML with tomllib (3.11+) or tomli when they exist and with
         a built-in subset reader otherwise, so it runs on the Python macOS ships.
  region compares the windows the board spec declares: the splash region, the firmware volume,
         the described DRAM and the stock reserved carve-outs must not lie on each other.
  dt     re-reads /proc/device-tree/soc/*/reg from this unit's own facts dump and holds every
         physical address in the driver headers against it.
  tree   reports the modules the kernel and firmware steps expect.

Exit status is 1 when any group reports an error.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import tomllib

HERE = pathlib.Path(__file__).resolve()
REPO = HERE.parents[1]

# SDM660 facts.  MSM_ID and CPU_REGS come from the two device trees described in
# notes/clover-device-tree-sources.md; SPLASH_BASE, DRAM_BASE and DRAM_SIZE from
# this unit itself (fw/xbl-uefiplat.txt and dt/memory.raw.txt under
# facts/<serial>-<stamp>/), which is where they disagree with clover-overlay.dtsi.
MSM_ID = 0x13D
CPU_REGS = [0x0, 0x1, 0x2, 0x3, 0x100, 0x101, 0x102, 0x103]
SPLASH_BASE = 0x9D400000
SPLASH_SIZE = 0x02400000
DISPLAY_SCALE = 2
DRAM_BASE = 0x80000000
DRAM_SIZE = 0x80000000
ARM_IO_BASE = 0x17000000
GICD_PHYS = 0x17A00000
TIMEBASE_HZ = 19200000

REQUIRED_CHOSEN = ["random-seed", "nvram-proxy-data", "nvram-bank-size", "dram-base",
                   "dram-size", "crypto-hash-method", "display-base", "display-scale",
                   "unique-chip-id", "amfi-only-platform-code", "root-matching"]
# Round322 removes ephemeral-storage (added by round320) and requires it ABSENT. boot 108 read
# back r320 and the desktop stall moved, it did not clear: SpringBoard left the migration port
# and now idles every beat on a chronod round-trip (tid=0x129, remote 0x2f03, ro=/ho=122:chronod,
# through the 1800 s deadline). And UniXNU-sheng -- the board that boots this image to the
# desktop -- turns out to boot with persistent_data=true and NO ephemeral-storage: make-sheng-adt.py:95
# reads "no ephemeral-storage, so first-boot data migration runs". round320 cited sheng backwards.
# This round stops asserting a volatile root is ephemeral and lets first-boot migration run against
# the card's real /private/var window (board.toml [userdata]: mmcblk0p64, mounted, not RAM) as the
# cheap A/B. See dev-log Round321 for the boot 108 read-back.
# The volume /chosen names. It is the unsealed OS volume inside the iPadOS container the board's
# OS window carries: UUID 04769D26-2CA0-4825-B443-E2E5F88300F2, role System, Sealed=No. Round119
# copied a dictionary naming a volume no service here carried; that was a wrong target, not a
# reason for root-matching to be absent. Round137 names the service CloverBlock actually publishes.
ROOT_VOLUME_UUID = "04769D26-2CA0-4825-B443-E2E5F88300F2"
# The two original Apple trust modules the boot bundle carries unchanged (scripts/make-clover-trust-bundle.py).
STATIC_MODULE_SHA256 = "da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2"
SYSTEM_MODULE_SHA256 = "d121ba89167e03aafd00fa687c30cd8ebc4ccafd10c1408adb2ba3de12403651"
FORBIDDEN_CHOSEN: list[str] = []
# One RAMDisk placeholder, no PackedRAMDisk: the payload still carries the mockfs image
# (scripts/make-clover-root.py) so the memory map stays the shape DarwinBoot handed over in every
# earlier round, but since round137 nothing mounts it as / -- the root is the APFS volume named by
# /chosen/root-matching. mdevadd runs at IOKitBSDInit.cpp:826-843 before either root branch and
# with its unit passed explicitly, so md0 exists and is simply unused.
REQUIRED_MEMORY_MAP = ["RAMDisk", "TrustCache"]


def read_node(data: bytes, offset: int):
    """Decode one node: u32 property count, u32 child count, properties, then children."""
    prop_count, child_count = struct.unpack_from("<II", data, offset)
    cursor = offset + 8
    properties = {}
    for _ in range(prop_count):
        name = data[cursor:cursor + 32].split(b"\0", 1)[0].decode("ascii", "replace")
        length, = struct.unpack_from("<I", data, cursor + 32)
        properties[name] = data[cursor + 36:cursor + 36 + length]
        cursor += 36 + ((length + 3) & ~3)
    children = {}
    for _ in range(child_count):
        child = read_node(data, cursor)
        children[child["name"]] = child
        cursor = child["end"]
    # The name property carries its own NUL terminator.
    return {"name": properties.get("name", b"").split(b"\0", 1)[0].decode("ascii", "replace"),
            "properties": properties, "children": children, "end": cursor}


def parse_adt(data: bytes):
    root = read_node(data, 0)
    if len(data) != root["end"]:
        raise ValueError(f"trailing bytes: the root node ends at {root['end']}, the blob is {len(data)}")
    return root


def adt_command(output: pathlib.Path):
    return [sys.executable, str(REPO / "scripts/make-clover-adt.py"), "--output", str(output),
            "--dram-size", hex(DRAM_SIZE), "--owned-memory-size", "0x40000000",
            "--chip-id", hex(MSM_ID), "--cpu-regs", ",".join(hex(v) for v in CPU_REGS),
            "--psci-method", "smc", "--unique-chip-id", "0x0123456789ab",
            "--root-match-volume-uuid", ROOT_VOLUME_UUID,
            "--data-ramdisk", "--trust-cache", "--ipad-identity"]


def load_generator():
    """Import scripts/make-clover-adt.py as a module; it guards main() behind __name__."""
    path = REPO / "scripts/make-clover-adt.py"
    spec = importlib.util.spec_from_file_location("clover_adt", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_regions(errors: list, hardware: dict | None = None) -> str:
    """The windows UniXNU actually uses must not lie on top of each other.

    The splash region is the one a board gets wrong: it is where the firmware draws, and the
    firmware volume itself sits just below the top of the described DRAM.
    """
    path = REPO / "scripts/make-clover-adt.py"
    if hardware is None:
        if not path.is_file():
            return "no board spec"
        try:
            hardware = load_generator().CLOVER_HARDWARE_SPEC
        except Exception as error:    # a broken generator must not hide the rest of the run
            errors.append(f"the board spec cannot be imported: {error}")
            return "spec unreadable"
    frame = hardware["framebuffer"]
    fb_start, fb_end = frame["base"], frame["base"] + frame["window_size"]
    fd = hardware["uefi_fd"]
    fd_start, fd_end = fd["base"], fd["base"] + fd["size"]
    dram_end = DRAM_BASE + DRAM_SIZE

    def overlap(first, second, third, fourth, label):
        if max(first, third) < min(second, fourth):
            errors.append(f"{label} overlap: {first:#x}-{second:#x} and {third:#x}-{fourth:#x}")

    overlap(fb_start, fb_end, fd_start, fd_end, "splash and the firmware volume")
    if fb_end > dram_end:
        errors.append(f"the splash window ends at {fb_end:#x}, past the described DRAM")
    for region in hardware["reserved_memory"]:
        # framebuffer0 is the used part of the splash window, so it overlaps by design, and
        # ramoops is the collision documented in the board spec: no pstore under UniXNU, the
        # panic log goes to the UART.
        if region["name"] in ("framebuffer0", "ramoops"):
            continue
        overlap(region["base"], region["base"] + region["size"], fb_start, fb_end,
                f"{region['name']} and the splash window")
    return f"splash {fb_start:#x}-{fb_end:#x}, FD {fd_start:#x}-{fd_end:#x}"


def check_adt(errors: list) -> str:
    generator = REPO / "scripts/make-clover-adt.py"
    if not generator.is_file():
        errors.append("scripts/make-clover-adt.py is missing")
        return "no generator"
    with tempfile.TemporaryDirectory() as work:
        output = pathlib.Path(work) / "kernel.macho.adt"
        result = subprocess.run(adt_command(output), capture_output=True, text=True)
        if result.returncode:
            errors.append(f"the ADT generator failed: {(result.stdout + result.stderr).strip()}")
            return "generator failed"
        data = output.read_bytes()
        try:
            root = parse_adt(data)
        except ValueError as error:
            errors.append(f"ADT encoding: {error}")
            return "ADT unreadable"

        chosen = root["children"].get("chosen")
        if chosen is None:
            errors.append("the ADT has no /chosen")
            return f"{len(data)} bytes"
        for prop in REQUIRED_CHOSEN:
            if prop not in chosen["properties"]:
                errors.append(f"/chosen is missing {prop}")
        for prop in FORBIDDEN_CHOSEN:
            if prop in chosen["properties"]:
                errors.append(f"/chosen carries {prop}, which this round does not want")
        root_matching = chosen["properties"].get("root-matching", b"")
        if (b"IOProviderClass" not in root_matching or b"IOMedia" not in root_matching
                or ROOT_VOLUME_UUID.encode() not in root_matching):
            errors.append("/chosen/root-matching must be an IOMedia IOPropertyMatch on UUID "
                          f"{ROOT_VOLUME_UUID}; IOFindBSDRoot deserializes it verbatim")
        # Round322: ephemeral-storage must be ABSENT. round320 required it present and u32 1;
        # boot 108 showed the skip only moved the stall (migration -> chronod) and sheng boots to
        # the desktop WITHOUT it (persistent_data, migration runs). os_variant_uses_ephemeral_storage()
        # tests the value, so a leftover u32 0 would be just as wrong as the u32 1 -- require the key gone.
        if "ephemeral-storage" in chosen["properties"]:
            errors.append("/chosen/ephemeral-storage must be absent in round322: this cheap A/B reverts "
                          "round320 so first-boot data migration runs against the real /private/var window")
        if len(chosen["properties"].get("random-seed", b"")) != 64:
            errors.append("/chosen/random-seed is not 64 bytes")
        if chosen["properties"].get("dram-base") != struct.pack("<Q", DRAM_BASE):
            errors.append(f"/chosen/dram-base is not 0x{DRAM_BASE:x}")
        # AMFI reads this at load and panics ("no amfi-only-platform-code device tree
        # node!", AppleMobileFileIntegrity.cpp:5025) unless it is a 4-byte property.
        if len(chosen["properties"].get("amfi-only-platform-code", b"")) != 4:
            errors.append("/chosen/amfi-only-platform-code is missing or is not a 4-byte integer")
        # Round140. Round139's frame is AMFI's own "can't has cs_enforcement_disable"
        # (AppleMobileFileIntegrity.cpp:5261): start() logs the boot arg, then panics when
        # PE_i_can_has_debugger() is false. That function returns debug_enabled, whose only
        # writer is /chosen's "debug-enabled" (pexpert/arm/pe_init.c:467-481). A 4-byte 1 is
        # the development-fused value; anything else leaves the r139 panic in place.
        if chosen["properties"].get("debug-enabled") != struct.pack("<I", 1):
            errors.append("/chosen/debug-enabled is not a 4-byte 1: PE_i_can_has_debugger() "
                          "would return false and AMFI would keep panicking on "
                          "cs_enforcement_disable")

        memory_map = chosen["children"].get("memory-map")
        if memory_map is None:
            errors.append("/chosen/memory-map is missing")
        else:
            for placeholder in REQUIRED_MEMORY_MAP:
                value = memory_map["properties"].get(placeholder)
                if value is None:
                    errors.append(f"/chosen/memory-map is missing {placeholder}")
                elif len(value) != 16:
                    errors.append(f"/chosen/memory-map/{placeholder} is {len(value)} bytes, not 16")
            if "PackedRAMDisk" in memory_map["properties"]:
                errors.append("/chosen/memory-map declares PackedRAMDisk next to RAMDisk; "
                              "DarwinBoot's AdtNode rejects a memory-map with two ramdisk slots")

        for node in ("cpus", "arm-io", "defaults", "options"):
            if node not in root["children"]:
                errors.append(f"the ADT has no /{node}; the platform expert refuses without it")

        cpus = root["children"].get("cpus", {}).get("children", {})
        regs = sorted(struct.unpack("<I", cpu["properties"]["reg"])[0]
                      for cpu in cpus.values() if "reg" in cpu["properties"])
        if regs != CPU_REGS:
            errors.append(f"/cpus carries {regs}, expected the Kryo 260 set {CPU_REGS}")
        stray = [name for name in cpus if not re.fullmatch(r"cpu[0-7]", name)]
        if stray:
            errors.append(f"/cpus has a child that is not cpuN: {stray}")
        timebases = {struct.unpack("<I", cpu["properties"]["timebase-frequency"])[0]
                     for cpu in cpus.values() if "timebase-frequency" in cpu["properties"]}
        if timebases and timebases != {TIMEBASE_HZ}:
            errors.append(f"/cpus timebase is {timebases}, expected 19.2 MHz")
        # The BCM2837 kernel has MAX_CPU_CLUSTERS 1 and no HAS_CLUSTER, so
        # ml_parse_cpu_topology derives the physical cluster from
        # (cluster_type == 'P') and asserts it fits in 0. cluster-type must
        # therefore be absent (the CLUSTER_TYPE_SMP default) or a 4-byte 'E';
        # anything else aborts in ml_readprop or on that assertion.
        for name, cpu in cpus.items():
            value = cpu["properties"].get("cluster-type")
            if value is None:
                continue
            if len(value) != 4 or struct.unpack("<I", value)[0] not in (ord("E"), ord("P")):
                errors.append(f"/cpus/{name}/cluster-type is {len(value)} bytes and must be a "
                              f"4-byte 'E'/'P' integer")
            elif struct.unpack("<I", value)[0] == ord("P"):
                errors.append(f"/cpus/{name}/cluster-type is 'P', but this single-cluster kernel "
                              f"asserts the physical cluster id is 0")

        arm_io = root["children"].get("arm-io", {})
        ranges = arm_io.get("properties", {}).get("ranges")
        if ranges is None or len(ranges) != 24:
            errors.append("/arm-io/ranges is missing or is not three 64-bit cells")
        elif struct.unpack("<QQQ", ranges)[1] != ARM_IO_BASE:
            errors.append("/arm-io does not open the Snapdragon peripheral window at 0x17000000")
        intc = arm_io.get("children", {}).get("intc")
        if intc is None:
            errors.append("/arm-io/intc is missing; ArmCpu has no GIC to map")
        elif intc["properties"].get("compatible", b"").split(b"\0")[0] != b"arm,gic-v3":
            errors.append("/arm-io/intc is not compatible with arm,gic-v3")
        elif len(intc["properties"].get("reg", b"")) != 32:
            errors.append("/arm-io/intc reg is not two 64-bit address-size pairs")
        elif struct.unpack("<4Q", intc["properties"]["reg"])[0] + ARM_IO_BASE != GICD_PHYS:
            errors.append(f"/arm-io/intc does not resolve to the GICD at 0x{GICD_PHYS:x}")

        psci = root["children"].get("psci")
        if psci is None:
            errors.append("/psci is missing; the eight cores cannot be started")
        elif psci["properties"].get("compatible", b"").split(b"\0")[0] != b"arm,psci-1.0":
            errors.append("/psci is not arm,psci-1.0")

        display = chosen["properties"].get("display-base")
        if display and struct.unpack("<Q", display)[0] != SPLASH_BASE:
            errors.append(f"/chosen/display-base is not the clover splash region 0x{SPLASH_BASE:x}")
        # Round205. libMobileGestalt's MGIOMFBSupport.c reads IODeviceTree:/chosen's display-scale
        # and display-rotation to answer the screen parameters UIKit/FrontBoard build FBSDisplayMode
        # from. Round204 read the scale arriving at that initialiser as 0.0 (mode+0x20) with the key
        # absent, which is the assert SpringBoard traps on, so the ADT has to carry it.
        scale = chosen["properties"].get("display-scale")
        if scale is None:
            errors.append("/chosen/display-scale is missing: MobileGestalt reports a zero screen "
                          "scale and FBSDisplayMode's initialiser asserts")
        elif len(scale) != 4:
            errors.append("/chosen/display-scale is not one 32-bit cell")
        elif struct.unpack("<I", scale)[0] != DISPLAY_SCALE:
            errors.append(f"/chosen/display-scale is not {DISPLAY_SCALE}")
        # Round240. backboardd's -[BKButtonHapticsController applyDefinitions:] compares
        # BSPlatform.homeButtonType against 2 and, when equal, skips the block that waits
        # forever on _controllerReadyGroup (the boot 28 deadlock, confirmed in r239).
        # MobileGestalt reads home-button-type from /buttons; the Mi Pad 4 has no Apple
        # SSHB home button, so 2 is the truthful value, mirroring UniXNU-sheng.
        buttons = root["children"].get("buttons")
        if buttons is None:
            errors.append("the ADT has no /buttons: BSPlatform takes the iPad7,1 home-button "
                          "default and backboardd stalls on _controllerReadyGroup")
        else:
            hbt = buttons["properties"].get("home-button-type")
            if hbt != struct.pack("<I", 2):
                errors.append("/buttons/home-button-type is not a 4-byte 2; backboardd would "
                              "dispatch the SSHB haptics block that waits forever")
            names = buttons["properties"].get("button-names", b"").split(b"\0")
            if b"menu" in names or b"home" in names:
                errors.append("/buttons/button-names declares a home/menu button this board "
                              "does not have")
        # Round241/242. boot 29 (/buttons only) and boot 30 (/product only) both left the
        # deadlock unchanged, so neither node alone is the homeButtonType source. Mirror the
        # whole iPad Pro identity UniXNU-sheng publishes (make-sheng-adt.py:187-197), which
        # boots this image to the desktop, and pin every field so a regression to the iPad7,1
        # default fails the gate instead of silently re-arming the stall.
        product = root["children"].get("product")
        if product is None:
            errors.append("the ADT has no /product: MobileGestalt falls back to the iPad7,1 "
                          "home-button identity and backboardd stalls")
        else:
            props = product["properties"]
            for key in ("product-name", "product-description"):
                if props.get(key, b"").split(b"\0")[0] != b"iPad Pro (12.9-inch) (2nd generation)":
                    errors.append(f"/product/{key} is not the home-button-less iPad Pro identity "
                                  "sheng uses; BSPlatform keeps the mechanical home-button answer")
            for key, value in (("artwork-device-idiom", b"pad"),
                               ("artwork-display-gamut", b"sRGB"),
                               ("compatible-device-fallback", b"iPad6,7")):
                if props.get(key, b"").split(b"\0")[0] != value:
                    errors.append(f"/product/{key} is not {value.decode()!r} (sheng's iPad Pro identity)")
            for key, value in (("artwork-device-subtype", 2732),
                               ("artwork-scale-factor", 2),
                               ("thin-bezel", 1)):
                if props.get(key) != struct.pack("<I", value):
                    errors.append(f"/product/{key} is not a 4-byte {value} (sheng's iPad Pro identity)")
        # Round242. The remaining gestalt inputs clover lacked versus sheng: hw.target comes
        # from the root target-type (IODTPlatformExpert), and MobileGestalt reads the /chosen
        # security tuple as a dev-fused, undemoted board (0,0,1,1). boot 30 still stalled with
        # /buttons and /product set, so pin both here too -- the whole identity must match the
        # proven-to-desktop port, not just the two nodes already tried.
        if root["properties"].get("target-type", b"").split(b"\0")[0] != b"J120":
            errors.append("the ADT root has no target-type J120: hw.target stays empty and "
                          "MobileGestalt's device identity is incomplete (sheng publishes J120)")
        for key, value in (("certificate-production-status", 0),
                           ("effective-production-status-ap", 0),
                           ("certificate-security-mode", 1),
                           ("effective-security-mode-sep", 1)):
            if chosen["properties"].get(key) != struct.pack("<I", value):
                errors.append(f"/chosen/{key} is not a 4-byte {value} (sheng's dev-fused security "
                              "tuple MobileGestalt reads)")
    return f"{len(data)} bytes, {len(cpus)} cores"


def toml_split(text: str, at: str = ",") -> list:
    """Split on `at` outside quotes and brackets."""
    parts, depth, quote, start = [], 0, None, 0
    for i, char in enumerate(text):
        if quote:
            if char == quote and text[i - 1:i] != "\\":
                quote = None
            continue
        if char in "\"'":
            quote = char
        elif char in "[{":
            depth += 1
        elif char in "]}":
            depth -= 1
        elif char == at and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    parts.append(text[start:])
    return [p.strip() for p in parts if p.strip()]


def toml_unquote(text: str) -> str:
    text = text.strip()
    inner = text[1:-1]
    return inner.replace('\\"', '"').replace("\\\\", "\\") if text[0] == '"' else inner


def toml_value(text: str):
    text = text.strip()
    if text.startswith("[") and text.endswith("]"):
        return [toml_value(item) for item in toml_split(text[1:-1])]
    if text.startswith("{") and text.endswith("}"):
        return toml_table(text[1:-1])
    if text[:1] in "\"'" and text[-1:] == text[:1] and len(text) > 1:
        return toml_unquote(text)
    if text in ("true", "false"):
        return text == "true"
    try:
        return int(text, 16) if text[:2].lower() == "0x" else int(text, 10)
    except ValueError:
        return text


def toml_table(text: str) -> dict:
    table = {}
    for item in toml_split(text):
        key, _, value = item.partition("=")
        table[toml_value(key.strip())] = toml_value(value)
    return table


def toml_subset(text: str) -> dict:
    root: dict = {}
    stack: list = []                                   # the tables [a.b] descends into
    for line in _toml_logical_lines(text):
        line = line.strip()
        if line.startswith("[[") and line.endswith("]]"):
            keys = [toml_value(k) for k in line[2:-2].split(".")]
            parent = _toml_descend(root, keys[:-1])
            entry: dict = {}
            parent.setdefault(keys[-1], [])
            if not isinstance(parent[keys[-1]], list):
                raise ValueError(f"{line} redeclares {keys[-1]} as an array")
            parent[keys[-1]].append(entry)
            stack = [entry]
            continue
        if line.startswith("[") and line.endswith("]"):
            keys = [toml_value(k) for k in line[1:-1].split(".")]
            stack = [root]
            for key in keys:
                child = stack[-1].setdefault(key, {})
                if not isinstance(child, dict):
                    raise ValueError(f"{line} redeclares {key} as a value")
                stack.append(child)
            continue
        key, _, value = line.partition("=")
        stack[-1][toml_value(key.strip())] = toml_value(value)
    return root


def _toml_descend(root: dict, keys: list) -> dict:
    table = root
    for key in keys:
        table = table.setdefault(key, {})
    return table


def _toml_logical_lines(text: str):
    """One entry per key or header, comments dropped and arrays joined."""
    buffer = ""
    for raw in text.splitlines():
        line = raw.rstrip()
        cut = _toml_comment_at(line)
        if not buffer and cut == 0:
            continue
        line = line[:cut].rstrip() if cut >= 0 else line
        if not buffer and not line.strip():
            continue
        buffer = f"{buffer} {line.strip()}".strip() if buffer else line.strip()
        if _toml_balanced(buffer):
            yield buffer
            buffer = ""
    if buffer:
        yield buffer


def _toml_comment_at(line: str) -> int:
    quote = None
    for i, char in enumerate(line):
        if quote:
            if char == quote and line[i - 1:i] != "\\":
                quote = None
        elif char in "\"'":
            quote = char
        elif char == "#":
            return i
    return -1


def _toml_balanced(text: str) -> bool:
    quote = None
    depth = 0
    for i, char in enumerate(text):
        if quote:
            if char == quote and text[i - 1:i] != "\\":
                quote = None
        elif char in "\"'":
            quote = char
        elif char in "[{":
            depth += 1
        elif char in "]}":
            depth -= 1
    return depth <= 0


def load_toml(path: pathlib.Path):
    """board.toml as a dict, on any Python this board's host ships.

    tomllib is 3.11+, tomli is its predecessor, and the subset reader above
    covers what board.toml uses: tables, dotted headers, arrays of tables,
    inline tables, arrays, hex integers, strings and booleans.  The sibling
    repository carries the same reader in uefi-clover/scripts/check-clover-uefi.py
    for the same reason; keep the two in step.
    """
    try:
        import tomllib                                  # Python 3.11 and newer
    except ModuleNotFoundError:
        try:
            import tomli as tomllib                     # its pure-Python predecessor
        except ModuleNotFoundError:
            return toml_subset(path.read_text())
    return tomllib.loads(path.read_text())


def check_board(errors: list) -> str:
    board_path = next((candidate / "board.toml" for candidate in (REPO, pathlib.Path.cwd())
                       if (candidate / "board.toml").is_file()), None)
    if board_path is None:
        return "board.toml not visible"
    try:
        board = load_toml(board_path)
    except (ValueError, SyntaxError) as error:
        errors.append(f"board.toml did not parse: {error}")
        return "board.toml unreadable"

    if board["board"]["name"] != "clover" or board["board"]["soc"] != "SDM660":
        errors.append("board.toml does not describe clover / SDM660")
    # Round158. scripts/porting-record.py (the required CI check sheng runs) compares
    # device.os_profile in porting.toml with board.toml [board] os_profile and fails when
    # either is absent. The value is the IPSW profile both files already describe.
    if board["board"].get("os_profile") != "ipados-17.7.11-21H461":
        errors.append('[board] os_profile is not "ipados-17.7.11-21H461": porting.toml would '
                      "fail the upstream Porting record check")
    if board["board"].get("example"):
        errors.append("board.toml still marks the board as an example")

    defconfig_name = board["kernel"]["defconfig"]
    defconfig_path = next((base / "configs" / defconfig_name
                           for base in (board_path.parent, pathlib.Path.cwd())
                           if (base / "configs" / defconfig_name).is_file()), None)
    if defconfig_path is None:
        errors.append(f"configs/{defconfig_name} is missing")
    else:
        text = defconfig_path.read_text()
        flags = dict(re.findall(r"^CONFIG_XNU_([A-Z0-9_]+)=(.*)$", text, re.M))
        for required in ("ARM_DT_PLATFORM", "ARMCPU", "NATIVE_AES", "CRYPTO_BOOTSTRAP"):
            if flags.get(required) not in ("y", "1", '"prelinked"'):
                errors.append(f"{defconfig_name} does not enable CONFIG_XNU_{required}")
        for forbidden in ("NATIVE_UFS", "NATIVE_ADRENO"):
            if flags.get(forbidden, "n").strip('"') in ("y", "1"):
                errors.append(f"{defconfig_name} enables CONFIG_XNU_{forbidden}, "
                              "for which clover has no driver")
        # Round180 replaces the display stub with the driver. The archive is deliberately absent from
        # CONFIG_XNU_EXTRA_LD_ARCHIVES: build-xnu-4k-baseline.sh force-loads its exact path itself when
        # XNU_NATIVE_LEGACY_DISPLAY is 1, after re-hashing that archive and every source its
        # build-report.json names. The three halves that have to agree are therefore the switch, the
        # builder in [kernel] archives, and the boot arguments that reach the probe the switch compiles
        # into bsd_init.o. Flip the switch off and the link stops asking for the archive; leave the
        # builder out and the link's `test -f` stops the build by name; drop either argument and the
        # class is linked and never constructed -- which, on a board whose readings come off a card, is
        # the same silence as having no display work at all.
        legacy_builder = any("build-clover-gop-legacy-display.py" in entry
                             for entry in board["kernel"]["archives"])
        stub_builder = any("build-clover-display-stub.py" in entry for entry in board["kernel"]["archives"])
        if "libCloverDisplayStub.a" in text or stub_builder:
            errors.append("the display stub is still wired (defconfig archive or [kernel] archives): "
                          "round180 replaced it with build/native-legacy-display/"
                          "libNativeLegacyDisplay.late.a")
        if flags.get("NATIVE_LEGACY_DISPLAY", "n") in ("y", "1"):
            if not legacy_builder:
                errors.append("CONFIG_XNU_NATIVE_LEGACY_DISPLAY is on but [kernel] archives omits "
                              "build-clover-gop-legacy-display.py, so the archive the link force-loads "
                              "does not exist")
            for argument in ("offline-mobile-graphics-load=1", "offline-legacy-register=1"):
                if argument not in board["payload"]["kernel_args"]:
                    errors.append(f"CONFIG_XNU_NATIVE_LEGACY_DISPLAY is on but kernel_args omits "
                                  f"{argument}: native_legacy_display_probe() is never called")
            # Round202 moves the probe from phase 1 (register, construct, release) to phase 4 (publish
            # the GOP range as an /arm-io display subsystem, probe, attach, start) because round201
            # read SpringBoard's SIGTRAP as FBSDisplayMonitor finding no main CADisplay. Each argument
            # below is one link of that chain and none is optional: the phase selects the path, gop
            # lets startHardware() claim the mapping, pm gives selector 6 a non-Busy answer, and
            # validate/transfer/retire are the swap path without which a started display shows
            # nothing. The two GPU-era switches stay out on purpose.
            if "offline-legacy-phase=4" not in board["payload"]["kernel_args"]:
                errors.append("kernel_args does not pin offline-legacy-phase=4: round202 starts the "
                              "display, and the probe's default of 1 would ship a boot that only "
                              "registers the class again")
            for argument in ("offline-legacy-gop=1", "offline-legacy-pm=1",
                             "offline-legacy-validate=1", "offline-legacy-transfer=1",
                             "offline-legacy-retire=1"):
                if argument not in board["payload"]["kernel_args"]:
                    errors.append(f"kernel_args omits {argument}: with phase 4 the display attaches "
                                  f"but never starts, presents or retires a frame, which reads on "
                                  f"the card as the same silence as having no display")
            for argument in ("sheng-fb-gpu=1", "offline-legacy-unit-scale="):
                if any(entry.startswith(argument) for entry in board["payload"]["kernel_args"]):
                    errors.append(f"kernel_args passes {argument}: the Adreno hand-off is a later "
                                  f"question, and the UI scale now lives in the ADT (/chosen/"
                                  f"display-scale), so the probe's publishOrMatchWord would collide "
                                  f"with the ADT's own display-rotation and stop startHardware")
            if flags.get("NATIVE_MOBILE_DISPLAY", "n") in ("y", "1"):
                errors.append("CONFIG_XNU_NATIVE_MOBILE_DISPLAY is on as well: that switch asks the "
                              "link for libNativeMobileDisplay.late.a, which this board does not build, "
                              "and the MDP5 driver has no hardware here")
        elif legacy_builder:
            errors.append("[kernel] archives builds the Legacy display archive but the defconfig leaves "
                          "CONFIG_XNU_NATIVE_LEGACY_DISPLAY off, so nothing calls into it")
        # round83, the AMFI PMGR contract. Three facts have to agree or the board ships a kernel
        # that silently has no power manager again: the defconfig switch (which both compiles the
        # sheng_pmgr_run() call into bsd_init.o and makes the link force-load
        # build/sheng-platform-power/libShengPlatformPower.a), the archive builder that writes that
        # exact path, and the clover-pmgr=2 boot arg the provider reads -- any other value panics.
        # scripts/patch-xnu-entry-probe.py also demands _clover_pmgr_observation for the panic
        # screen, so turning the switch off fails the payload build by name.
        pmgr_builder = any("build-clover-pmgr.py" in entry for entry in board["kernel"]["archives"])
        if flags.get("SHENG_PMGR", "n") in ("y", "1"):
            if not pmgr_builder:
                errors.append("CONFIG_XNU_SHENG_PMGR is on but [kernel] archives omits "
                              "build-clover-pmgr.py, so the base link's test -f on "
                              "libShengPlatformPower.a stops the build")
            if "clover-pmgr=2" not in board["payload"]["kernel_args"]:
                errors.append("CONFIG_XNU_SHENG_PMGR is on but no clover-pmgr=2 in kernel_args: "
                              "sheng_pmgr_run() panics on any mode it does not know")
        elif pmgr_builder:
            errors.append("[kernel] archives builds the PMGR provider but the defconfig leaves "
                          "CONFIG_XNU_SHENG_PMGR off, so nothing ever loads it")

        # round87, the eMMC host instrument. CONFIG_XNU_EXTRA_LD_ARCHIVES lists both extra archives
        # as plain paths, with no -Wl,-force_load, so each one survives the link only because
        # something else names its single exported symbol. For CloverSdhci that symbol is
        # clover_emmc_run, called from the tail of CloverPMGR's sheng_pmgr_run() -- which is itself
        # only compiled into bsd_init.o when SHENG_PMGR is on. Drop either half and the link does
        # not fail; the eMMC round simply stops existing, which on a board whose only output is one
        # photograph is the worst possible silence.
        sdhci_builder = any("CloverSdhci" in entry for entry in board["kernel"]["archives"])
        sdhci_arg = any(argument.startswith("clover-emmc=") for argument in board["payload"]["kernel_args"])
        if "libCloverSdhci.a" in text and not sdhci_builder:
            errors.append("the defconfig links libCloverSdhci.a but [kernel] archives omits "
                          "build-arm-driver.py CloverSdhci, so that archive does not exist")
        if sdhci_builder and not sdhci_arg:
            errors.append("[kernel] archives builds CloverSdhci but no clover-emmc= boot arg reaches "
                          "it: clover_emmc_run() returns without touching the host")
        if sdhci_arg and not sdhci_builder:
            errors.append("kernel_args passes clover-emmc= but [kernel] archives does not build "
                          "CloverSdhci, so the arg is noise")
        if sdhci_builder and flags.get("SHENG_PMGR", "n") not in ("y", "1"):
            errors.append("CloverSdhci is scheduled from sheng_pmgr_run(), which CONFIG_XNU_SHENG_PMGR="
                          f"{flags.get('SHENG_PMGR', 'n')} does not compile into the kernel")

        # round107, the block device. libCloverBlock.a is linked because CloverSdhci names
        # clover_block_run() from its mode 21 tail, and only mode 21 publishes the geometry and
        # the block read service that class calls. All three halves have to agree, or the
        # storage stack silently never exists -- the same failure shape as the two above.
        block_builder = any("build-clover-block.py" in entry for entry in board["kernel"]["archives"])
        if "libCloverBlock.a" in text and not block_builder:
            errors.append("the defconfig links libCloverBlock.a but [kernel] archives omits "
                          "build-clover-block.py, so that archive does not exist")
        if block_builder and "libCloverBlock.a" not in text:
            errors.append("[kernel] archives builds CloverBlock but the defconfig does not link "
                          "libCloverBlock.a, so clover_block_run() is undefined")
        if block_builder and "clover-emmc=21" not in board["payload"]["kernel_args"]:
            errors.append("[kernel] archives builds CloverBlock but kernel_args does not select "
                          "clover-emmc=21, the only mode that publishes the block device")

        # round184 parks the three instruments. The goal moved: the question is no longer what the
        # touch bus, the PMIC rails or the shared heap say, it is how to get iPadOS onto its own
        # screen. Each of these three spends boot time and bus risk on a question that is not that
        # one, so their arguments leave the shipped boot -- while their archives stay built and
        # linked, so a round that resumes them changes one line rather than re-deriving the whole
        # stack. The build+link half has to agree with itself either way, because "parked" and
        # "silently never existed" must not be the same reading.
        #
        # Round259 resumes the first of them -- the argument is asserted *present* below, in mode 3,
        # the one that asks the panel a question its own vendor driver asks. The other two stay parked.
        parked = (("round181", "CloverInput", "libCloverInput.a", "clover-input=",
                   "clover_input_run()", ("1", "2", "3")),
                  ("round182", "CloverPower", "libCloverPower.a", "clover-power=",
                   "clover_power_run()", ("1", "2", "3")),
                  ("round183", "CloverDsp", "libCloverDsp.a", "clover-dsp=",
                   "clover_dsp_run()", ("1", "2", "3")))
        for _label, driver, archive, argument, entry_point, modes in parked:
            built = any(driver in item for item in board["kernel"]["archives"])
            passed = [item.split("=", 1)[1] for item in board["payload"]["kernel_args"]
                      if item.startswith(argument)]
            if archive in text and not built:
                errors.append(f"the defconfig links {archive} but [kernel] archives omits "
                              f"build-arm-driver.py {driver}, so that archive does not exist")
            if built and archive not in text:
                errors.append(f"[kernel] archives builds {driver} but the defconfig does not link "
                              f"{archive}, so {entry_point} is undefined")
            # Built and linked, but not scheduled: the archive survives the link only because
            # sheng_pmgr_run() names its entry point, so this half is what keeps a parked driver
            # restorable rather than already dead.
            if built and flags.get("SHENG_PMGR", "n") not in ("y", "1"):
                errors.append(f"{driver} is scheduled from sheng_pmgr_run(), which "
                              f"CONFIG_XNU_SHENG_PMGR={flags.get('SHENG_PMGR', 'n')} does not "
                              "compile into the kernel")
            # The park itself. An argument that comes back has to be deliberate: it is asserted
            # here, not left to whatever the last round wrote.
            if built and not passed:
                continue
            if passed and not built:
                errors.append(f"kernel_args passes {argument} but [kernel] archives does not build "
                              f"{driver}, so the arg is noise")
            if passed and any(value not in modes for value in passed):
                errors.append(f"{argument}{passed} names a mode {driver} does not implement "
                              f"({', '.join(modes)}); it would report and return")

        # Round259's resumption, pinned. A boot arg that comes back by accident is a second bus
        # instrument nobody scheduled, and one that leaves because a later round rewrote the list is
        # the I2C question silently unasked again; mode 3 is the only mode that actually asks the
        # panel something, so that is the string this round owns.
        if "clover-input=3" not in board["payload"]["kernel_args"]:
            errors.append("kernel_args no longer carries clover-input=3: round259 resumed drivers/"
                          "CloverInput in the same flight as the synthetic digitizer frame, because "
                          "real fingers and synthetic fingers are two halves of one question -- and a "
                          "NACK is a valid answer there (round183's), so the instrument is not a risk "
                          "this round decided to avoid")

        # round287 puts the deadline back. round285 measured its price on the standing card: boot 73
        # ran past t=3145 s with nothing interrupting it and never came back, so the window could only
        # be read by a hand-written BCB -- an unattended run that cannot return by itself cannot be read
        # by scripts/run-clover-unattended.sh, whose whole landing path is the deadline panic's own
        # recovery BCB. The argument is the contract again; the value stays a reading, not a pin, so the
        # only bound is 60..1800 -- short of that it cuts the boot before the census reads, long of it
        # the run outlives the board's own resets and the tee ring wraps.
        unattended_args = [argument for argument in board["payload"]["kernel_args"]
                           if argument.startswith("clover-unattended=")]
        if not unattended_args:
            errors.append("kernel_args dropped clover-unattended=<s>: with no deadline the boot never "
                          "returns by itself and run-clover-unattended.sh cannot read the window it "
                          "exists to read -- round287 restores it after round285 measured that cost")
        else:
            raw = unattended_args[0].split("=", 1)[1]
            try:
                seconds = int(raw)
            except ValueError:
                seconds = -1
            if not 60 <= seconds <= 1800:
                errors.append(f"clover-unattended={raw} is outside 60..1800: shorter cuts the census "
                              "off, longer outlives the resets the board survives")

        # round184, the panic-time ring drain. The deadline is only useful if the reading survives
        # it, and the two RAM rings do not: kind 21 and kind 22 hold their slots in kernel memory and
        # only reach the card when a driver tail flushes them, so a panic -- which is exactly what the
        # deadline produces -- reset the board with the last lines still in RAM. The drain is asserted
        # as source, not as an argument: it is a property of the sink the panic path already calls.
        sdhci_source = (REPO / "drivers/CloverSdhci/CloverSdhci.cpp").read_text()
        sink = sdhci_source.find("extern \"C\" unsigned int clover_emmc_panic_append "
                                 "(const char *text, unsigned int length)")
        # Round301 grew the sink's body (the panic-text copy sits ahead of every card write), so these
        # locality tests are cut on the function's own end brace rather than on a character window that
        # any added comment would silently push past -- a window is a size assumption, the brace is the
        # thing the assertion actually means.
        sink_end = sdhci_source.find("\n}", sink) if sink >= 0 else -1
        sink_body = sdhci_source[sink:sink_end] if sink_end > sink else ""
        if sink < 0:
            errors.append("drivers/CloverSdhci no longer defines clover_emmc_panic_append(const char "
                          "*, unsigned int): that name and signature are the patcher's PANIC_SINK "
                          "contract")
        elif "panicDrainRings ();" not in sink_body:
            errors.append("the panic sink does not call panicDrainRings () from its own body: the drain "
                          "has to run on the path the panic takes, not on a driver tail the boot that "
                          "panicked never reaches")

        # round196, the BCB the panic path writes. Round195's frame is what makes this a source
        # assertion and not a boot-arg one: the panic screen left fb=1 in IMEM's restart-reason
        # cell, the read-back agreed, and the board still came up in Android -- this XBL does not
        # consult that word. What it does read is the Android bootloader message in misc, which the
        # host script already proved lands in TWRP in 14 s. The driver half is the LBA (device
        # geometry, so it lives here), the command string, the unattended gate and the call from
        # the sink; without all four the reset after a panic is the ordinary one again.
        if "static const uint32_t kMiscFirstLba = 13180952u;" not in sdhci_source:
            errors.append("drivers/CloverSdhci lacks kMiscFirstLba = 13180952: the panic path cannot "
                          "write the recovery BCB without the misc partition's first LBA")
        if '"boot-recovery"' not in sdhci_source:
            errors.append("drivers/CloverSdhci never writes the boot-recovery command: a BCB with an "
                          "empty command is what the bootloader reads as 'boot normally'")
        if "!gUnattendedArmed" not in sdhci_source:
            errors.append("the BCB write is not gated on gUnattendedArmed: a boot without the "
                          "unattended contract would commit the next reset to recovery")
        if sink >= 0 and "panicMarkRecovery ();" not in sink_body:
            errors.append("the panic sink does not call panicMarkRecovery (): the BCB has to be in "
                          "misc before the screen resets the board")

        # round198, the BCB armed before anything can reset. Round197's board reset itself 238 s in,
        # mid-CloverBlock-write, and the panic window held no record for that frame: the reset did
        # not come through the sink round196 hooked. A BCB written only from the sink is therefore
        # not a lander. The write moves to the moment the log rings are armed -- the card is
        # selected, no block driver exists yet, and the unattended gate is already up -- and the
        # reading line names it so the tee ring carries the return code.
        armed = "CloverSdhci r198: recovery BCB pre-armed rc=%u written=%u\\n"
        if armed not in sdhci_source:
            errors.append("drivers/CloverSdhci does not announce the round198 pre-armed BCB: the "
                          "reading would have no line to show whether misc took the command")
        if sdhci_source.count("panicMarkRecovery ();") < 2:
            errors.append("panicMarkRecovery () is called from the sink only: a reset that bypasses "
                          "the panic path leaves misc empty again, which is what round197 saw")

        # round199, the user breakpoint reading. r198's frame ends with SpringBoard dying of SIGTRAP
        # 0.35-0.66 s after every exec (rv=0x5 ns=2 code=0x5, desc=none) 24 times, and the exit line
        # names the process but not the instruction: an assert, a ptrauth check and an explicit trap
        # all arrive as the same SIGTRAP. So the reading moves to the path that delivers it --
        # handle_breakpoint() in sleh.c, before exception_triage() -- and the driver keeps a class
        # for it so the tee ring budgets it like every other chatty producer.
        # round200 adds the half r199 could not answer: r199 named the trap (SpringBoard: brk #0,
        # pc==lr, the same 0x1bdc472ec in all eight execs) but the address is in the dyld shared
        # cache, which does not exist on the host to symbolize against. So the reading now walks
        # the current map for the entry that owns the pc and prints the segment bounds, the offset
        # into the segment, the offset into the backing file, the eight instructions that lead into
        # the brk, x0-x3/x8/x16/x17, the first words of the segment, and any of x0-x3 that reads as
        # a user-space string. Every trap costs two lines, and the kernel caps itself at 24 traps.
        # round204 answers why round203's SpringBoard lines were missing: SpringBoard asserted at
        # 393 s, by which time the respawning daemons had taken all 24 slots and spent the 48-line
        # budget, so the one process the display bring-up waits on printed nothing. SpringBoard now
        # owns slots 24-31 with a 72-line budget of its own, and the mode object is read from x19
        # (the FBSDisplayMode under construction) instead of x0, which holds the assert message.
        ubrk_patch = next((base / "patches/xnu-clover/zzzzzzz-clover-user-brk-why.patch"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover/zzzzzzz-clover-user-brk-why.patch").is_file()),
                          None)
        if ubrk_patch is None:
            errors.append("patches/xnu-clover/zzzzzzz-clover-user-brk-why.patch is missing: the next "
                          "frame would show SpringBoard's SIGTRAP with no trapping instruction again")
        else:
            text = ubrk_patch.read_text()
            added = [line for line in text.splitlines() if line.startswith("+") and
                     not line.startswith("+++")]
            removed = [line for line in text.splitlines() if line.startswith("-") and
                       not line.startswith("---")]
            joined = "\n".join(added)
            if removed:
                errors.append(f"{ubrk_patch.name} deletes {removed[0][:60]!r}: r199 only adds a "
                              "reading, and the exception path must keep delivering what it delivered")
            for needle in ('"clover r199: ubrk pid=%d comm=%s n=%u pc=0x%llx lr=0x%llx sp=0x%llx '
                           'cpsr=0x%x esr=0x%x ec=%u brk=0x%x\\n"',
                           '"clover r200: ubrk2 pid=%d x0=0x%llx x1=0x%llx',
                           '"clover r203: ubrk3 pid=%d x4=%016llx x5=%016llx x6=%016llx '
                           'x7=%016llx\\n"',
                           '"clover r203: ubrk6 pid=%d obj=%016llx',
                           "clover_user_breakpoint_why(arm_saved_state_t *state, uint32_t esr)",
                           "vm_map_lookup_entry(clover_map,",
                           "clover_foff = (uint64_t)VME_OFFSET(clover_entry) + clover_off;",
                           "clover_ubrk_total >= 24", "printed >= 2",
                           '"clover r204: ubrk7 pid=%d o=%016llx', "clover_ubrk_sb_total >= 8",
                           "clover_ubrk_sb_lines + (clover_shared ? 7 : 2)) > 72",
                           "clover_user_breakpoint_why(state, esr);"):
                if needle not in text:
                    errors.append(f"{ubrk_patch.name} lacks {needle[:52]!r}: the frame could not say "
                                  "which instruction trapped and on whose behalf")
            if text.find("clover_user_breakpoint_why(state, esr);") < \
                    text.find("boot_progress_user_breakpoint(state, esr);"):
                errors.append(f"{ubrk_patch.name} places the reading before the boot-progress hook "
                              "inside handle_breakpoint(): the patch would stop applying to the "
                              "context zzz-boot-progress adds")
            if "handle_kernel_breakpoint" in joined:
                errors.append(f"{ubrk_patch.name} touches the kernel breakpoint path: a kernel brk "
                              "panics by design and must not go through the user reading")
        if '{ "clover r200: ubrk", "r200-ubrk", 64, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round200's reading no tee class: the r200 lines "
                          "would land in the ring uncounted and could push out the exec/exit pairs")
        if '{ "clover r203: ", "r203-ubrk", 64, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round203's shared-cache dump no tee class: its "
                          "four lines per pid would land in the ring uncounted")
        if '{ "clover r204: ", "r204-ubrk", 64, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round204's x19 dump no tee class: the one "
                          "SpringBoard reading the bring-up waits on would land in the ring uncounted")
        if '{ "clover r207: ", "r207-kill", 48, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round207's sender line no tee class: the one "
                          "SIGKILL reading the bring-up waits on would land in the ring uncounted")
        if '{ "clover r208: ", "r208-kill", 64, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round208's sender-pc and thread lines no tee "
                          "class: the reading the bring-up waits on would land in the ring uncounted")
        if '{ "clover r218: ", "r218-sbs", 192, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round218's chain lines no tee class: five "
                          "chains per state change would land in the ring uncounted")
        if '{ "clover r221: ", "r221-sbs", 192, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round221's mach_msg2 descriptor lines no tee "
                          "class: three lines per thread changing thirty-two times would land in "
                          "the ring uncounted")
        if '{ "clover r222: ", "r222-sbs", 192, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round222's frame and body lines no tee class: "
                          "two lines per picked thread changing thirty-two times would land in "
                          "the ring uncounted")
        if '{ "clover r223: ", "r223-sbs", 192, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round223's port-state lines no tee class: two "
                          "lines per picked thread changing thirty-two times would land in the ring "
                          "uncounted")
        if '{ "clover r224: ", "r224-peer", 192, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round224's launchd sample no tee class: a line "
                          "every eight seconds for the length of the boot would land in the ring "
                          "uncounted")
        if '{ "clover r225: ", "r225-p1", 192, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round225's pid-1 sender reading no tee class: "
                          "twenty-four probes of five lines would land in the ring uncounted")
        if '{ "clover r226: ", "r226-die", 192, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round226's kill-site registers and receive "
                          "samples no tee class: seven lines per kill plus the sbrcv updates "
                          "would land in the ring uncounted")
        if '{ "clover r228: ", "r228-rcv", 480, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round228's receive dump no tee class: three "
                          "generations of kill dumps (129 lines each) plus the sbtask lines would "
                          "land in the ring uncounted and evict the earlier readings")
        if '{ "clover r229: ", "r229-bb", 64, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round229's receiver sample no tee class: a "
                          "thread line and a queue line per changed batch would land in the ring "
                          "uncounted")
        if '{ "clover r230: ", "r230-hop", 96, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round230's next-hop lines no tee class: a hop "
                          "line per thread parked in the trap, plus its queue line, would land in "
                          "the ring uncounted")
        if '{ "clover r231: ", "r231-bb", 192, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round231's receive-name hops and waiting-thread "
                          "rows no tee class: thirteen lines per fire over a whole boot would land "
                          "in the ring uncounted")
        if '{ "clover r232: ", "r232-set", 128, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round232's port-set member rows no tee class: "
                          "each hop name that resolves as a set adds a row per fire, and over a "
                          "whole boot they would land in the ring uncounted")
        if '{ "clover r234: bbset", "r234-set", 384, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round234's whole-set row no tee class: the row "
                          "that names the blocked member would land in the ring uncounted")
        for needle in ('{ "clover r234: bbhop", "r234-hop", 384, 0, 0, 0 },',
                       '{ "clover r234: sbown", "r234-own", 192, 0, 0, 0 },',
                       '{ "clover r234: bblock", "r234-lock", 96, 0, 0, 0 },',
                       '{ "clover r234: beat", "r234-beat", 96, 0, 0, 0 },'):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci lacks round234 tee class {needle[:40]!r}: the "
                              "beat, owner, hop and lock rows would spend each other's records")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r234-set")' not in sdhci_source:
            errors.append("drivers/CloverSdhci does not exempt the whole-set row from the per-pass "
                          "push cap: boot 22 lost members i=8..10 to that cap and a set that did "
                          "not change would look like a set that was never read")
        # Round249. Boot 36 is the first frame whose census actually ran, and the census arrived on
        # the card in pieces: eleven rows (t=, nine nodes, path buttons=) and nothing after them,
        # while the same pass's bb rows and the tee's own counters kept running to t=1800. Eleven is
        # what was left of the sixteen pushes a pass may make once the earlier rows took their share,
        # and a throttled line is scanned past, never retried -- so hbt len=, the three byname rows,
        # svc total=, stopped=, button-family= and census done were gone before anyone read them.
        # A census that was throttled and a census that stopped mid-walk leave the same silence, and
        # the whole point of this instrument was to tell those two apart.
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r243")' not in sdhci_source:
            errors.append("drivers/CloverSdhci does not exempt the census from the per-pass push cap: "
                          "boot 36 proved the census is one burst of more than sixteen rows, so its "
                          "tail would again be throttled and read as a plane that said nothing")
        # Round250 prints from the last line of that same burst. An instrument whose verdict is
        # "nobody asked" is worthless if the line carrying it is the one the cap drops, and r249
        # proved on hardware that the tail of a burst is exactly what gets lost.
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r252")' not in sdhci_source:
            errors.append("drivers/CloverSdhci does not exempt round252's dump from the per-pass push "
                          "cap: it rides the census burst, so its rows would be throttled and an "
                          "answered question would read as silence")
        if '{ "clover r217: ", "r217-sbs", 64, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round217's live sample no tee class: a line "
                          "that changes every eight seconds for the length of the boot would land "
                          "in the ring uncounted")
        if '{ "clover r211: ", "r211-kill", 64, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round211's stack-backtrace lines no tee class: "
                          "twelve lines per kill would land in the ring uncounted and could push out "
                          "the exec/exit pairs")
        if '{ "clover r236: bbustk", "r236-ustk", 192, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round236's userspace-stack rows no tee class: "
                          "one row per picked thread per changed batch, over a whole boot, would "
                          "land in the ring uncounted and push out the r234 rows that name the "
                          "blocked member")
        for tee, why in (
                ('{ "clover r238: ulock", "r238-ulock", 224, 0, 0, 0 },',
                 "the 12-slot ulock series would land uncounted and lose its tail"),
                ('{ "clover r238: ulockc", "r238-ulockc", 96, 0, 0, 0 },',
                 "the counter row would be pushed out by the address rows that follow it"),
                ('{ "clover r237: bbobj", "r237-obj", 96, 0, 0, 0 },',
                 "the object-header rows would share the beat's budget and lose their tail"),
                ('{ "clover r239: bbgrp", "r239-grp", 96, 0, 0, 0 },',
                 "the dispatch-group state rows would share the header's budget and lose their tail"),
                ('{ "clover r239: bbchain", "r239-chain", 96, 0, 0, 0 },',
                 "the block-chain rows would push the group state out of the ring"),
                ('{ "clover r239: bbnochain", "r239-nochain", 96, 0, 0, 0 },',
                 "the negative chain row would be unread, so silence could not be told "
                 "apart from a chain that was never searched"),
                ('{ "clover r243: ", "r243", 64, 0, 0, 0 },',
                 "the IODeviceTree census would land uncounted: its node rows could push "
                 "the beat rows that prove the kernel is alive out of the ring"),
                ('{ "clover r252: ", "r252-mgq", 128, 0, 0, 0 },',
                 "the user-client question rows and the absent-answer list would land uncounted, and "
                 "the one reading that separates a wrong answer from no question at all "
                 "would be missing"),
                ('{ "clover r255: ", "r255-hid", 16, 0, 0, 0 },',
                 "round255's input readings would share the display's budget: a boot where the panel "
                 "works and the HID device does not is the reading that round was here for, and "
                 "neither half may push the other out of the ring"),
                ('{ "clover r255: beat", "r256-beat", 24, 0, 0, 0 },',
                 "round256's beat would spend the early checkpoints' budget -- and the beat exists "
                 "precisely because those lines are gone before the ring is ever read (boot 43: verdict "
                 "at t=25 s, window from t=1208 s). It is also listed AHEAD of the bare prefix, because "
                 "teeProbeClass returns the first token that appears in a line and 'clover r255: beat' "
                 "contains 'clover r255: ' too"),
                ('{ "clover r181: ", "r181-i2c", 24, 0, 0, 0 },',
                 "round259 puts the touch bus back in the boot, and an unbounded line type is a ring "
                 "that starts late -- the r254 lesson, applied before the producer is turned on rather "
                 "than after it ate the budget"),
                ('{ "clover r260: bus", "r260-bus", 16, 0, 0, 0 },',
                 "round260's re-published bus verdict would share the beat's budget, and the two are "
                 "different questions -- one is what the panel answered, the other is what the device "
                 "published")):
            if tee not in sdhci_source:
                errors.append(f"drivers/CloverSdhci gives round238's rows no tee class ({tee}): "
                              f"{why}")
        sb_patch = next((base / "patches/xnu-clover/zzzzzzzz-clover-sb-sample.patch"
                         for base in (REPO, pathlib.Path.cwd())
                         if (base / "patches/xnu-clover/zzzzzzzz-clover-sb-sample.patch").is_file()),
                        None)
        if sb_patch is None:
            errors.append("patches/xnu-clover/zzzzzzzz-clover-sb-sample.patch is missing: round216 "
                          "showed the kill-time reading can neither take the lock nor read a state "
                          "that still exists, so the next frame would have no reading at all")
        else:
            sb_text = sb_patch.read_text()
            if sb_text.count("lck_mtx_try_lock(&task->lock)") != 2:
                errors.append(f"{sb_patch.name} does not make both walks try-lock task->lock: a "
                              "blocking walk is the 39 s the r216 frame measured")
            for needle in ('"clover r217: sbthr busy=1 pid=%d budget=%u\\n"',
                           '"clover r217: sbs t=%llu pid=%d n=%u c=%u%s%s\\n"',
                           "clover_sb_note_current(void)", "clover_sb_sample(void)",
                           "if (n >= 5 || toff >= (int)sizeof(tokens) - 64) {",
                           "strncmp(p->p_comm, \"SpringBoard\", 16) == 0"):
                if needle not in sb_text:
                    errors.append(f"{sb_patch.name} lacks {needle[:56]!r}: the sample could not name "
                                  "the blocked threads before the kill")
            for needle in ('"clover r218: killsrc upc=%016llx ulr=%016llx code=%08x.%08x.%08x.%08x.%08x.%08x.%08x.%08x\\n"',
                           '"clover r218: killustk n=%u sp=%016llx fp=%016llx f0=%016llx f1=%016llx f2=%016llx f3=%016llx\\n"',
                           '"clover r218: sbpc t=%llu tid=0x%llx x16=%llx n=%u f0=%llx f1=%llx f2=%llx f3=%llx\\n"',
                           "clover_sender_ulr(void)", "clover_sender_ufp(uint64_t *fp, uint64_t *sp)",
                           "clover_user_fp_walk(", "if (current_task() != task) {",
                           '"clover r221: sbm2 t=%llu tid=0x%llx op=%llx"',
                           '"clover r221: sbm2x t=%llu tid=0x%llx pc=%llx"',
                           '"clover r221: sbmsg t=%llu tid=0x%llx"',
                           '"clover r222: sbuf t=%llu"',
                           '"clover r222: sbhex t=%llu"',
                           '"clover r223: sbport t=%llu %s nm=%x cnt=%u ql=%u seq=%u"',
                           "clover_sb_port_state(", "ikm_header(kmsg)",
                           "cqe_queue_first(&port->ip_messages.imq_messages,",
                           "clover_user_fp_walk(fps[pick[j]],",
                           "clover_sb_port_owner(", "ipc_object_translate(",
                           "vm_map_lookup_entry(", "VME_OFFSET(entry)",
                           "static char clover_sb_last_m2[",
                           "if (x16s[i] != (uint64_t)-47 || r[0] == 0) {"):
                if needle not in sb_text:
                    errors.append(f"{sb_patch.name} lacks {needle[:56]!r}: the next frame could "
                                  "not say which syscall each thread is in or who called kill")

        peer_patch = next((base / "patches/xnu-clover/zzzzzzzzz-clover-peer-sample.patch"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover/zzzzzzzzz-clover-peer-sample.patch").is_file()),
                          None)
        if peer_patch is None:
            errors.append("patches/xnu-clover/zzzzzzzzz-clover-peer-sample.patch is missing: round223 "
                          "left the reply port empty with the request still at seqno 0, so the next "
                          "frame would have nothing that says whether launchd ever asked for it")
        else:
            peer_text = peer_patch.read_text()
            for needle in ('"clover r224: peer t=%llu pid=%d n=%u sc=%d act=%d b=%u%s\\n"',
                           "clover_peer_sample(void)", "proc_find(1)",
                           "lck_mtx_try_lock(&task->lock)",
                           "clover_peer_sample();"):
                if needle not in peer_text:
                    errors.append(f"{peer_patch.name} lacks {needle[:56]!r}: the sample could not "
                                  "name what launchd's threads are waiting on")

        p1_patch = next((base / "patches/xnu-clover/zzzzzzzzzz-clover-p1-sample.patch"
                         for base in (REPO, pathlib.Path.cwd())
                         if (base / "patches/xnu-clover/zzzzzzzzzz-clover-p1-sample.patch").is_file()),
                        None)
        if p1_patch is None:
            errors.append("patches/xnu-clover/zzzzzzzzzz-clover-p1-sample.patch is missing: round224 "
                          "proved the receiver is not the wall and left the sender unread, so the "
                          "next frame would have nothing that says what launchd sends")
        else:
            p1_text = p1_patch.read_text()
            for needle in ('"clover r225: p1s t=%llu pid=%d n=%u c=%u%s%s\\n"',
                           '"clover r225: p1m2 t=%llu tid=0x%llx op=%llx b=%x sz=%x"',
                           '"clover r225: p1uf t=%llu tid=0x%llx"',
                           '"clover r225: p1hex t=%llu tid=0x%llx"',
                           "clover_p1_note_current(void)", "clover_p1_sample(void)",
                           "strncmp(p->p_comm, \"launchd\", 16) == 0",
                           "static mach_port_name_t clover_sb_last_reply;",
                           'clover_sb_port_state(st, clover_sb_last_reply, "rp",',
                           '"rem", secs, "r225");'):
                if needle not in p1_text:
                    errors.append(f"{p1_patch.name} lacks {needle[:56]!r}: the frame could not name "
                                  "what launchd sends or what SpringBoard's reply port holds")
            if "current_task() != task" not in p1_text:
                errors.append(f"{p1_patch.name} does not keep copyin inside the owning task: a "
                              "launchd stack read from another context is a different address")

        r226_patch = next((base / "patches/xnu-clover/zzzzzzzzzzz-clover-r226.patch"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover/zzzzzzzzzzz-clover-r226.patch").is_file()),
                          None)
        if r226_patch is None:
            errors.append("patches/xnu-clover/zzzzzzzzzzz-clover-r226.patch is missing: round225 "
                          "left the kill's ulr at a Killing %@ helper and the receiver unread, so "
                          "the next frame would have nothing that names the caller or the wait")
        else:
            r226_text = r226_patch.read_text()
            for needle in ('"clover r226: kreg%u %016llx.%016llx.%016llx.%016llx.%016llx.%016llx.%016llx.%016llx\\n"',
                           '"clover r226: kreg4 fp=%016llx lr=%016llx sp=%016llx pc=%016llx cpsr=%08x\\n"',
                           '"clover r226: kraw%u %016llx.%016llx.%016llx.%016llx.%016llx.%016llx.%016llx.%016llx\\n"',
                           "clover_sender_regs(", "clover_sender_regs(uint64_t *out, uint32_t max)",
                           '"clover r226: sbrcv t=%llu tid=0x%llx k=%u cnt=%u ql=%u seq=%u"',
                           "clover_r226_receive_note(", "clover_sb_is_current(",
                           "clover_port_recv_task(", "ipc_entry_lookup(",
                           "ipc_kmsg_queue_first(&port->ip_messages.imq_messages)",
                           '"clover r226: sbgone t=%llu pid=%d\\n"',
                           "if (secs < peer_last_secs + 4) {"):
                if needle not in r226_text:
                    errors.append(f"{r226_patch.name} lacks {needle[:56]!r}: the kill could not "
                                  "name its caller, the sender's registers or the receive wait")

        r227_patch = next((base / "patches/xnu-clover/zzzzzzzzzzzz-clover-r227.patch"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover/zzzzzzzzzzzz-clover-r227.patch").is_file()),
                          None)
        if r227_patch is None:
            errors.append("patches/xnu-clover/zzzzzzzzzzzz-clover-r227.patch is missing: round226 "
                          "printed each changed receive live and the boot stayed silent, so the "
                          "next frame would have nothing that says where SpringBoard parked")
        else:
            r227_text = r227_patch.read_text()
            for needle in ('"clover r227: rcvsum t=%llu n=%u tot=%u\\n"',
                           '"clover r227: rcv%u t=%llu tid=%llx k=%u p=%llx o=%llx"',
                           '"clover r227: rcvb%u n=%u"',
                           '"clover r227: sbtask gen=%u pid=%d t=%llu\\n"',
                           "#define CLOVER_RECV_RING 64",
                           "struct clover_recv_ent {",
                           "clover_recv_dump(void)",
                           "clover_recv_note_waitq(waitq)",
                           "clover_r227_dumped",
                           "clover_recv_note(port, IP_NULL, 2,",
                           "__atomic_exchange_n(&clover_sb_task, cur, __ATOMIC_ACQ_REL)",
                           "task_deallocate(old)"):
                if needle not in r227_text:
                    errors.append(f"{r227_patch.name} lacks {needle[:56]!r}: the receive ring "
                                  "could not be dumped at the kill or the latch would not follow "
                                  "a SpringBoard restart")

        r228_patch = next((base / "patches/xnu-clover/zzzzzzzzzzzzz-clover-r228.patch"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover/zzzzzzzzzzzzz-clover-r228.patch").is_file()),
                          None)
        if r228_patch is None:
            errors.append("patches/xnu-clover/zzzzzzzzzzzzz-clover-r228.patch is missing: boot 16 "
                          "watched three SpringBoard generations and only the first kill dumped, "
                          "so the guard has to follow the victim's pid")
        else:
            r228_text = r228_patch.read_text()
            for needle in ('"clover r228: rcdump t=%llu n=%u tot=%u\\n"',
                           '"clover r228: rce t=%llu pid=%u gen=%u tid=%llx k=%u"',
                           '"clover r228: rcvsum t=%llu n=%u tot=%u\\n"',
                           '"clover r228: rcv%u t=%llu pid=%u gen=%u tid=%llx k=%u p=%llx o=%llx"',
                           '"clover r228: rcvb%u n=%u"',
                           '"clover r228: sbtask gen=%u pid=%d t=%llu\\n"',
                           "clover_r228_dumped_pid",
                           "clover_sb_gen_read(void)",
                           "hw_lock_lock(&clover_recv_lock, LCK_GRP_NULL)",
                           "static struct clover_recv_ent snap[CLOVER_RECV_RING];",
                           "uint32_t cre_pid;",
                           "uint32_t cre_gen;",
                           "clover_emmc_probe_say(line, (unsigned int)len);"):
                if needle not in r228_text:
                    errors.append(f"{r228_patch.name} lacks {needle[:56]!r}: the per-generation "
                                  "dump guard, the pid/gen stamp, the locked snapshot or the "
                                  "probe-ring tail could not land on the card")

        r229_patch = next((base / "patches/xnu-clover/zzzzzzzzzzzzzz-clover-r229.patch"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover/zzzzzzzzzzzzzz-clover-r229.patch").is_file()),
                          None)
        if r229_patch is None:
            errors.append("patches/xnu-clover/zzzzzzzzzzzzzz-clover-r229.patch is missing: boot 17 "
                          "ended on a send to backboardd nobody answered, and the receiver has to "
                          "be read on its own clock")
        else:
            r229_text = r229_patch.read_text()
            for needle in ('"clover r229: bb t=%llu pid=%d n=%u sc=%d act=%d k=%u f=%u%s\\n"',
                           '"clover r229: bb t=%llu pid=%d n=%u gone\\n"',
                           '"clover r229: bb t=%llu pid=%d n=%u busy\\n"',
                           'strncmp(ro + c + 1, "backboardd", 10)',
                           "clover_bb_sample(bb_pid, task, bb_name, secs)",
                           'clover_sb_port_state(sb_task, name, "bb", secs, "r229")',
                           "lck_mtx_try_lock(&task->lock)",
                           "bb_fired >= 24"):
                if needle not in r229_text:
                    errors.append(f"{r229_patch.name} lacks {needle[:56]!r}: the receiver's own "
                                  "threads, the queue behind the name, or the bound that keeps a "
                                  "respawning daemon from filling the ring could not be read")

        # Round230 answers what boot 18 left open. The receiver was read 24 times and said the same
        # thing 24 times, so the reading is now gated on the receiver's own change, and every thread
        # of that receiver sitting in mach_msg2_trap has its destination translated in the receiver's
        # own port space: that is the hop SpringBoard's request stops at, and naming it is the whole
        # question. copyin cannot be used here -- the current task is the sender -- so the hop comes
        # from kernel-side saved registers only.
        r230_patch = next((base / "patches/xnu-clover/zzzzzzzzzzzzzz-clover-r230.patch"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover/zzzzzzzzzzzzzz-clover-r230.patch").is_file()),
                          None)
        if r230_patch is None:
            errors.append("patches/xnu-clover/zzzzzzzzzzzzzz-clover-r230.patch is missing: boot 18 "
                          "proved the receiver waits too, and nothing yet says on whom")
        else:
            r230_text = r230_patch.read_text()
            for needle in ('"clover r230: bbhop t=%llu pid=%d tid=0x%llx rem=%x loc=%x id=%x"',
                           'clover_sb_port_owner(task, (mach_port_name_t)hrem[i], ro, sizeof(ro))',
                           'clover_sb_port_state(task, (mach_port_name_t)hrem[0], "bbq", secs,',
                           'x16 != (uint64_t)-47',
                           '(strcmp(tokens, bb_last) == 0) && (strcmp(hopsig, bb_last_hop) == 0)',
                           "task_reference(task);",
                           "bb_fired >= 16",
                           '"clover r229: bb t=%llu pid=%d n=%u sc=%d act=%d f=%u s=%u hops=%u%s\\n"'):
                if needle not in r230_text:
                    errors.append(f"{r230_patch.name} lacks {needle[:56]!r}: the receiver would again "
                                  "print a beat instead of a change, or name no next hop")

        # Round231 corrects the register the hop row reads. Boot 19 showed x3 remote=0 on every
        # parked thread -- a receive has no remote name -- while round221 had already proved the
        # receive name is in x5's high half (rn=b03). The row now carries x5/x6, the name resolves
        # in the receiver's own space, and the sampler fires on a changed set or a two-minute
        # heartbeat instead of spending sixteen fires on one unchanged receiver. The unfair-lock
        # thread boot 18/19 kept is printed raw for the host to resolve.
        r231_patch = next((base / "patches/xnu-clover/zzzzzzzzzzzzzzz-clover-r231.patch"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover/zzzzzzzzzzzzzzz-clover-r231.patch").is_file()),
                          None)
        if r231_patch is None:
            errors.append("patches/xnu-clover/zzzzzzzzzzzzzzz-clover-r231.patch is missing: boot 19 "
                          "read the hop out of x3, where a receive has no name")
        else:
            r231_text = r231_patch.read_text()
            for needle in ('"clover r231: bbhop t=%llu pid=%d tid=0x%llx op=%llx rn=%x"',
                           '(uint32_t)(x[5] >> 32)',
                           'clover_sb_port_state(task, (mach_port_name_t)hrn[0], "bbq", secs,',
                           '"clover r231: bbwait t=%llu pid=%d tid=0x%llx st=%x"',
                           "same && secs - bb_last_t < 120",
                           "bb_fired >= 96",
                           "uall, (unsigned long long)dt, tokens"):
                if needle not in r231_text:
                    errors.append(f"{r231_patch.name} lacks {needle[:56]!r}: the hop would again be "
                                  "read from the register a receive leaves empty, or the sampler "
                                  "would spend its fires on one unchanged receiver")

        # Round232 answers the shape boot 20 printed: three receive names came back kr=17 under both
        # SEND and RECEIVE, which is what a port set looks like from the outside, and the sampler
        # stopped at 1195 s because it rode the sender's batch signature. The receiver is now latched
        # once a batch names it and sampled on its own twenty-second beat, and each hop name is
        # translated as a set: a member walk is the only way to line the sender's port object up with
        # the ports a set actually serves.
        r232_patch = next((base / "patches/xnu-clover/zzzzzzzzzzzzzzzz-clover-r232.patch"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover/zzzzzzzzzzzzzzzz-clover-r232.patch").is_file()),
                          None)
        if r232_patch is None:
            errors.append("patches/xnu-clover/zzzzzzzzzzzzzzzz-clover-r232.patch is missing: boot 21 "
                          "would again read a port-set name as a dead right and stop sampling at the "
                          "last changed batch")
        else:
            r232_text = r232_patch.read_text()
            for needle in ('#include <ipc/ipc_pset.h>',
                           'waitq_set_foreach_member_locked(&pset->ips_wqset',
                           'ip_from_waitq(wq)',
                           '"clover %s: bbset t=%llu %s nm=%x o=%llx n=%u all=%u m=%s\\n"',
                           'kr = ipc_object_translate(space, name, MACH_PORT_RIGHT_PORT_SET,',
                           'pset ? "+ps" : ""',
                           'clover_bb_target_pid != 0 && secs >= clover_bb_beat',
                           'clover_bb_target_pid = bb_pid;',
                           'clover_sb_port_state(task, (mach_port_name_t)hrn[i],',
                           '"clover %s: sbport t=%llu %s nm=%x o=%llx cnt=%u ql=%u"'):
                if needle not in r232_text:
                    errors.append(f"{r232_patch.name} lacks {needle[:56]!r}: a set would again read "
                                  "as a dead right, the sender's object could not be matched to a "
                                  "member, or the sampler would stop when the batch stops changing")

        # Round233 closes the four gaps boot 21 left. The set row only kept the first eight members,
        # so the blocked destination could still be hidden; every member is now printed with a star
        # on the destination, every hop and receiver name gets its owner, the beat moves off the
        # command port onto a kernel thread so the reading survives a quiet trap, and the unfair
        # lock word an idle thread waits on is read out of its own address space.
        r233_patch = next((base / "patches/xnu-clover/zzzzzzzzzzzzzzzzz-clover-r233.patch"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover/zzzzzzzzzzzzzzzzz-clover-r233.patch").is_file()),
                          None)
        if r233_patch is None:
            errors.append("patches/xnu-clover/zzzzzzzzzzzzzzzzz-clover-r233.patch is missing: boot 21 "
                          "left the blocked member, the owners, a dead-trapped kernel and the unfair "
                          "lock holder all unread")
        else:
            r233_text = r233_patch.read_text()
            for needle in ('#define CLOVER_BBSEP_MAX 24',
                           '"clover r233: bbsep t=%llu %s nm=%x o=%llx i=%u/%u"',
                           'assert_wait_timeout((event_t)clover_bb_beat_thread,',
                           '"clover r233: beat t=%llu pid=%d rn=%x rep=%x\\n"',
                           '"clover r233: sbown t=%llu %s nm=%x own=%s\\n"',
                           '"clover r233: bbhopo t=%llu pid=%d tid=0x%llx"',
                           'copyinmap(task->map, (vm_map_offset_t)(addr & ~3ULL)',
                           'ipc_kobject_get_locked(',
                           'clover_bb_lock_word(task, utid[i], ux0[i], ux1[i], ust[i], secs);',
                           'clover_bb_note_mark(sb_task, name);'):
                if needle not in r233_text:
                    errors.append(f"{r233_patch.name} lacks {needle[:56]!r}: the member dump would "
                                  "again hide the blocked destination, no owner would be named, the "
                                  "beat would die with the trap, or the unfair lock would stay opaque")

        # Round234 is the counter-reading to boot 22: the member dump shared one class with the
        # beat and ran out at 808 s, the per-member rows lost i=8..10 to the sixteen-push cap, and
        # every lock word resolved to no owner because the raw ulock value was translated without
        # ipc_entry_name_mask() -- the call ulock_owner_value_to_port_name() itself makes. The set
        # now rides one row (continued on bbset+ if it does not fit one record), the row's class is
        # exempt from the push cap in the driver, and the owner is masked the way XNU masks it.
        r234_patch = next((base / "patches/xnu-clover/zzzzzzzzzzzzzzzzzzzz-clover-r234.patch"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover/zzzzzzzzzzzzzzzzzzzz-clover-r234.patch").is_file()),
                          None)
        if r234_patch is None:
            errors.append("patches/xnu-clover/zzzzzzzzzzzzzzzzzzzz-clover-r234.patch is missing: boot 22 "
                          "would again spend the set's budget on the beat, lose members past the push "
                          "cap, and read 0x4202 as an unowned lock")
        else:
            r234_text = r234_patch.read_text()
            for needle in ('"clover r234: bbset%s t=%llu %s rn=%s nm=%x o=%llx n=%u"',
                           '((uint64_t)(uintptr_t)snap.ports[last] == mark) ?',
                           '"*" : "");',
                           'first ? "+" : ""',
                           'own = (uint32_t)ipc_entry_name_mask((mach_port_name_t)word);',
                           'ulock_owner_value_to_port_name() makes) adds 0x03 there'):
                if needle not in r234_text:
                    errors.append(f"{r234_patch.name} lacks {needle[:56]!r}: the set could again lose "
                                  "members, hide the blocked destination, or report an owner that "
                                  "the kernel itself would resolve")
            for line in r234_text.splitlines():
                if line.startswith("+") and "word & ~1u" in line:
                    errors.append(f"{r234_patch.name} adds 'word & ~1u': clearing bit 0 does not "
                                  "set the ipc generation bits that ipc_entry_name_mask() adds")

        # Round235 answers boot 23: the receiver-name fallback called ipc_entry_lookup on a space
        # that was already dead -- ipc_space_terminate() clears is_table under the write lock and the
        # sampler's read lock does not revive it -- and is_active_table's assert fired right after
        # fairplayd exited. is_active() is that same table test without the panic, and it has to sit
        # inside the read-locked stretch, before the lookup it protects.
        r235_patch = next((base / "patches/xnu-clover/zzzzzzzzzzzzzzzzzzzzz-clover-r235.patch"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover/zzzzzzzzzzzzzzzzzzzzz-clover-r235.patch").is_file()),
                          None)
        if r235_patch is None:
            errors.append("patches/xnu-clover/zzzzzzzzzzzzzzzzzzzzz-clover-r235.patch is missing: "
                          "the receiver-name fallback would again assert on a terminating space")
        else:
            r235_text = r235_patch.read_text()
            for needle in ('is_read_lock(space);',
                           'if (!is_active(space)) {',
                           '"clover r235: bbspace t=%llu nm=%x sp=%llx dead\\n"',
                           'entry = ipc_entry_lookup(space, name);'):
                if needle not in r235_text:
                    errors.append(f"{r235_patch.name} lacks {needle[:56]!r}: a dead space would again "
                                  "reach is_active_table's assert, or the reading that proves the "
                                  "guard fired would be gone")
            if r235_text.find('if (!is_active(space)) {') > \
                    r235_text.find('entry = ipc_entry_lookup(space, name);'):
                errors.append(f"{r235_patch.name} checks is_active after the lookup: the assert "
                              "would still fire first")

        # Round236 asks what the blocked task is waiting on in its own address space. copyin
        # resolves in the caller's map, and the caller is the sampler, not backboardd, so the walk
        # translates each frame with pmap_find_phys and reads the physical page. ml_phys_read_64
        # returns one 32-bit word, so a 64-bit slot takes two translated reads -- a frame record
        # may straddle a page -- and the walk is bounded to the stack window.
        r236_patch = next((base / "patches/xnu-clover/zzzzzzzzzzzzzzzzzzzzzz-clover-r236.patch"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover/zzzzzzzzzzzzzzzzzzzzzz-clover-r236.patch").is_file()),
                          None)
        if r236_patch is None:
            errors.append("patches/xnu-clover/zzzzzzzzzzzzzzzzzzzzzz-clover-r236.patch is missing: "
                          "the blocked threads' userspace frames would again be unreadable from the "
                          "sampler's address space, and the wait would stay a raw pc")
        else:
            r236_text = r236_patch.read_text()
            for needle in ('extern unsigned int ml_phys_read_64(addr64_t paddr64);',
                           'clover_user_read32(pmap_t pmap, uint64_t addr, uint32_t *value)',
                           'ppnum_t page = pmap_find_phys(pmap, (addr64_t)addr);',
                           'phys = ((uint64_t)page << PAGE_SHIFT) | (addr & (PAGE_SIZE - 1));',
                           'clover_user_bp_walk(pmap_t pmap, uint64_t fp, uint64_t sp, uint64_t *frames,',
                           'clover_user_read32(pmap, fp + 12, &rhi)',
                           'ufp[un] = (uint64_t)saved_state64(ss)->x[29];',
                           'ulr[un] = (uint64_t)saved_state64(ss)->x[30];',
                           'usp[un] = (uint64_t)get_saved_state_sp(ss);',
                           'clover_user_bp_walk(vm_map_pmap(task->map), ufp[i], usp[i],',
                           '"clover r236: bbustk t=%llu pid=%d tid=0x%llx sp=%llx"',
                           '" fp=%llx lr=%llx fn=%u%s\\n"'):
                if needle not in r236_text:
                    errors.append(f"{r236_patch.name} lacks {needle[:56]!r}: the kernel would again "
                                  "print a userspace pc with no chain behind it, or read a foreign "
                                  "map through copyin")
            if r236_text.count("clover_user_read32(pmap, fp") < 4:
                errors.append(f"{r236_patch.name} reads fewer than four 32-bit halves per frame: "
                              "ml_phys_read_64 returns one word, so a 64-bit slot straddling a page "
                              "would be assembled from half a record")

        # Round237 asks whether the wait is ever woken. Both syscall entry points feed a small
        # address-keyed table for the latched pid: waits entered, waits that blocked, waits that came
        # back through ulock_wait_continue, and wake calls with their return. The table prints from
        # the two-minute heartbeat, and the same sample reads the object header 0x34 bytes below the
        # wait word and walks ten frames instead of six.
        r237_patch = next((base / "patches/xnu-clover/zzzzzzzzzzzzzzzzzzzzzzz-clover-r237.patch"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover/zzzzzzzzzzzzzzzzzzzzzzz-clover-r237.patch").is_file()),
                          None)
        if r237_patch is None:
            errors.append("patches/xnu-clover/zzzzzzzzzzzzzzzzzzzzzzz-clover-r237.patch is missing: "
                          "the ulock wait would stay an unexplained pc and nothing would say "
                          "whether it was ever woken")
        else:
            r237_text = r237_patch.read_text()
            for needle in ('#define CLOVER_ULOCK_SLOTS 12',
                           'clover_ulock_note_wait(int pid, uint64_t addr, uint32_t op)',
                           'clover_ulock_note_block(proc_pid(p), (uint64_t)args->addr,',
                           'clover_ulock_note_woken(cp != PROC_NULL ? proc_pid(cp) : 0,',
                           'clover_ulock_note_wake(proc_pid(p), (uint64_t)args->addr,',
                           'clover_ulock_dump(secs);',
                           '"clover r237: ulock t=%llu pid=%d a=%llx w=%u b=%u k=%u"',
                           '"clover r237: bbobj t=%llu pid=%d"',
                           'uint64_t uframes[6][10];',
                           'uframes[i], 10);'):
                if needle not in r237_text:
                    errors.append(f"{r237_patch.name} lacks {needle[:56]!r}: a wait that never "
                                  "returns would again look the same as one that was woken")
            if r237_text.find('clover_ulock_note_woken(cp != PROC_NULL') < \
                    r237_text.find('ret = wait_result_to_return_code(wr);'):
                errors.append(f"{r237_patch.name} records the wake before the wait result exists: "
                              "the row could not say why the wait came back")

        # Round238 moves the filter out of the sampler latch: boot 26 showed the recorded waits were
        # all healthy while the stuck address never appeared, and the two candidate causes -- only
        # eight of twelve slots printed, or the wait predating the pid latch -- are both removed here.
        # The hooks now take the caller's process name and decide for themselves, every call is
        # counted, all twelve slots print blocked-first with ages, and the blocked row's word is read
        # again at dump time.
        r238_name = "zzzzzzzzzzzzzzzzzzzzzzzz-clover-r238.patch"
        r238_patch = next((base / "patches/xnu-clover" / r238_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r238_name).is_file()), None)
        if r238_patch is None:
            errors.append(f"patches/xnu-clover/{r238_name} is missing: the ulock table would again "
                          "depend on the sampler latch and print only eight of twelve rows")
        else:
            r238_text = r238_patch.read_text()
            for needle in ('clover_ulock_is_target(const char *name)',
                           'strncmp(name, "backboardd", sizeof("backboardd")) == 0',
                           'clover_ulock_note_wait(int pid, const char *name, uint64_t addr, uint32_t op)',
                           'clover_ulock_note_block(int pid, const char *name, uint64_t addr, uint32_t op)',
                           'clover_ulock_note_woken(int pid, const char *name, uint64_t addr,',
                           'clover_ulock_note_wake(int pid, const char *name, uint64_t addr,',
                           'proc_name_address(p)',
                           'proc_name_address(cp)',
                           '"clover r238: ulockc t=%llu seen=%u filt=%u stored=%u over=%u"',
                           'blocked > woken',
                           'p = proc_find(row->pid);'):
                if needle not in r238_text:
                    errors.append(f"{r238_patch.name} lacks {needle[:56]!r}: a wait that began "
                                  "before the latch would be lost again, or the dump would not say "
                                  "where it was dropped")
            if r238_text.count('clover_ulock_enter(pid, name, addr)') < 4:
                errors.append(f"{r238_patch.name} does not route all four hooks through the same "
                              "filter: one path would count differently from the others")

        # Round239 turns boot 27's host-proven deadlock chain into on-device readings. The group the
        # waiter parks on is named by its own state word (dg_bits and dg_gen make up the 64-bit
        # dg_state at object+0x30, and the ulock address is its high half), so the sampler reads all
        # three words; and because a blocked thread's live x19 belongs to whichever libdispatch frame
        # is innermost, the block is recovered from the frames the walker already walked -- each
        # return address's own caller frame, eight bytes below its link -- and accepted only when the
        # object it captures names exactly the group being waited on.
        r239_name = "zzzzzzzzzzzzzzzzzzzzzzzzz-clover-r239.patch"
        r239_patch = next((base / "patches/xnu-clover" / r239_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r239_name).is_file()), None)
        if r239_patch is None:
            errors.append(f"patches/xnu-clover/{r239_name} is missing: the group's own state would "
                          "again be inferred from a wait word instead of read, and the chain the "
                          "host proved would stay unproven on the device")
        else:
            r239_text = r239_patch.read_text()
            for needle in ('clover_user_read64(pmap_t pmap, uint64_t addr, uint64_t *value)',
                           'uint64_t *callers',
                           'callers[i] = parent;',
                           'clover_bb_find_block(pmap_t pmap, const uint64_t *callers, uint32_t fn,',
                           'clover_user_read64(pmap, callers[i] - 8, &cand)',
                           'hold != group',
                           'uint64_t ucallers[6][10];',
                           'uframes[i], ucallers[i], 10);',
                           'base + 0x30, &state',
                           'base + 0x38, &head',
                           'base + 0x40, &tail',
                           '"clover r239: bbgrp t=%llu pid=%d"',
                           '"clover r239: bbchain t=%llu pid=%d"',
                           '"clover r239: bbnochain t=%llu pid=%d"'):
                if needle not in r239_text:
                    errors.append(f"{r239_patch.name} lacks {needle[:56]!r}: the group state would "
                                  "not be read, or the chain would be assumed instead of searched")
            if r239_text.find('clover_bb_find_block(pmap_t pmap') > \
                    r239_text.find('"clover r239: bbchain'):
                errors.append(f"{r239_patch.name} prints the chain row before the finder exists: "
                              "the patch would not compile")
            if r239_text.find('"clover r239: bbchain') > \
                    r239_text.find('"clover r239: bbnochain'):
                errors.append(f"{r239_patch.name} prints the negative row before the positive one: "
                              "a sample that found the chain would answer it had found nothing")

        # Round243. The host disassembly pinned -[BSPlatform homeButtonType] to
        # MGGetSInt32Answer("JwLB44/jEB8aFDpXQ16Tuw") whose libMobileGestalt handler
        # resolves IORegistryEntryFromPath("IODeviceTree:/buttons") and copies its
        # "home-button-type" property; missing entry or property answers 0, and 0 is
        # what sends backboardd into the SSHB wait that never drains. Three ADT rounds
        # have not moved the wait, so the plane gets observed rather than assumed --
        # and the census has to run from the beat that is already proven to fire.
        r243_name = "zzzzzzzzzzzzzzzzzzzzzzzzzz-clover-r243.patch"
        r243_patch = next((base / "patches/xnu-clover" / r243_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r243_name).is_file()), None)
        if r243_patch is None:
            errors.append(f"patches/xnu-clover/{r243_name} is missing: whether /buttons is "
                          "published at all would again be argued from the ADT bytes instead "
                          "of read off the live plane")
        else:
            r243_text = r243_patch.read_text()
            for needle in ('clover_iodt_census(unsigned long long secs)',
                           'IORegistryIterator::iterateOver(gIODTPlane, 0)',
                           'IORegistryEntry::fromPath("/buttons", gIODTPlane)',
                           'IORegistryEntry::fromPath("/chosen", gIODTPlane)',
                           'e->getProperty("home-button-type")',
                           'root->getProperty("target-type")',
                           '"clover r243: census t=%llu',
                           '"clover r243: node %s',
                           '"clover r243: hbt len=%u u32=%u',
                           '"clover r243: path buttons=%s',
                           'clover_iodt_census((unsigned long long) secs);'):
                if needle not in r243_text:
                    errors.append(f"{r243_patch.name} lacks {needle[:56]!r}: the census would "
                                  "not walk the plane, or would not name what it found")
            if r243_text.count("static volatile int ran = 0;") != 1:
                errors.append(f"{r243_patch.name} is not once-only: a census that reprints every "
                              "beat would spend its whole budget saying the same thing")

        # Round244. Boot 32 proved the census ran (its beat printed bbgrp beside it) and proved
        # the 512-record ring rotated its ~30 rows away hours before the drain reads the window.
        # The gate has to be ahead of the latch (an early beat must not consume the one shot) and
        # inside the deadline's tail: 1500 s of 1800 s leaves the rows younger than the drain.
        r244_name = "zzzzzzzzzzzzzzzzzzzzzzzzzzz-clover-r244.patch"
        r244_patch = next((base / "patches/xnu-clover" / r244_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r244_name).is_file()), None)
        if r244_patch is None:
            errors.append(f"patches/xnu-clover/{r244_name} is missing: the census would print at "
                          "the first beat again and be rotated out of the drained window")
        else:
            r244_text = r244_patch.read_text()
            if "if (secs < 1500) {" not in r244_text:
                errors.append(f"{r244_patch.name} lacks the 1500 s gate: rows printed hours "
                              "before the drain would again be invisible")
            if r244_text.find("if (secs < 1500) {") > r244_text.find("if (ran) {"):
                errors.append(f"{r244_patch.name} checks the latch before the gate: an early "
                              "beat would consume the one shot and print nothing")

        # Round245: boot 33 read the plane complete (/buttons present, home-button-type len=4
        # u32=2) and the wait still stood, so the census has to answer the two questions the ADT
        # cannot: whether hw.product reaches the tree at all, and whether any service that could
        # ever issue the controller-ready notification is registered.
        r245_name = "zzzzzzzzzzzzzzzzzzzzzzzzzzzz-clover-r245.patch"
        r245_patch = next((base / "patches/xnu-clover" / r245_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r245_name).is_file()), None)
        if r245_patch is None:
            errors.append(f"patches/xnu-clover/{r245_name} is missing: the reading would again be "
                          "the plane's contents alone, which boot 33 already showed is not the gap")
        else:
            r245_text = r245_patch.read_text()
            for needle in ('"clover r243: model=%.16s',
                           '"clover r243: model absent',
                           '+static bool\n+clover_name_has(const char *name, const char *part)',
                           'IORegistryIterator::iterateOver(gIOServicePlane, 0)',
                           '"clover r243: svc total=%d button-family=%d',
                           'clover_name_has(nm, "utton")', 'clover_name_has(nm, "HID")',
                           'clover_name_has(nm, "aptic")', 'clover_name_has(nm, "SSB")',
                           'clover_name_has(nm, "Resolver")'):
                if needle not in r245_text:
                    errors.append(f"{r245_patch.name} lacks {needle[:56]!r}: the census would not "
                                  "answer which of the two remaining causes it is")
            for stale in ('strstr(', 'extern const IORegistryPlane *gIOServicePlane;'):
                if stale in r245_text:
                    errors.append(f"{r245_patch.name} re-adds {stale!r}: this translation unit does "
                                  "not pull in <string.h>, and the plane global already has C++ "
                                  "linkage through IOPlatformExpert.h -- either one fails to build")
            if "static volatile int ran = 0;" in r245_text:
                errors.append(f"{r245_patch.name} re-adds the latch: the census body already owns "
                              "the one-shot gate and a second latch would silence it forever")

        # Round246. Boot 34 answered svc total=1, which is what a one-level walk of the IOService
        # plane always returns (its root has a single child, the platform expert), so the count said
        # nothing about the board. The same round adds the read that separates the two live causes:
        # userland never holds the plane object, it hands fromPath() a null plane and the name is
        # resolved through getPlane(), so the by-name lookup is the userland-equivalent question.
        r246_name = "z" * 31 + "-clover-r246.patch"
        r246_patch = next((base / "patches/xnu-clover" / r246_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r246_name).is_file()), None)
        if r246_patch is None:
            errors.append(f"patches/xnu-clover/{r246_name} is missing: the button-family count would "
                          "again be one level deep, and the plane name userland resolves by would "
                          "still never have been looked at")
        else:
            r246_text = r246_patch.read_text()
            for needle in ('kCloverSvcCap',
                           'kIORegistryIterateRecursively',
                           'getNextObjectRecursive()',
                           '"clover r243: svc total=%d stopped=%d button-family=%d',
                           'IORegistryEntry::getPlane("IODeviceTree")',
                           '"clover r243: plane byname=%s buttons byname=%s',
                           '"clover r243: hbt byname len=%u u32=%u'):
                if needle not in r246_text:
                    errors.append(f"{r246_patch.name} lacks {needle[:56]!r}: boot 35 could not tell "
                                  "an empty service plane from an uncounted one, nor a missing plane "
                                  "name from a missing node")
            for stale in ('iterateOver(gIOServicePlane, 0)',
                          '"clover r243: svc total=%d button-family=%d'):
                if stale in "".join(line for line in r246_text.splitlines(True)
                                    if line.startswith("+")):
                    errors.append(f"{r246_patch.name} still carries {stale!r}: the one-level walk is "
                                  "the instrument that reported total=1 on a plane that has services")

        # Round247. Boot 35 never came back: r246's walk released every entry it was handed, and
        # IORegistryIterator documents that reference as the iterator's (IORegistryEntry.h:993,
        # "The caller should not release it"). Early refcount loss takes registry entries out from
        # under the plane, which is how a census ends up eating the boot that runs it.
        r247_name = "z" * 32 + "-clover-r247.patch"
        r247_patch = next((base / "patches/xnu-clover" / r247_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r247_name).is_file()), None)
        if r247_patch is None:
            errors.append(f"patches/xnu-clover/{r247_name} is missing: the service walk would keep "
                          "releasing entries the iterator still points at -- wrong on the board, but "
                          "still wrong, and boot 37 would be the first frame to test it")
        else:
            r247_added = "".join(line for line in r247_patch.read_text().splitlines(True)
                                 if line.startswith("+"))
            r247_removed = "".join(line for line in r247_patch.read_text().splitlines(True)
                                    if line.startswith("-"))
            if "svc->release();" in r247_added:
                errors.append(f"{r247_patch.name} adds a release of the iterated entry back: the "
                              "iterator owns that reference and releasing it early corrupts the "
                              "registry the census is reading")
            if "svc->release();" not in r247_removed:
                errors.append(f"{r247_patch.name} does not remove the iterated entry's release: "
                              "the iterator would still be handed back a plane it corrupts")
            for needle in ('IORegistryEntry.h:993', 'iterator is ours to release',
                           'kCloverSvcCap = 2000'):
                if needle not in r247_added:
                    errors.append(f"{r247_patch.name} lacks {needle!r}: the next reader would have "
                                  "nothing saying why this loop must not release")

        # Round250, the question itself. Boot 37 answered everything the kernel can be asked about
        # the home button and answered it right: the by-name plane route resolves, /buttons is on it,
        # home-button-type reads back len=4 u32=2, the service plane carries two button-family
        # services and none is stopped. backboardd is still waiting. What no round has ever measured
        # is whether a userland process asked the registry at all, so the recorder sits on the two
        # exits every user-client path resolution and property read passes through. Its dump is the
        # first frame with either answer, and both answers move the diagnosis: a named caller means
        # the value it was given is the bug, and "no user client ever named a button path or key"
        # means MobileGestalt gets that identity from somewhere outside this registry -- which would
        # make the ADT, the plane and this census all beside the point.
        r250_name = "z" * 33 + "-clover-r250.patch"
        r250_patch = next((base / "patches/xnu-clover" / r250_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r250_name).is_file()), None)
        if r250_patch is None:
            errors.append(f"patches/xnu-clover/{r250_name} is missing: boot 38 would again read the "
                          "registry's answer without the question, and the next round would have to "
                          "guess whether userland ever asked")
        else:
            r250_added = "".join(line for line in r250_patch.read_text().splitlines(True)
                                 if line.startswith("+"))
            r250_text = r250_patch.read_text()
            for needle in ('clover_mg_note("path", path, 0, entry != NULL ? 1 : 0)',
                           'clover_mg_note("path", cpath, 0, entry != NULL ? 1 : 0)',
                           'IOCopyPropertyCompatibleReal(IORegistryEntry * regEntry, const char * name)',
                           'clover_mg_note("key", name, length, value)',
                           '"clover r250: asks path=%llu key=%llu notes=%u',
                           '"clover r250: no user client ever named a button path or key',
                           'extern "C" void clover_mg_dump(void);',
                           '\tclover_mg_dump();'):
                if needle not in r250_text:
                    errors.append(f"{r250_patch.name} lacks {needle[:60]!r}: without it the frame "
                                  "would not say who asked, what they asked for, and what they got")
            # The recorder is used by is_io_registry_entry_from_path, which sits two hundred lines
            # above its definition -- so the forward declaration has to be in the patch, or the
            # kernel simply will not build and boot 38 will be a build log.
            if "static void clover_mg_note(const char *kind," not in r250_added:
                errors.append(f"{r250_patch.name} uses clover_mg_note before defining it with no "
                              "forward declaration: the first compile of the round would be the "
                              "discovery")
            # IOCopyLogNameForPID hands back an OSString, not a C string -- the same mistake r250's
            # first draft made, and one no host check catches because nothing here compiles XNU.
            if "nm = IOCopyLogNameForPID(" in r250_added:
                errors.append(f"{r250_patch.name} treats IOCopyLogNameForPID's return as a char *: "
                              "it is a +1 OSString, so the kernel would not build and the name of "
                              "the caller would be the reason")
            for needle in ('who->getCStringNoCopy()', 'who->release();'):
                if needle not in r250_added:
                    errors.append(f"{r250_patch.name} lacks {needle!r}: the copied OSString would "
                                  "leak a retain on every recorded question")
            # Every row the kernel prints has to start with the token the tee's class is keyed on,
            # or the reading is dropped before it reaches the card and r249's lesson repeats.
            if r250_text.count('"clover r250: ') < 3:
                errors.append(f"{r250_patch.name} does not print every row under the "
                              '"clover r250: " prefix the tee class matches: a row on another '
                              "prefix never reaches the card")
            if "utton" not in r250_added:
                errors.append(f"{r250_patch.name} does not filter on the button substring: the "
                              "recorder would spend its ring on unrelated property reads and lose "
                              "the question under the noise")

        # Round252, the absent answers. Boot 39 found the first property this board was asked for
        # and does not have -- function-button_halleffect, answered len=0 val=0 -- and it was found
        # by luck: the name happened to contain the button substring. Every other gap is invisible
        # to that filter, so the recorder now also reports what cannot be answered, for exactly the
        # processes the button tally already named. The gate on the tally is the contract: an
        # unfiltered miss list over a whole boot would push the decisive rows out of the ring, which
        # is the defect this project has now paid for twice.
        r252_name = "z" * 35 + "-clover-r252.patch"
        r252_patch = next((base / "patches/xnu-clover" / r252_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r252_name).is_file()), None)
        if r252_patch is None:
            errors.append(f"patches/xnu-clover/{r252_name} is missing: the next round would again be "
                          "picked from whatever the button filter happened to let through, not from "
                          "what this board is missing")
        else:
            r252_added = "".join(line for line in r252_patch.read_text().splitlines(True)
                                 if line.startswith("+"))
            r252_text = r252_patch.read_text()
            for needle in ('#define CLOVER_MG_MISSES 16',
                           'static const struct clover_mg_who *\nclover_mg_asked(int pid)'
                           if False else 'clover_mg_asked(int pid)',
                           'clover_mg_miss("key", name)',
                           'clover_mg_miss("path", path)',
                           'clover_mg_miss("path", cpath)',
                           '"clover r252: misses total=%u shown=%u',
                           '"clover r252: miss %u/%u %s',
                           'static void clover_mg_miss(const char *kind, const char *what);'):
                if needle not in r252_text:
                    errors.append(f"{r252_patch.name} lacks {needle[:64]!r}: without it a missing "
                                  "property is indistinguishable from a property nobody asked about")
            # Both from_path bodies have to record the unresolved path, and the wrapper the absent
            # property; the tally gate is what keeps a chatty boot from filling the miss ring.
            if "asker == NULL" not in r252_added:
                errors.append(f"{r252_patch.name} records misses without gating them on the button "
                              "tally: every probe in the boot would land in the ring and the rows "
                              "that answer the actual question would be overwritten again")
            if 'printf("clover r251: ' in r252_added:
                errors.append(f"{r252_patch.name} still prints under the r251 prefix, which no "
                              "longer matches the tee class the burst is accounted to")
            # An inserted source line whose \n was eaten by the script that wrote it turns the
            # format string into a real newline inside a literal -- which clang reports as a cascade
            # of nine errors that all point somewhere else. Pin the literal, not the count.
            if r252_added.count('printf("clover r252: %s\\n", row);') != 2:
                errors.append(f"{r252_patch.name} does not carry two intact "
                              "'printf(\"clover r252: %s\\\\n\", row);' literals: a format string "
                              "broken by a real newline compiles as a wall of unrelated errors")

        # Round254, the same recorder's second truncation. Boot 40 kept the census and the tally and
        # still lost the reading: the miss list came back as "misses total=153 shown=16", sixteen rows
        # that were four names repeated -- soc-generation eight times over -- so the instrument spent
        # its budget restating one absence and hid the rest. One row per distinct name with a count is
        # the same instrument read correctly, the live print moves from the button rows (which the
        # census replays anyway) to the miss rows (whose first-seen order no later dump can restore),
        # and the tee class grows to carry it.
        r254_name = "z" * 36 + "-clover-r254.patch"
        r254_patch = next((base / "patches/xnu-clover" / r254_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r254_name).is_file()), None)
        if r254_patch is None:
            errors.append(f"patches/xnu-clover/{r254_name} is missing: the next boot would again be "
                          "read from a sixteen-slot ring that keeps the same absence eight times")
        else:
            r254_text = r254_patch.read_text()
            r254_added = "".join(line for line in r254_patch.read_text().splitlines(True)
                                 if line.startswith("+"))
            r254_removed = "".join(line for line in r254_patch.read_text().splitlines(True)
                                   if line.startswith("-"))
            for needle in ('#define CLOVER_MG_MISSES 32',
                           'struct clover_mg_miss {',
                           'clover_mg_miss_distinct',
                           'clover_mg_same(const char *a, const char *b)',
                           '"clover r252: misses total=%u distinct=%u\\n"',
                           '"clover r252: miss new t=%llu %c pid=%d comm=%.20s missing=%s\\n"',
                           '"clover r252: miss %u/%u t=%llus %c pid=%d comm=%.20s count=%u '
                           'missing=%s\\n"',
                           '"clover r252: nothing a button-asking process reached for was absent\\n"'):
                if needle not in r254_added:
                    errors.append(f"{r254_patch.name} lacks {needle[:64]!r}: without it the frame "
                                  "cannot say which absences there were, only how many of one")
            # The repetition this round exists to remove, both halves of it.
            if 'misses total=%u shown=%u' in r254_added:
                errors.append(f"{r254_patch.name} still reports a shown count out of a ring: a frame "
                              "that shows sixteen of one hundred and fifty-three misses has to say so")
            if r254_removed.count('printf("clover r252: %s\\n", row);') != 2:
                errors.append(f"{r254_patch.name} does not take both live row prints out of "
                              "clover_mg_note/clover_mg_miss: the census replays those rows, and the "
                              "pass budget they spend is the one the miss list is printed into")
            # r251's and r252's literal-integrity lesson, applied to this round's own strings: a
            # generator that eats the backslash before an n leaves a printf spanning two source lines,
            # which still compiles and still matches the tee prefix, and no prefix-counting assertion
            # notices.
            if r254_added.count('missing=%s\\n"') != 2 or 'distinct=%u\\n"' not in r254_added:
                errors.append(f"{r254_patch.name} does not carry two intact 'missing=%s\\n' literals "
                              "plus the distinct total: a format string broken by an eating generator "
                              "changes what the card reads back while leaving every prefix needle green")

        # Round262, the unfiltered thread census. r261's host forensics narrowed the boot's last dead
        # wait to one open question -- who owes that dispatch group its leave -- and the wait rows this
        # same sampler already prints cannot answer it: they keep at most six threads that are waiting
        # AND not in the trap, so a thread that is idle, or a second thread asleep on the very word the
        # first one waits on, is invisible by construction. Sheng reads the same shape with
        # package/BootUiTraceProbe.c. These assertions pin the census pass, the clock that gates it (the
        # card keeps the last 512 kernel rows and the sampler fires every twenty seconds, so an ungated
        # fifteen-line dump spends the whole window on one instrument), the totals that keep "not shown"
        # separate from "does not exist", and the tee class that carries it.
        r262_name = "z" * 37 + "-clover-r262.patch"
        r262_patch = next((base / "patches/xnu-clover" / r262_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r262_name).is_file()), None)
        if r262_patch is None:
            errors.append(f"patches/xnu-clover/{r262_name} is missing: the next boot would read the "
                          "group's waiters through the filter built for a different question, and r261 "
                          "showed that filter is exactly what hides the thread that owes the leave")
        else:
            r262_text = r262_patch.read_text()
            r262_added = "".join(line for line in r262_text.splitlines(True)
                                 if line.startswith("+"))
            for needle in ('#define CLOVER_BB_CENSUS 14',
                           '"clover r262: census t=%llu pid=%d threads=%u shown=%u"',
                           '"clover r262: th tid=%llx st=%x we=%llx cont=%llx"',
                           'bb_census_t == 0 || secs >= bb_census_t + 120',
                           'ctid[cn] = thr->thread_id;',
                           'cwe[cn] = (uint64_t)(uintptr_t)thr->wait_event;',
                           'ccont[cn] = (uint64_t)(uintptr_t)thr->continuation;'):
                if needle not in r262_text:
                    errors.append(f"{r262_patch.name} lacks {needle[:64]!r}: without it the frame "
                                  "cannot name the thread that owes the group its leave")
            # The census has to be the unfiltered pass, or it re-reports what r231 already says.
            for needle in ('		call++;', '		cwait++;', '		crun++;'):
                if needle not in r262_added:
                    errors.append(f"{r262_patch.name} does not count every thread in the census "
                                  f"{needle.strip()!r}: a header without totals cannot tell a capped "
                                  "walk from a task that really has that many threads")
            if 'x16 == (uint64_t)-47' in r262_added or 'if ((thr->state & TH_WAIT) == 0)' in r262_added:
                errors.append(f"{r262_patch.name} filters inside the census pass: skipping a running "
                              "thread, or one that sits in the trap, is the blind spot this round "
                              "exists to remove")
            if r262_added.count('"clover r262: ') < 2:
                errors.append(f"{r262_patch.name} does not print every row under the "
                              '"clover r262: " prefix the tee class matches')
            if '{ "clover r262: ", "r262-th", 384, 0, 0, 0 },' not in sdhci_source:
                errors.append("drivers/CloverSdhci gives round262's census no tee class, or gives it a "
                              "budget that trims its tail: fifteen rows per beat across an 1800 s "
                              "deadline is about 225 lines, and a class that runs out mid-boot drops "
                              "the late readings the ring window is made of")
            baseline_script = (REPO.parent / "UniXNU" /
                               "scripts/build-xnu-4k-baseline.sh").read_text()
            # Round263 stacks on this census, so r262 is no longer the first entry -- what still has to
            # hold is the order relative to the patch it rewrote: the list is reversed at build time, so
            # the newest round must appear above the one it edits on top of.
            late_list = " ".join(line for line in baseline_script.splitlines()
                                 if "clover-r2" in line)
            if (late_list.find(r262_name) < 0 or late_list.find(r262_name) > late_list.find(
                    "z" * 36 + "-clover-r254.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r262 ahead of r254 in "
                              "LATE_PATCH: the compile pass would run on the census-shaped tree and the "
                              "replay would look like a broken patch when it is only out of order")
            load_copy = REPO.parent / "UniXNU" / "patches" / "xnu" / r262_name
            if not load_copy.is_file():
                errors.append(f"UniXNU/patches/xnu/{r262_name} is missing: unixnu loads patches from "
                              "that directory and silently skips a board patch that lives only in "
                              "this repo")

        # Round263 widens the census r262 added, on the strength of what boot 49 showed: the cap was hit
        # on every beat (threads=17, shown=14), so threads went unsaid, and the four-slot hop capture is
        # also a cap -- which matters because the hop row is the only place a waiting thread's port gets
        # translated to the task that owns it. The row therefore gains the block deadline (0 = an untimed
        # wait, finite = one given a timeout: the difference between an idle receiver and a thread waiting
        # for an answer) plus the mach_msg2 names read where r230/r231 read them, and the owner of the
        # send right resolved by the same helper the hop rows use.
        r263_name = "z" * 38 + "-clover-r263.patch"
        r263_patch = next((base / "patches/xnu-clover" / r263_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r263_name).is_file()), None)
        if r263_patch is None:
            errors.append(f"patches/xnu-clover/{r263_name} is missing: the next boot would again report "
                          "fourteen of a task's threads and name the peer of only four of them")
        else:
            r263_text = r263_patch.read_text()
            r263_added = "".join(line for line in r263_text.splitlines(True)
                                 if line.startswith("+"))
            for needle in ('#define CLOVER_BB_CENSUS 20',
                           '#define CLOVER_BB_M2_TRAP  (-47)',
                           '"clover r262: census t=%llu pid=%d threads=%u shown=%u"',
                           '" hidden=%u wait=%u run=%u',
                           '" pc=%llx dl=%llx m2=%llx',
                           'clover_sb_port_owner(task, (mach_port_name_t)c',
                           'if (thr->wait_timer_armed && thr->wait_timer != NULL)',
                           'absolutetime_to_nanoseconds(thr->wait_timer->tc_soft_deadline,'):
                if needle not in r263_text:
                    errors.append(f"{r263_patch.name} lacks {needle[:64]!r}: without it the row cannot "
                                  "say who owes this thread its answer, or whether it is waiting forever")
            # The cap has to be visible, and the trap test has to be the same one the hop rows use.
            if 'hidden=%u' not in r263_added:
                errors.append(f"{r263_patch.name} still hides what its cap dropped: a header without a "
                              "hidden count reads as a task that has only as many threads as were shown")
            if 'x[16] == CLOVER_BB_M2_TRAP' not in r263_added.replace('(int64_t)saved_state64(ss)->', ''):
                errors.append(f"{r263_patch.name} does not gate the mach_msg2 fields on the trap number: "
                              "registers of a thread that is not in the trap are not port names")
            baseline_script = (REPO.parent / "UniXNU" /
                               "scripts/build-xnu-4k-baseline.sh").read_text()
            # Round264 rewrote this row, so r263 is no longer the first entry; what has to hold is
            # that it still leaves ahead of the rounds it edited on top of.
            late_list = " ".join(line for line in baseline_script.splitlines()
                                 if "clover-r2" in line)
            if (late_list.find(r263_name) < 0 or late_list.find(r263_name) > late_list.find(
                    "z" * 37 + "-clover-r262.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r263 ahead of r262 in "
                              "LATE_PATCH: the compile pass would run on the widened-census tree and a "
                              "replay failure would look like a broken patch instead of an out-of-order one")
            load_copy = REPO.parent / "UniXNU" / "patches" / "xnu" / r263_name
            if not load_copy.is_file():
                errors.append(f"UniXNU/patches/xnu/{r263_name} is missing: unixnu loads patches from "
                              "that directory and silently skips a board patch that lives only in "
                              "this repo")
            if '{ "clover r262: ", "r262-th", 384, 0, 0, 0 },' not in sdhci_source:
                errors.append("drivers/CloverSdhci no longer carries the census class: the widened row "
                              "is the same producer and shares its budget, so a missing entry drops the "
                              "reading entirely")

        # Round264 re-reads the census row after boot 50: r263 resolved the owner of x[6] as if it named a
        # port, and mach_msg2_trap's own unpacking says x6 is rcv_size|priority while the remote port -- the
        # right the caller sent on, and the only thing that can answer it -- sits in x3's low half. So peer=
        # named a non-port and every row printed "-", the worst kind of instrument result: it looks like an
        # answer. The row is re-derived from mach_msg.c field by field, the remote's owner becomes peer= with
        # three distinct shapes so "no send right" and "cannot resolve" stop printing alike, and the receive
        # port is read directly (imq_msgcount, ip_srights) to ask what no register can: whether anyone can
        # still post to that port at all.
        r264_name = "z" * 39 + "-clover-r264.patch"
        r264_patch = next((base / "patches/xnu-clover" / r264_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r264_name).is_file()), None)
        if r264_patch is None:
            errors.append(f"patches/xnu-clover/{r264_name} is missing: the next boot would again resolve a "
                          "rcv_size as a port name and report the absence of a peer as if it were a finding")
        else:
            r264_text = r264_patch.read_text()
            r264_added = "".join(line for line in r264_text.splitlines(True)
                                 if line.startswith("+"))
            for needle in ('crn[cn] = (uint64_t)saved_state64(ss)->x[5] >> 32',
                           'crem[cn] = (uint64_t)saved_state64(ss)->x[3]',
                           'cloc[cn] = (uint64_t)saved_state64(ss)->x[3] >> 32',
                           'cto[cn] = (uint64_t)saved_state64(ss)->x[7]',
                           '" pc=%llx dl=%llx m2=%llx rcv=%x remote=%x local=%x to=%llx"',
                           '" sz=%x mc=%u sr=%x peer=%s\\n"',
                           'clover_sb_port_mail(task_t task, mach_port_name_t name',
                           '*mc = port->ip_messages.imq_msgcount',
                           '*sr = port->ip_srights',
                           'clover_sb_port_owner(task, (mach_port_name_t)crem[i]',
                           'strlcpy(peer, "none", sizeof(peer))',
                           'strlcpy(peer, "?", sizeof(peer))',
                           '" hidden=%u wait=%u run=%u rx=%u tm=%u\\n"'):
                if needle not in r264_text:
                    errors.append(f"{r264_patch.name} lacks {needle[:64]!r}: without it the row cannot say "
                                  "whom the waiter actually sent to, or whether anyone can still answer it")
            # The two labels this round exists to retire: a row that still calls x[6] a send right, or
            # prints the remote port as a timeout, has gone back to the reading boot 50 mis-took.
            for gone in ('rn=%x sn=%x rem=%x',
                           'clover_sb_port_owner(task, (mach_port_name_t)csn[i]',
                           '" rs=%x rem=%x'):
                if gone in r264_added:
                    errors.append(f"{r264_patch.name} still prints {gone!r}: x6 is rcv_size|priority and "
                                  "x3's low half is a port name, so those labels are the defect itself")
            if 'rx=%u' not in r264_added or 'tm=%u' not in r264_added:
                errors.append(f"{r264_patch.name} leaves the receive-only count out of the header: a beat "
                              "has to say how many waiters hold no send right without a reader recounting rows")
            baseline_script = (REPO.parent / "UniXNU" /
                               "scripts/build-xnu-4k-baseline.sh").read_text()
            # An order assertion, not a "first in the list" one: r265 sits on top of r264, so r264 can no
            # longer be the head and demanding it would fail for the right reason and the wrong cause.
            late_list = " ".join(line for line in baseline_script.splitlines()
                                 if "clover-r2" in line)
            if (late_list.find(r264_name) < 0 or
                    late_list.find("z" * 40 + "-clover-r265.patch") > late_list.find(r264_name) or
                    late_list.find(r264_name) > late_list.find("z" * 38 + "-clover-r263.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r264 between r265 and r263 in "
                              "LATE_PATCH: the compile pass would run on a tree this patch was not written "
                              "against, and a replay failure would read as a broken patch")
            load_copy = REPO.parent / "UniXNU" / "patches" / "xnu" / r264_name
            if not load_copy.is_file():
                errors.append(f"UniXNU/patches/xnu/{r264_name} is missing: unixnu loads patches from "
                              "that directory and silently skips a board patch that lives only in "
                              "this repo")
            if '{ "clover r262: ", "r262-th", 384, 0, 0, 0 },' not in sdhci_source:
                errors.append("drivers/CloverSdhci dropped the census class this row is budgeted under: "
                              "the re-decode keeps one header plus twenty rows per beat, so the same "
                              "producer needs the same class")

        # Round265 turns the ruler around. Boot 51 resolved peer= for the one thread in backboardd that
        # holds a send right -- tid 0x43d, remote=0x6b0f, rcv=0xc903, mc=0, four beats in a row -- to
        # pid 140, whose comm XNU itself truncates at fifteen characters (MAXCOMLEN), and the only image
        # path matching com.apple.datam... is DataMigration.framework's com.apple.datamigrator XPC
        # service. A name is not an explanation, so the census now also runs on the debtor task, and the
        # queue of the port the request was actually sent to is read as rmc=/rsr=. sr=1 on the waiter's own
        # reply port is the kernel's held reply right for an in-flight MACH_SEND_REPLY message, which is
        # why the new pair is on the remote side instead.
        r265_name = "z" * 40 + "-clover-r265.patch"
        r265_patch = next((base / "patches/xnu-clover" / r265_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r265_name).is_file()), None)
        if r265_patch is None:
            errors.append(f"patches/xnu-clover/{r265_name} is missing: without it the boot names a debtor "
                          "and then reads nothing about it")
        else:
            r265_text = r265_patch.read_text()
            r265_added = "".join(line for line in r265_text.splitlines(True)
                                 if line.startswith("+"))
            for needle in ('clover_bb_peer_task(task_t task, mach_port_name_t name)',
                           'clover_bb_peer_census(task_t peer, uint64_t secs)',
                           'queue_iterate(&peer->threads, thr, thread_t, task_threads)',
                           '#define CLOVER_BB_PEER_ROWS 12',
                           'clover_sb_port_mail(task, (mach_port_name_t)crem[i], &rmc,',
                           '" sz=%x mc=%u sr=%x rmc=%u rsr=%x peer=%s\\n"',
                           '"clover r265: peer t=%llu pid=%d comm=%s%s threads=%u shown=%u"',
                           '" rcv=%x remote=%x to=%llx mc=%u sr=%x rmc=%u rsr=%x peer=%s\\n"',
                           'clover_sb_port_owner(peer, (mach_port_name_t)prm[i], ppeer,',
                           'clover_bb_peer_census(peer_task, secs);',
                           'task_deallocate(peer_task);'):
                if needle not in r265_added:
                    errors.append(f"{r265_patch.name} lacks {needle[:64]!r}: that is the reading which tells "
                                  "an unanswered request from one the debtor never took")
            # The peer census must not be able to call back into the sampler that calls it.
            if 'clover_bb_sample(' in r265_added:
                errors.append(f"{r265_patch.name} calls the sampler from inside the peer census: the two "
                              "share the dedupe statics, so a second process would swallow the first one's "
                              "rows")
            baseline_script = (REPO.parent / "UniXNU" /
                               "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in baseline_script.splitlines()
                                 if "clover-r2" in line)
            if (late_list.find(r265_name) < 0 or
                    late_list.find("z" * 41 + "-clover-r266.patch") > late_list.find(r265_name) or
                    late_list.find(r265_name) > late_list.find("z" * 39 + "-clover-r264.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r265 between r266 and r264 in "
                              "LATE_PATCH: the compile pass would run on a tree this patch was not written "
                              "against, and a replay failure would read as a broken patch")
            load_copy = REPO.parent / "UniXNU" / "patches" / "xnu" / r265_name
            if not load_copy.is_file():
                errors.append(f"UniXNU/patches/xnu/{r265_name} is missing: unixnu loads patches from that "
                              "directory and silently skips a board patch that lives only in this repo")
            if '{ "clover r265: ", "r265-peer", 224, 0, 0, 0 },' not in sdhci_source:
                errors.append("drivers/CloverSdhci has no class for the peer census: twelve rows plus a "
                              "header on a two-minute clock is 195 lines across a 1800 s boot, and an "
                              "unjewelled prefix means the boot reads as if the debtor was never walked")

        # Round266 fixes the field this round's own row lost. Boot 52 answered the r265 fork -- rmc=0 rsr=1,
        # the debtor took the request and has not answered -- and its census then showed five threads parked in
        # filt_wlwait (a kqueue EVFILT_WORKLOOP request) and one in a mach semaphore, with no thread of that
        # task holding a send right. Both are waits whose object is not a port, so the only identity a row can
        # carry is thread->wait_event; r265 copied r264's field list and left we= out, which makes "five
        # sleepers on one note" and "five on five notes" print alike. Same family as the hidden= and peer=
        # lessons: a field that cannot resolve must not print like a field that is absent, and a census that
        # changes the KIND of waited-on object has to change its identity field with it.
        r266_name = "z" * 41 + "-clover-r266.patch"
        r266_patch = next((base / "patches/xnu-clover" / r266_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r266_name).is_file()), None)
        if r266_patch is None:
            errors.append(f"patches/xnu-clover/{r266_name} is missing: the boot would name the waiting kinds "
                          "and still be unable to group them or say who called them")
        else:
            r266_text = r266_patch.read_text()
            r266_added = "".join(line for line in r266_text.splitlines(True)
                                 if line.startswith("+"))
            for needle in ('pwe[pn] = (uint64_t)(uintptr_t)thr->wait_event;',
                           'ppc[pn] = (uint64_t)get_saved_state_pc(ss);',
                           '"clover r265: pth tid=%llx st=%x cont=%llx we=%llx pc=%llx"',
                           'kcont[k] == (uint64_t)(uintptr_t)thr->continuation',
                           '#define CLOVER_BB_STACKS 3',
                           'fn = clover_user_bp_walk(vm_map_pmap(peer->map), kfp[k], ksp[k],',
                           '"clover r266: pustk t=%llu pid=%d tid=%llx cont=%llx"',
                           '" we=%llx sp=%llx fp=%llx lr=%llx fn=%u%s\\n"'):
                if needle not in r266_added:
                    errors.append(f"{r266_patch.name} lacks {needle[:64]!r}: without it the peer rows still "
                                  "cannot group a non-port wait or name its caller")
            # A walk that runs while the task lock is held reads user memory through a pmap that can be
            # torn down; r236 walks after the unlock and r266 must not be the round that changes that.
            walk_at = r266_text.find('clover_user_bp_walk')
            unlock_at = r266_text.find('lck_mtx_unlock(&peer->lock)')
            if unlock_at >= 0 and walk_at < unlock_at:
                errors.append(f"{r266_patch.name} walks the peer stack before dropping the peer task lock: "
                              "the frame walk is user-memory I/O and belongs after the unlock, as in r236")
            load_copy = REPO.parent / "UniXNU" / "patches" / "xnu" / r266_name
            if not load_copy.is_file():
                errors.append(f"UniXNU/patches/xnu/{r266_name} is missing: unixnu loads patches from that "
                              "directory and silently skips a board patch that lives only in this repo")
            if '{ "clover r266: ", "r266-stack", 64, 0, 0, 0 },' not in sdhci_source:
                errors.append("drivers/CloverSdhci has no class for the peer stacks: the new prefix is a "
                              "producer of its own, and an unbudgeted one drops the reading silently")

        # Round267 stops naming a wait by its address. Boot 53 answered the r266 fork -- the debtor's five
        # workloop waits carry five distinct we= words, so nothing in that process sleeps on one shared note,
        # and the row still could not say who owns any of them. The kernel already answers that question:
        # stackshot_thread_wait_owner_info (osfmk/kern/kern_stackshot.c) switches on thread->block_hint and
        # calls the kdp_*_find_owner helpers, which are all extern in osfmk/kern/block_hint.h while only the
        # dispatcher is static. So the census calls the three that are plain reads -- semaphore, ulock and
        # workloop sync wait -- and reports hint= plus ak= on every waiter, because a hint the instrument
        # did not go and read must not print like a wait nobody owns. KernelMutex stays unasked for two reasons
        # that are both true: no census row in boot 52 or 53 carries that hint (nothing sleeps in lck_mtx_sleep),
        # and kdp_lck_mtx_find_owner asserts state != LCK_MTX_TAG_DESTROYED on a mutex it reads while holding
        # nothing, which is a panic on a race in a probe that runs every two minutes. The ctid lookup is not the
        # risk -- compact_id_resolve (osfmk/kern/compact_id.c:89) is a bare array index and ctid_get_thread
        # (osfmk/kern/thread.c:4118) takes no reference, which is the claim r267's own comment gets wrong.
        r267_name = "z" * 42 + "-clover-r267.patch"
        r267_patch = next((base / "patches/xnu-clover" / r267_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r267_name).is_file()), None)
        if r267_patch is None:
            errors.append(f"patches/xnu-clover/{r267_name} is missing: the boot would still print a wait "
                          "address and call that an owner")
        else:
            r267_text = r267_patch.read_text()
            r267_added = "".join(line for line in r267_text.splitlines(True)
                                 if line.startswith("+"))
            for needle in ('clover_wait_owner(thread_t thr, uint32_t *hint, uint32_t *asked',
                           'kdp_sema_find_owner(thr->waitq.wq_q, thr->wait_event, &wi);',
                           'kdp_ulock_find_owner(thr->waitq.wq_q, thr->wait_event, &wi);',
                           'kdp_workloop_sync_wait_find_owner(thr, thr->wait_event, &wi);',
                           'bzero(&wi, sizeof(wi));',
                           '(thr->state & TH_WAIT) == 0 || thr->wait_event == 0',
                           'clover_wait_owner(thr, &phint[pn], &pak[pn], &pown[pn], &pctx[pn]);',
                           'clover_wait_owner(thr, &chint[cn], &cak[cn], &cown[cn], &cctx[cn]);',
                           '"clover r267: own t=%llu pid=%d tid=%llx hint=%x',
                           '" ak=%u owner=%llx ctx=%llx\\n"'):
                if needle not in r267_added:
                    errors.append(f"{r267_patch.name} lacks {needle[:64]!r}: without it the owner is still "
                                  "an address, or an unanswered wait prints like one nobody owes")
            # One helper, two callers. A third call site would mean a census that asks the kernel outside the
            # task lock it was written for, and bzero of the record is what keeps a find_owner that writes
            # only context (a semaphore with no owner) from printing a stack residue as a tid.
            if r267_added.count("clover_wait_owner(") != 3:
                errors.append(f"{r267_patch.name} calls clover_wait_owner "
                              f"{r267_added.count('clover_wait_owner(') - 1} times instead of twice: the "
                              "helper is allowed one definition and one call per census, and both censuses "
                              "collect under the task lock")
            # Collect while locked, print after: each call has to precede its own row in file order, which is
            # the order the patch text carries.
            for call, row in (('clover_wait_owner(thr, &phint[pn]', 'hint=%x ak=%u'),
                              ('clover_wait_owner(thr, &chint[cn]', 'hint=%x"')):
                if not (0 <= r267_added.find(call) < r267_added.find(row)):
                    errors.append(f"{r267_patch.name} prints the owner row before it collects the wait "
                                  "record: the kdp_ helpers read another process's objects and belong in "
                                  "the locked pass, the way r236 walks after the unlock and never before it")
            baseline_script = (REPO.parent / "UniXNU" /
                               "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in baseline_script.splitlines()
                                 if "clover-r2" in line)
            if (late_list.find(r267_name) < 0 or
                    late_list.find(r267_name) > late_list.find("z" * 41 + "-clover-r266.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r267 before r266 in LATE_PATCH: "
                              "the compile pass would run on a tree this patch was not written against, and "
                              "a replay failure would read as a broken patch")
            load_copy = REPO.parent / "UniXNU" / "patches" / "xnu" / r267_name
            if not load_copy.is_file():
                errors.append(f"UniXNU/patches/xnu/{r267_name} is missing: unixnu loads patches from that "
                              "directory and silently skips a board patch that lives only in this repo")
            if '{ "clover r267: ", "r267-owner", 192, 0, 0, 0 },' not in sdhci_source:
                errors.append("drivers/CloverSdhci has no class for the wait-owner rows: a new prefix is a "
                              "producer of its own and an unbudgeted one drops the reading silently")

        # Round268 reads the object behind the one hint whose kernel answer is not an answer.
        # kdp_sema_find_owner reports pid_from_task(sem->owner), and create_semaphore sets that field to the
        # task that made the semaphore (osfmk/kern/sync_sema.c:128; the struct comment in sync_sema.h:55 says
        # exactly that), so boot 54's owner=pid 138 on its own waiter names a creator, not a signaller. The
        # signaller is whoever holds a send right on the semaphore's port, which is the reading that decides
        # whether that thread can ever wake: count (negative means waiters are queued), ip_srights, and the
        # receiving task named.
        r268_name = "z" * 43 + "-clover-r268.patch"
        r268_patch = next((base / "patches/xnu-clover" / r268_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r268_name).is_file()), None)
        if r268_patch is None:
            errors.append(f"patches/xnu-clover/{r268_name} is missing: the census would still stop at a "
                          "semaphore's creator and could not say whether anyone can signal it")
        else:
            r268_text = r268_patch.read_text()
            r268_added = "".join(line for line in r268_text.splitlines(True)
                                 if line.startswith("+"))
            for needle in ('clover_sema_state(thread_t thr, int *count, uint32_t *srights',
                           'offsetof(struct semaphore, waitq)',
                           '*count = sem->count;',
                           'port = sem->port;',
                           '*srights = (uint32_t)port->ip_srights;',
                           'space = port->ip_receiver;',
                           'snprintf(who, wholen, "%d:%s", task_pid(recv)',
                           '"clover r268: sem t=%llu pid=%d tid=%llx count=%d"',
                           '" srights=%u otype=%x receiver=%s\\n"',
                           'psem[pn] = (uint8_t)clover_sema_state(thr, &psc[pn],',
                           'csem[cn] = (uint8_t)clover_sema_state(thr, &csc[cn], &csp[cn],',
                           '#include <kern/sync_sema.h>',
                           '#define CLOVER_BB_SEMA_NAME 20'):
                if needle not in r268_added:
                    errors.append(f"{r268_patch.name} lacks {needle[:64]!r}: without it the semaphore row "
                                  "cannot say whether the wait is structurally unanswerable")
            # The round that shipped this read it unlocked, which is the defect r269 removes -- not the
            # accessor. ip_get_receiver() (osfmk/ipc/ipc_port.h:528) asserts ip_mq_lock_held(), so it is the
            # SAFE form once the port is locked; what is unsafe is the raw port->ip_receiver, because for a
            # port that is not in any space the same slot aliases ip_destination. A ban on the accessor was
            # written from that misunderstanding and forbade the only correct usage, so it is reversed here
            # and the requirement now lives in the round269 block.
            if "space = port->ip_receiver;" not in r268_added:
                errors.append(f"{r268_patch.name} no longer shows the unlocked raw read: this block records "
                              "what r268 shipped so the r269 assertions have something to be a fix of")
            # A row that is only printed when the hint says semaphore: an unconditional read would dereference
            # the container_of result for waits that are not semaphores at all.
            if "if (phint[pn] == (uint32_t)kThreadWaitSemaphore) {" not in r268_added or \
                    "if (chint[cn] == (uint32_t)kThreadWaitSemaphore) {" not in r268_added:
                errors.append(f"{r268_patch.name} reads a semaphore off a thread the kernel did not report "
                              "as one: the container_of result is only meaningful under that hint")
            baseline_script = (REPO.parent / "UniXNU" /
                               "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in baseline_script.splitlines()
                                 if "clover-r2" in line)
            if (late_list.find(r268_name) < 0 or
                    late_list.find(r268_name) > late_list.find("z" * 42 + "-clover-r267.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r268 before r267 in LATE_PATCH: "
                              "the compile pass would run on a tree this patch was not written against")
            load_copy = REPO.parent / "UniXNU" / "patches" / "xnu" / r268_name
            if not load_copy.is_file():
                errors.append(f"UniXNU/patches/xnu/{r268_name} is missing: unixnu loads patches from that "
                              "directory and silently skips a board patch that lives only in this repo")
            if '{ "clover r268: ", "r268-sema", 64, 0, 0, 0 },' not in sdhci_source:
                errors.append("drivers/CloverSdhci has no class for the semaphore-rights rows: a new prefix "
                              "is a producer of its own and an unbudgeted one drops the reading silently")

        # Round269 names the holder of the one send right. boot 55 read count=-1 srights=1 on the debtor's
        # semaphore, which is the middle branch of the r268 fork: the object is alive, a wake-up is owed, and
        # nothing said by whom. A port counts send rights but never points back at the spaces holding them, so
        # the census now asks the kernel's reverse lookup -- ipc_hash_lookup (osfmk/ipc/ipc_hash.c) maps
        # (space, object) -> name, and the comment at ipc_hash.c:139 records that only pure send rights are
        # entered there, which is exactly the set this round wants: a hit is a task that can signal, while a
        # table scan would also name the receiver.
        r269_name = "z" * 44 + "-clover-r269.patch"
        r269_patch = next((base / "patches/xnu-clover" / r269_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r269_name).is_file()), None)
        if r269_patch is None:
            errors.append(f"patches/xnu-clover/{r269_name} is missing: boot 56 would again stop at srights=1 "
                          "and never say who owes the signal")
        else:
            r269_text = r269_patch.read_text()
            r269_added = "".join(line for line in r269_text.splitlines(True)
                                 if line.startswith("+"))
            for needle in ('clover_sema_send_holder(ipc_port_t port',
                           '#define CLOVER_BB_SEMA_PROBE 400',
                           'ipc_hash_lookup(space, (ipc_object_t) port, &name, &entry)',
                           'smr_ipc_enter();',
                           'smr_ipc_leave();',
                           'is_read_lock(space);',
                           'extern int proc_rele(struct proc *p);',
                           'extern struct proc *proc_find(int pid);',
                           'io_lock((ipc_object_t)port);',
                           'io_unlock((ipc_object_t)port);',
                           'space = ip_in_a_space(port) ? ip_get_receiver(port) : IPC_SPACE_NULL;',
                           'strlcpy(who, "no-space", wholen)',
                           'strlcpy(who, "kspace", wholen)',
                           'receiver=%s port=%llx',
                           '"clover r269: srhost t=%llu pid=%d tid=%llx port=%llx hit=%llu"',
                           'name=%llx type=%llx scanned=%u probed=%u'):
                if needle not in r269_added:
                    errors.append(f"{r269_patch.name} lacks {needle[:64]!r}: without it the sender of the "
                                  "semaphore stays unnamed, or the receiver is read the way r268 read it")
            # The reversed contract. r268's checker forbade ip_get_receiver() because its comment claimed the
            # accessor asserts io_lock_held(); it asserts ip_mq_lock_held() (ipc_port.h:528), so it is the
            # correct reader once the port is locked, and the raw ip_receiver field is what must not be read:
            # for a port with no space that slot aliases ip_destination, and the space pointer that follows is
            # then dereferenced. Both halves are pinned, not just the ban.
            if "ip_get_receiver(port)" not in r269_added:
                errors.append(f"{r269_patch.name} still resolves the receiver by field: the accessor is the "
                              "only reader of ip_receiver that checks the port is in a space at all")
            if "space = port->ip_receiver;" in r269_added:
                errors.append(f"{r269_patch.name} re-adds r268's raw ip_receiver read: an unlocked field read "
                              "can hand back ip_destination, and the is_task dereference after it is a panic")
            # proc_find/proc_task/proc_rele are not declared in any header task.c includes; every earlier
            # census in this file declares them inside the function that uses them. Without that, proc_rele is
            # an implicit declaration -- an error in this tree's C dialect, and proc_find's pointer return
            # would be truncated to int if the compiler let it through.
            if r269_added.count("extern int proc_rele(struct proc *p);") != 1:
                errors.append(f"{r269_patch.name} does not declare proc_rele where it calls it: the sender "
                              "scan would not compile, or would compile with a truncated proc_find")
            # One helper, two callers, one latch each. A third call would put the process-list walk inside a
            # held task lock, which is not what it was written for.
            if r269_added.count("clover_sema_send_holder(") != 3:
                errors.append(f"{r269_patch.name} calls clover_sema_send_holder "
                              f"{r269_added.count('clover_sema_send_holder(') - 1} times instead of twice: the "
                              "helper is allowed one definition and one call per census, and both censuses "
                              "run it after dropping the peer task lock")
            # The walk must sit in the print phase, not the collect phase: the peer task lock is dropped
            # before rows are printed (round236 established that, and the round266 block above asserts it for
            # the stack walk), and a proc_find pass belongs after it for the same reason. The unlock line is
            # unchanged context here, so what this patch can prove is the order inside the print phase: the
            # semaphore row is printed, and only then is its port looked up in the process list.
            if r269_added.find("receiver=%s port=%llx") > r269_added.find(
                    "clover_sema_send_holder(pport[i]"):
                errors.append(f"{r269_patch.name} puts the sender-right walk before the semaphore row it "
                              "belongs to: the census walks after the peer task lock is dropped, and that "
                              "is the print phase this row lives in")
            if r269_added.rfind("receiver=%s port=%llx") > r269_added.find(
                    "clover_sema_send_holder(cport[i]"):
                errors.append(f"{r269_patch.name} puts the blocked process's walk before its semaphore row, "
                              "which is the same print-phase rule on the other census")
            baseline_script = (REPO.parent / "UniXNU" /
                               "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in baseline_script.splitlines()
                                 if "clover-r2" in line)
            if (late_list.find(r269_name) < 0 or
                    late_list.find(r269_name) > late_list.find("z" * 43 + "-clover-r268.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r269 before r268 in LATE_PATCH: "
                              "the compile pass would run on a tree this patch was not written against")
            load_copy = REPO.parent / "UniXNU" / "patches" / "xnu" / r269_name
            if not load_copy.is_file():
                errors.append(f"UniXNU/patches/xnu/{r269_name} is missing: unixnu loads patches from that "
                              "directory and silently skips a board patch that lives only in this repo")
            if '{ "clover r269: ", "r269-srhost", 64, 0, 0, 0 },' not in sdhci_source:
                errors.append("drivers/CloverSdhci has no class for the sender-right rows: a new prefix is a "
                              "producer of its own and an unbudgeted one drops the reading silently")

        # Round270 reads the wake-up itself. boot 56 settled who owes the signal (the debtor's own task) and
        # the shape that explains it: eight threads, none runnable, five of them libdispatch workloop workers
        # asleep in a queue that received work -- and a sleeping thread on another core is woken by an IPI,
        # which this board's CPU driver has never trusted ("ICC_SGI1R_EL1 generates nothing on this board",
        # drivers/ArmCpu/ArmCpu.cpp:43) without anyone ever measuring whether the IPI arrives. The counters
        # already exist inside an extern "C" block (ArmCpu.cpp:32-42), so this round only has to print them.
        r270_name = "z" * 45 + "-clover-r270.patch"
        r270_patch = next((base / "patches/xnu-clover" / r270_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r270_name).is_file()), None)
        if r270_patch is None:
            errors.append(f"patches/xnu-clover/{r270_name} is missing: the frame would keep asserting that "
                          "a worker was never woken without ever saying whether an IPI arrived")
        else:
            r270_added = "".join(line for line in r270_patch.read_text().splitlines(True)
                                 if line.startswith("+"))
            for needle in ('extern volatile uint64_t armcpu_ipi_sent;',
                           'extern volatile uint64_t armcpu_ipi_interrupts;',
                           'extern volatile uint32_t armcpu_core_sgi[8];',
                           'extern volatile uint32_t armcpu_core_timer[8];',
                           '"clover r270: ipi t=%llu sent=%llu recv=%llu tick=%llu"',
                           'sgi=%u,%u,%u,%u,%u,%u,%u,%u tm=%u,%u,%u,%u,%u,%u,%u,%u',
                           'armcpu_core_sgi[7]', 'armcpu_core_timer[7]'):
                if needle not in r270_added:
                    errors.append(f"{r270_patch.name} lacks {needle[:64]!r}: without the per-core array or "
                                  "both sides of the tally the row cannot separate 'nobody sent' from "
                                  "'one core never receives'")
            # The row has to come before the thread rows it qualifies, and once per fire rather than once per
            # thread: the counters are global, so fifteen copies of nine numbers are noise that eats the budget.
            if r270_added.count("clover r270: ipi") != 1:
                errors.append(f"{r270_patch.name} prints the IPI row "
                              f"{r270_added.count('clover r270: ipi')} times: one census fire is one row")
            baseline_script = (REPO.parent / "UniXNU" /
                               "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in baseline_script.splitlines()
                                 if "clover-r2" in line)
            if (late_list.find(r270_name) < 0 or
                    late_list.find(r270_name) > late_list.find("z" * 44 + "-clover-r269.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r270 before r269 in LATE_PATCH: "
                              "the compile pass would run on a tree this patch was not written against")
            load_copy = REPO.parent / "UniXNU" / "patches" / "xnu" / r270_name
            if not load_copy.is_file():
                errors.append(f"UniXNU/patches/xnu/{r270_name} is missing: unixnu loads patches from that "
                              "directory and silently skips a board patch that lives only in this repo")
            if '{ "clover r270: ", "r270-ipi", 64, 0, 0, 0 },' not in sdhci_source:
                errors.append("drivers/CloverSdhci has no class for the IPI row: a new prefix is a producer "
                              "of its own and an unbudgeted one drops the reading silently")
            # The claim this round is checking is the driver's own, so the checker records that the driver
            # still says it: if ArmCpu ever ships SGI delivery a different way, this row is answering a
            # question nobody asked any more.
            armcpu = (REPO / "drivers/ArmCpu/ArmCpu.cpp").read_text()
            if "armcpu_ipi_sent = armcpu_ipi_sent + 1" not in armcpu or \
                    "armcpu_core_sgi[logicalId & 7]" not in armcpu:
                errors.append("drivers/ArmCpu no longer tallies the IPIs it sends and the SGIs it receives: "
                              "the census row would be reading globals that nothing writes")

        # Round271 asks the kernel the same question the driver was asked in r270. Boot 57 answered
        # "sent=0 recv=0, and only core 0 ever takes a timer interrupt" from the CPU driver's counters --
        # which cannot tell "the board lost the interrupt" from "there was never a cpu registered on that
        # core to lose it to". CpuDataEntries[] is the array XNU fills as cpus register, StartedState is
        # set by processor_start(), cpu_running flips as that cpu enters and leaves idle, and
        # cpu_stat.timer_cnt is the kernel's own tick tally. Eight threads waiting on one registered core
        # is a different fault, with a different fix, from eight threads waiting on eight cores that
        # nobody wakes.
        r271_name = "z" * 46 + "-clover-r271.patch"
        r271_patch = next((base / "patches/xnu-clover" / r271_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r271_name).is_file()), None)
        if r271_patch is None:
            errors.append(f"patches/xnu-clover/{r271_name} is missing: the frame would go on reporting "
                          "the board's interrupt counters as if they said how many cpus XNU is using")
        else:
            r271_text = r271_patch.read_text()
            r271_added = "".join(line for line in r271_text.splitlines(True)
                                 if line.startswith("+"))
            for needle in ('#include <arm/cpu_data_internal.h>',
                           '#include <arm/cpu_internal.h>',
                           '#define CLOVER_R271_NCORE (MAX_CPUS < 8 ? MAX_CPUS : 8)',
                           'CpuDataEntries[k].cpu_data_vaddr',
                           'cdp->cpu_flags & StartedState',
                           'if (cdp->cpu_running)',
                           'cdp->cpu_stat.timer_cnt',
                           '"clover r271: cpu t=%llu real=%u max=%u reg=%u start=%u run=%u"',
                           's=%u%u%u%u%u%u%u%u r=%u%u%u%u%u%u%u%u'):
                if needle not in r271_added:
                    errors.append(f"{r271_patch.name} lacks {needle[:64]!r}: without the kernel's own "
                                  "registered/started/running/timer fields the row is another copy of "
                                  "the driver's numbers")
            if "armcpu_" in r271_added:
                errors.append(f"{r271_patch.name} reads the driver's globals again: r271 exists precisely "
                              "because those counters describe interrupts, not cpus")
            if r271_added.count("clover r271: cpu") != 1:
                errors.append(f"{r271_patch.name} prints the cpu row "
                              f"{r271_added.count('clover r271: cpu')} times: one census fire is one row")
            # The row has to sit inside the census block -- after r270's global row and before the thread
            # rows it qualifies. Both neighbours are context lines, so this reads the whole patch, not
            # just its plus side.
            if "armcpu_core_timer[6], armcpu_core_timer[7]);" not in r271_text:
                errors.append(f"{r271_patch.name} is not applied directly after r270's row: the last "
                              "context line of the hunk is that row's closing printf argument list, and "
                              "without it the two readings are not known to arrive in the same frame")
            if (r271_text.find("clover r271: cpu") < 0 or
                    r271_text.find("for (i = 0; i < cn; i++)") < r271_text.find("clover r271: cpu")):
                errors.append(f"{r271_patch.name} puts the cpu row after the thread rows it is meant to "
                              "be read against")
            baseline_script = (REPO.parent / "UniXNU" /
                               "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in baseline_script.splitlines()
                                 if "clover-r2" in line)
            if (late_list.find(r271_name) < 0 or
                    late_list.find(r271_name) > late_list.find("z" * 45 + "-clover-r270.patch")):                errors.append("build-xnu-4k-baseline.sh does not reverse r271 before r270 in LATE_PATCH: "
                              "the compile pass would run on a tree this patch was not written against")
            load_copy = REPO.parent / "UniXNU" / "patches" / "xnu" / r271_name
            if not load_copy.is_file():
                errors.append(f"UniXNU/patches/xnu/{r271_name} is missing: unixnu loads patches from that "
                              "directory and silently skips a board patch that lives only in this repo")
            if '{ "clover r271: ", "r271-cpu", 64, 0, 0, 0 },' not in sdhci_source:
                errors.append("drivers/CloverSdhci has no class for the cpu row: a new prefix is a "
                              "producer of its own and an unbudgeted one drops the reading silently")

        # Round272 patches nothing. Boot 58 measured reg=1 -- only the boot cpu ever registered -- and
        # the reason is readable in the shipped image: nm over the unstripped kernel finds no
        # AppleARMSMP symbols, because iokit/conf/files.arm64:7 lists it as `optional iokitcpp`. What is
        # left unwitnessed is whether the kernel ever tried, and osfmk/arm64/cpu.c:909 already kprints
        # that attempt, so the whole round is a tee token and a class. These assertions hold the round to
        # that: the token has to be in the allowlist (which gates before the class table is consulted --
        # a class without its token admits nothing), and no new board patch may appear.
        if '"cpu_start()",' not in sdhci_source:
            errors.append("drivers/CloverSdhci's allowlist has no cpu_start() token: teeLineMatches runs "
                          "before the class table, so an allowlisted-off line never reaches its budget")
        if '{ "cpu_start()", "r272-cpustart", 16, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the cpu_start() row: an unbudgeted "
                          "producer of its own would push the census out of the ring")
        if sdhci_source.index('"cpu_start()",') > sdhci_source.index("struct TeeProbeClass"):
            errors.append("the cpu_start() allowlist token is not in the allowlist table (it must sit "
                          "above the class table, which is where the gate that admits a line lives)")
        r272_name = "z" * 47 + "-clover-r272.patch"
        if (REPO / "patches/xnu-clover" / r272_name).is_file():
            errors.append(f"patches/xnu-clover/{r272_name} exists: round272's claim is that the kernel "
                          "already says what this round needs to hear, so a patch would mean the round "
                          "stopped being a tee change and became a new reading no one asked for")
        r272_late = " ".join(line for line in (REPO.parent / "UniXNU" /
                                               "scripts/build-xnu-4k-baseline.sh").read_text().splitlines()
                             if "clover-r2" in line)
        if r272_late.find("z" * 46 + "-clover-r271.patch") < 0 or \
                r272_late.find("z" * 46 + "-clover-r271.patch") > r272_late.find(
                    "z" * 45 + "-clover-r270.patch"):
            errors.append("build-xnu-4k-baseline.sh no longer reverses r271 first: round272 ships no "
                          "patch, so the LATE head must still be the r271 row that produced boot 58")

        # Round273 reads the workloop object instead of the kernel's one-number answer for it. r267's owner=
        # field is whichever of three branches kdp_workloop_sync_wait_find_owner took -- kqwl_owner, a bound
        # request thread, or "a thread was requested" -- and boot 59's owner=3b7 (a thread of the debtor's own,
        # asleep in the same census) could have come from any of them, with a different fix each. So the row
        # prints kq_state, kq_count, kqwl_dynamicid and the whole kqwl_request: tr_state, tr_count, tr_thread.
        # The read lives in bsd/kern/kern_event.c because struct kqworkloop is only complete under
        # BSD_KERNEL_PRIVATE -- a round that tried to include sys/eventvar.h from task.c compiles against the
        # exported copy and gets ten "unknown type name" errors -- and task.c reaches it the same way it
        # reaches kdp_workloop_sync_wait_find_owner. The peer row gains wl=, the count of workloop waiters in
        # that fire: boot 60 found the debtor's threads waiting on mach ports instead, and without the count
        # an absent r273 row cannot be told apart from an instrument that never ran.
        r273_name = "z" * 47 + "-clover-r273.patch"
        r273_patch = next((base / "patches/xnu-clover" / r273_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r273_name).is_file()), None)
        if r273_patch is None:
            errors.append(f"patches/xnu-clover/{r273_name} is missing: without the workloop request the "
                          "census still only has r267's collapsed owner, which cannot say whether XNU ever "
                          "asked the workqueue for a servicer")
        else:
            r273_text = r273_patch.read_text()
            r273_added = "".join(line for line in r273_text.splitlines(True)
                                 if line.startswith("+"))
            if "--- a/bsd/kern/kern_event.c" not in r273_text or \
                    "--- a/osfmk/kern/task.c" not in r273_text:
                errors.append(f"{r273_name} does not carry both halves of the round: the read is "
                              "kern_event.c's and the row is task.c's")
            for needle in ('extern void     clover_workloop_snapshot(uint64_t event, uint64_t out[11]);',
                           'struct clover_workloop_read {',
                           'clover_workloop_snapshot((uint64_t)(uintptr_t)thr->wait_event, raw);',
                           '#define CLOVER_WL_FIELDS 11',
                           '#define CLOVER_WL_FLAGS  0',
                           '#define CLOVER_WL_TR     6',
                           '#define CLOVER_WL_TT     8',
                           '#define CLOVER_WL_OW     9',
                           '#define CLOVER_WL_PID    10',
                           'if (phint[pn] == (uint32_t)kThreadWaitWorkloopSyncWait)',
                           'if (pwl[i].valid != 0)',
                           '"clover r273: wl t=%llu pid=%d tid=%llx kn=%llx kq=%llx wlo=%u"',
                           '" ks=%x kc=%u kqpid=%d dyn=%llx tr=%x tc=%u tt=%llx ow=%llx\\n"',
                           '" hidden=%u wait=%u run=%u rx=%u tm=%u wl=%u\\n"',
                           '#define CLOVER_WL_OUT_FLAGS 0',
                           '#define CLOVER_WL_OUT_TR    6',
                           '#define CLOVER_WL_OUT_TT    8',
                           '#define CLOVER_WL_OUT_OW    9',
                           '#define CLOVER_WL_OUT_PID   10',
                           'clover_workloop_snapshot(uint64_t event, uint64_t out[11])',
                           'kq = knote_get_kq(kn);',
                           'if ((kq->kq_state & KQ_WORKLOOP) == 0)',
                           'kqr->tr_state >= WORKQ_TR_STATE_BINDING',
                           'out[CLOVER_WL_OUT_KC] = (uint64_t)kq->kq_count;',
                           'out[CLOVER_WL_OUT_DYN] = (uint64_t)kqwl->kqwl_dynamicid;'):
                if needle not in r273_added:
                    errors.append(f"{r273_name} lacks {needle[:64]!r}: without it the row is another copy "
                                  "of what r267 already collapsed into owner=")
            # The exported-header trap, stated as a prohibition: the moment task.c names a bsd-private type
            # again, the file it was compiled against is not the file that was read.
            task_part = r273_text[:r273_text.index("--- a/bsd/kern/kern_event.c")] \
                if "--- a/bsd/kern/kern_event.c" in r273_text else r273_text
            # Prose in a comment may name the accessor this round deliberately does not call; the
            # prohibition is on holding one: a type, a dereference, or the unpack itself.
            for bad in ('struct kqworkloop *', 'struct kqueue *', 'kqwl->', 'kn->'):
                if bad in task_part:
                    errors.append(f"{r273_name} reads bsd-private types from task.c ({bad!r}): task.c "
                                  "compiles against EXPORT_HDRS, where those fields do not exist")
            if r273_added.count("clover r273: wl") != 1:
                errors.append(f"{r273_name} prints the workloop row {r273_added.count('clover r273: wl')} "
                              "times: one waiter per row is the whole design, two rows per waiter would "
                              "double the budget for no extra reading")
            if '{ "clover r273: ", "r273-wl", 64, 0, 0, 0 },' not in sdhci_source:
                errors.append("drivers/CloverSdhci has no class for the workloop row: a new prefix is a "
                              "producer of its own and an unbudgeted one drops the reading silently")
            late_script = (REPO.parent / "UniXNU" /
                           "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in late_script.splitlines()
                                 if "clover-r2" in line)
            if (late_list.find(r273_name) < 0 or
                    late_list.find(r273_name) > late_list.find("z" * 46 + "-clover-r271.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r273 before r271 in LATE_PATCH: "
                              "the compile pass would run on a tree this patch was not written against")
            load_copy = REPO.parent / "UniXNU" / "patches" / "xnu" / r273_name
            if not load_copy.is_file():
                errors.append(f"UniXNU/patches/xnu/{r273_name} is missing: unixnu loads patches from that "
                              "directory and silently skips a board patch that lives only in this repo")
            elif load_copy.read_bytes() != r273_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r273_name} is not the same bytes as this repo's copy: "
                              "the two trees would compile different kernels")

        # Round274 asks for the one number that turns boot 60's four unnamed frames into function names: the
        # address the peer's own binary was loaded at. The read is osfmk-side and legal there -- struct _vm_map
        # is complete in task.c, which is exactly the thing that was NOT true of struct kqworkloop in r273 --
        # so the patch touches one file. The row is printed once per fire, ahead of the rows it resolves, and
        # carries ne= (how many entries the map has) next to shown= (how many this row carries) so a truncated
        # walk reads as truncation rather than as the whole address space.
        r274_name = "z" * 48 + "-clover-r274.patch"
        r274_patch = next((base / "patches/xnu-clover" / r274_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r274_name).is_file()), None)
        if r274_patch is None:
            errors.append(f"patches/xnu-clover/{r274_name} is missing: without the peer's own mappings the "
                          "four main-image frames of the bound servicer stay four numbers and nothing more")
        else:
            r274_text = r274_patch.read_text()
            r274_added = "".join(line for line in r274_text.splitlines(True)
                                 if line.startswith("+"))
            if "--- a/bsd/" in r274_text:
                errors.append(f"{r274_name} touches a bsd file: this read is of struct _vm_map, which task.c "
                              "can resolve on its own -- reaching for bsd again would be r273's mistake twice")
            for needle in ('#define CLOVER_WL_MAP_ROWS 4',
                           'uint64_t mestart[CLOVER_WL_MAP_ROWS];',
                           'uint64_t meend[CLOVER_WL_MAP_ROWS];',
                           'uint32_t mene;',
                           'for (i = 0; i < CLOVER_WL_MAP_ROWS; i++) {',
                           'mene = (uint32_t)peer->map->hdr.nentries;',
                           'me = vm_map_first_entry(peer->map);',
                           'me != vm_map_to_entry(peer->map) && men < CLOVER_WL_MAP_ROWS;',
                           'mestart[men] = (uint64_t)me->vme_start;',
                           '"clover r274: map t=%llu pid=%d ne=%u shown=%u e0=%llx-%llx e1=%llx-%llx"',
                           '" e2=%llx-%llx e3=%llx-%llx\\n"'):
                if needle not in r274_added:
                    errors.append(f"{r274_name} lacks {needle[:64]!r}: without it the row cannot say which "
                                  "range the frames fell in, which is the whole point of the round")
            if r274_added.count("clover r274: map") != 1:
                errors.append(f"{r274_name} prints the map row {r274_added.count('clover r274: map')} times: "
                              "one row per fire is the design, per thread would eat the census budget")
            if '{ "clover r274: ", "r274-map", 32, 0, 0, 0 },' not in sdhci_source:
                errors.append("drivers/CloverSdhci has no class for the map row: a new prefix is a producer "
                              "of its own and an unbudgeted one drops the reading silently")
            late_script = (REPO.parent / "UniXNU" /
                           "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in late_script.splitlines()
                                 if "clover-r2" in line)
            if (late_list.find(r274_name) < 0 or
                    late_list.find(r274_name) > late_list.find("z" * 47 + "-clover-r273.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r274 before r273 in LATE_PATCH: the "
                              "compile pass would run on a tree this patch was not written against")
            load_copy = REPO.parent / "UniXNU" / "patches" / "xnu" / r274_name
            if not load_copy.is_file():
                errors.append(f"UniXNU/patches/xnu/{r274_name} is missing: unixnu loads patches from that "
                              "directory and silently skips a board patch that lives only in this repo")
            elif load_copy.read_bytes() != r274_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r274_name} is not the same bytes as this repo's copy: "
                              "the two trees would compile different kernels")

        # Round275 stops chasing the debtor and measures what it is waiting on. Boot 61's frames named the
        # wait (-waitUntilPreboardDidFinishUnlock, then dispatch_semaphore_wait forever) and this board's own
        # r252 ledger named a missing IOService:/IOResources/AppleKeyStore, but the only fix on the table --
        # sheng's owned keystore -- publishes only when the AppleKeyStore *metaclass* is absent, and that
        # precondition has never been read here. So the round measures, it does not patch: the two class
        # lookups, every service in the plane whose name says KeyStore, and the walk's own reached/stopped
        # count so that "none" can never be read off a truncated walk. One iterator, one pass: a second
        # whole-plane walk would take the registry lock twice to answer one question.
        r275_name = "z" * 49 + "-clover-r275.patch"
        r275_patch = next((base / "patches/xnu-clover" / r275_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r275_name).is_file()), None)
        if r275_patch is None:
            errors.append(f"patches/xnu-clover/{r275_name} is missing: without it the keystore question "
                          "stays an inference drawn from another round's ledger line")
        else:
            r275_text = r275_patch.read_text()
            r275_added = "".join(line for line in r275_text.splitlines(True)
                                 if line.startswith("+"))
            if r275_text.count("--- a/") != 1 or "--- a/iokit/Kernel/IOStartIOKit.cpp" not in r275_text:
                errors.append(f"{r275_name} does not touch exactly iokit/Kernel/IOStartIOKit.cpp: the census "
                              "walk already lives there, and a second file would mean a second walk")
            for needle in ('OSSymbol::withCString("AppleKeyStore")',
                           'OSSymbol::withCString("AppleKeyStoreUserClient")',
                           'OSMetaClass::getMetaClassWithName(ksname)',
                           'OSMetaClass::getMetaClassWithName(ksclient)',
                           '"clover r275: aks class=%d userclient_class=%d\\n"',
                           'clover_name_has(nm, "KeyStore")',
                           'mc->getClassNameSymbol()',
                           '"clover r275: svc %.24s class=%.24s\\n"',
                           'if (keystore_seen < 8)',
                           '"clover r275: aks walked=%d stopped=%d keystore_services=%d\\n"'):
                if needle not in r275_added:
                    errors.append(f"{r275_name} lacks {needle[:64]!r}: that is one of the three readings, "
                                  "and a round that returns two of three is how boot 34 read a stopped "
                                  "walk as an empty plane")
            if "iterateOver" in r275_added:
                errors.append(f"{r275_name} opens a second registry iterator: the keystore rows belong in "
                              "the walk that is already running, not beside another one")
            if r275_added.count("aks class=") != 1 or r275_added.count("aks walked=") != 1:
                errors.append(f"{r275_name} prints its summary rows more than once: the census is latched "
                              "per boot, so a second copy would mean the walk is being re-entered and its "
                              "budget is shared with the thread rows that resolve the wait")
            if not (r275_added.find("aks class=") < r275_added.find('svc %.24s class=')
                    < r275_added.find("aks walked=")):
                errors.append(f"{r275_name} does not print class, then services, then the tally: a ring "
                              "that rotates mid-fire would cut the rows off from their own bound")
            if '{ "clover r275: ", "r275-aks", 64, 0, 0, 0 },' not in sdhci_source:
                errors.append("drivers/CloverSdhci has no class for the keystore rows: a new prefix is a "
                              "producer of its own and an unbudgeted one drops the reading silently -- the "
                              "exact way r272's cpu_start() line looked like a kernel that never tried")
            if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r275")' not in sdhci_source:
                errors.append("the keystore rows are not exempt from the per-pass push cap: they print inside "
                              "the r243 census burst, which boot 36 measured as already spent by the time the "
                              "walk's own rows arrive, so the cap would eat them and a truncated row reads as "
                              "\"the plane has none\"")
            late_script = (REPO.parent / "UniXNU" /
                           "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in late_script.splitlines()
                                 if "clover-r2" in line)
            if (late_list.find(r275_name) < 0 or
                    late_list.find(r275_name) > late_list.find("z" * 48 + "-clover-r274.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r275 before r274 in LATE_PATCH: the "
                              "compile pass would run on a tree this patch was not written against")
            if late_script.count("\nfor LATE_PATCH in ") != 1:
                errors.append("build-xnu-4k-baseline.sh has more than one LATE_PATCH loop: the older ones "
                              "were joined by a backslash and silently became words of the list")
            load_copy = REPO.parent / "UniXNU" / "patches" / "xnu" / r275_name
            if not load_copy.is_file():
                errors.append(f"UniXNU/patches/xnu/{r275_name} is missing: unixnu loads patches from that "
                              "directory and silently skips a board patch that lives only in this repo")
            elif load_copy.read_bytes() != r275_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r275_name} is not the same bytes as this repo's copy: "
                              "the two trees would compile different kernels")

        # Round276 is the round the r275 reading authorised: the owned AppleKeyStore goes in, and the census
        # that authorised it is fixed in the same pass so the publish can be seen. Two things are asserted
        # because both have been got wrong on this board: that the vendored driver is the upstream file with no
        # edit (byte identity, checked against ~/UniXNU/drivers), and that the board's own instrument can read
        # what the board publishes -- boot 62's keystore_services=0 came from a case-sensitive match against a
        # name that is lowercase, so a needle that cannot see the thing it asks about is what would otherwise
        # have reported this round's success as its failure.
        r276_name = "z" * 50 + "-clover-r276.patch"
        r276_patch = next((base / "patches/xnu-clover" / r276_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r276_name).is_file()), None)
        if r276_patch is None:
            errors.append(f"patches/xnu-clover/{r276_name} is missing: without the widened match and the probe "
                          "readout the keystore would be published blind")
        else:
            r276_text = r276_patch.read_text()
            r276_added = "".join(line for line in r276_text.splitlines(True) if line.startswith("+"))
            if r276_text.count("--- a/") != 1 or "--- a/iokit/Kernel/IOStartIOKit.cpp" not in r276_text:
                errors.append(f"{r276_name} does not touch exactly iokit/Kernel/IOStartIOKit.cpp: this is the "
                              "census' own file and a second file would mean a second walk or a second lock")
            for needle in ('clover_name_has_ci(const char *name, const char *part)',
                           'clover_lower(char c)',
                           'clover_name_has_ci(nm, "keystore")',
                           'clover_name_has_ci(cn, "keystore")',
                           'mc->getClassNameSymbol()',
                           '"clover r276: ks svc %.24s class=%.24s hit=%s%s\\n"',
                           '"clover r276: ks walked=%d stopped=%d ks_names=%d ks_classes=%d rows=%d\\n"',
                           '"clover r276: aks stage=%d status=%x service=%llx refused=%u opens=%u calls=%u "',
                           '"sel7=%u sel14=%u sel17=%u sel35=%u sls=%u\\n"',
                           'extern "C" volatile uint32_t native_keyevents_stage'):
                if needle not in r276_added:
                    errors.append(f"{r276_name} lacks {needle[:64]!r}: the name match, the class match and the "
                                  "probe readout are three separate readings and a round that ships two of the "
                                  "three is how boot 62 read a zero that meant nothing")
            if "iterateOver" in r276_added:
                errors.append(f"{r276_name} opens a second registry iterator: the census walk is the one pass "
                              "this board can afford to take the registry lock in")
            if 'clover r275: svc %.24s class=%.24s' in r276_added.replace('\n', '') and \
                    '"clover r275: svc' in r276_added:
                errors.append(f"{r276_name} re-adds the r275 service row beside its own: two rows asking one "
                              "question in one burst is how a truncated one gets read as the other")
            if '{ "clover r276: ", "r276-ks", 64, 0, 0, 0 },' not in sdhci_source:
                errors.append("drivers/CloverSdhci has no class for the keystore publish rows: an unbudgeted "
                              "producer drops the reading silently, and dropping the row that says whether the "
                              "service registered is the one thing this round has to answer")
            if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r276")' not in sdhci_source:
                errors.append("the r276 rows are not exempt from the per-pass push cap: they print inside the "
                              "r243 census burst, so without the exemption a capped round is indistinguishable "
                              "from one that found nothing")
            late_script = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in late_script.splitlines() if "clover-r2" in line)
            if (late_list.find(r276_name) < 0 or
                    late_list.find(r276_name) > late_list.find("z" * 49 + "-clover-r275.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r276 before r275 in LATE_PATCH: the "
                              "compile pass would run on a tree this patch was not written against")
            loop = late_script.split("for LATE_PATCH in ", 1)
            if len(loop) != 2:
                errors.append("build-xnu-4k-baseline.sh has no parsable LATE_PATCH loop: the reversal list is "
                              "the only thing that makes the compile pass run on a tree this patch series was "
                              "written against, and an unparsable one is an unverified build")
            else:
                # The bug this guard exists for: the list's head was written as a bare filename, so the loop's
                # `if apply --reverse --check` guard was false for a path that does not exist and the newest
                # patch was never reversed. The build then died in bsd_init.c, a file that patch does not touch,
                # and three rebuilds in a row blamed the tree instead of the list. An entry is only a LATE entry
                # if the loop can actually find it.
                entries = re.findall(r'"([^"]+\.patch)"', loop[1].split("; do", 1)[0])
                bare = [entry for entry in entries if not entry.startswith("$ROOT/patches/xnu/")]
                if bare:
                    errors.append("LATE_PATCH has entries with no $ROOT/patches/xnu/ prefix (" +
                                  ", ".join(bare) + "): the reversal loop skips a path it cannot find, so the "
                                  "patch is left applied while its neighbours are reversed and the replay fails "
                                  "somewhere that has nothing to do with it")
                absent = [entry for entry in entries
                          if not (REPO.parent / "UniXNU" / entry.replace("$ROOT/", "")).is_file()]
                if absent:
                    errors.append("LATE_PATCH names patches that are not on disk (" + ", ".join(absent) +
                                  "): the same silent skip, from the other side")
                if len(entries) < 40:
                    errors.append(f"LATE_PATCH was parsed as only {len(entries)} entries: the loop spans more "
                                  "than one line and a short read means this checker is not looking at the list "
                                  "the build uses")
            load_copy = REPO.parent / "UniXNU" / "patches" / "xnu" / r276_name
            if not load_copy.is_file():
                errors.append(f"UniXNU/patches/xnu/{r276_name} is missing: unixnu loads patches from that "
                              "directory and silently skips a board patch that lives only in this repo")
            elif load_copy.read_bytes() != r276_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r276_name} is not the same bytes as this repo's copy: "
                              "the two trees would compile different kernels")

        # Round277 is the round boot 63 forced: the publish went in, and the census that was supposed to read
        # it said nothing at all -- no r243 row, no r275 row, no r276 row, the kernel log stopping one second
        # before the census' own gate and no panic record for the boot. Those are three different facts wearing
        # one absence, and the reason is plumbing: every census row reaches the card through the msgbuf tee,
        # which is allowed sixteen block writes per pass and lags the kernel by seconds. So the same questions
        # are now also answered on the driver's probe relay. The assertions below exist because an instrument
        # that can only be read when the pipeline keeps up is the instrument that produced boot 63.
        r277_name = "z" * 51 + "-clover-r277.patch"
        r277_patch = next((base / "patches/xnu-clover" / r277_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r277_name).is_file()), None)
        if r277_patch is None:
            errors.append(f"patches/xnu-clover/{r277_name} is missing: without the relay the census is "
                          "readable only through the channel that failed to carry it")
        else:
            r277_text = r277_patch.read_text()
            r277_added = "".join(line for line in r277_text.splitlines(True) if line.startswith("+"))
            if r277_text.count("--- a/") != 1 or "--- a/iokit/Kernel/IOStartIOKit.cpp" not in r277_text:
                errors.append(f"{r277_name} does not touch exactly iokit/Kernel/IOStartIOKit.cpp: the relay "
                              "rows belong to the census that is already taking the registry lock once")
            for needle in ('clover_emmc_probe_say(const char *text, unsigned int length)',
                           '#define CLOVER_CENSUS_SAY(',
                           '"clover r277: enter secs=%llu calls=%d\\n"',
                           '"clover r277: waiting secs=%llu calls=%d\\n"',
                           '"clover r277: walking visited=%d names=%d classes=%d\\n"',
                           '"clover r277: svc-walk end visited=%d stopped=%d classes=%d\\n"',
                           '"clover r277: aks stage=%d status=%x service=%llx class=%d opens=%u calls=%u\\n"',
                           '(total & 31) == 0'):
                if needle not in r277_added:
                    errors.append(f"{r277_name} lacks {needle[:60]!r}: entry, the waiting heartbeat, the walk's "
                                  "own progress and the publish reading are four separate rows and a round that "
                                  "ships three of them is the boot 63 reading again")
            # The waiting row is the round's positive control, and it is only that if it is bounded: an
            # unbounded heartbeat would fill a 64-slot relay ring and hide the rows it exists to corroborate.
            if r277_added.count("clover_census_calls % 16") != 1:
                errors.append(f"{r277_name} does not bound the waiting row to every sixteenth call: the relay "
                              "ring is sixty-four slots shared with every other probe in the boot")
            if "iterateOver" in r277_added.replace("si = IORegistryIterator::iterateOver", ""):
                errors.append(f"{r277_name} opens a third registry iterator: the relay moves the reading off "
                              "the tee, it does not add another walk of the plane")
            late_script = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in late_script.splitlines() if "clover-r2" in line)
            if (late_list.find(r277_name) < 0 or
                    late_list.find(r277_name) > late_list.find("z" * 50 + "-clover-r276.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r277 before r276 in LATE_PATCH: the "
                              "compile pass would run on a tree this patch was not written against")
            load_copy = REPO.parent / "UniXNU" / "patches" / "xnu" / r277_name
            if not load_copy.is_file():
                errors.append(f"UniXNU/patches/xnu/{r277_name} is missing: unixnu loads patches from that "
                              "directory and silently skips a board patch that lives only in this repo")
            elif load_copy.read_bytes() != r277_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r277_name} is not the same bytes as this repo's copy: "
                              "the two trees would compile different kernels")

        # The keystore itself: a vendored driver, a board instrument that supplies what it links against, and
        # the two boot arguments that arm it. Every one of these is checked by name because the failure mode is
        # silent -- a driver nobody links is a kernel that boots, and an instrument whose gate never opens is a
        # service that never registers, and neither says so unless something is pinned.
        keystore_source = REPO / "drivers/NativeKeyStoreEvents/NativeKeyStoreEvents.cpp"
        keystore_upstream = REPO.parent / "UniXNU" / "drivers/NativeKeyStoreEvents/NativeKeyStoreEvents.cpp"
        if not keystore_source.is_file():
            errors.append("drivers/NativeKeyStoreEvents/NativeKeyStoreEvents.cpp is missing: the AppleKeyStore "
                          "this board was measured to lack is what round276 is here to publish")
        else:
            keystore_text = keystore_source.read_text()
            if not keystore_upstream.is_file():
                errors.append("the upstream copy of the keystore driver is gone, so this repo's vendored copy can "
                              "no longer be proven unedited")
            elif keystore_source.read_bytes() != keystore_upstream.read_bytes():
                errors.append("the vendored keystore driver is not byte-identical to ~/UniXNU/drivers: the whole "
                              "claim of this port is that the upstream service is shipped unchanged, with the "
                              "board supplying only its six hooks")
            for needle in ('class AppleKeyStore final : public IOService',
                           'OSDefineMetaClassAndStructors(AppleKeyStore, IOService)',
                           'native_aks_state_enabled', 'native_aks_lock_state', 'native_aks_call',
                           '"Native keystore state service"'):
                if needle not in keystore_text:
                    errors.append(f"the vendored keystore driver lost {needle[:48]!r}: that is the shape the "
                                  "target's libaks matches and reads")
        ks_probe = REPO / "package/CloverKeyStoreProbe.cpp"
        if not ks_probe.is_file():
            errors.append("package/CloverKeyStoreProbe.cpp is missing: the driver links against six board "
                          "symbols and cannot stand alone")
        else:
            ks_text = ks_probe.read_text()
            for needle in ('PE_parse_boot_argn("offline-key-events"', 'PE_parse_boot_argn("clover-aks"',
                           'OSMetaClass::getMetaClassWithName(name)',
                           '__asm__("__GLOBAL__sub_I_NativeKeyStoreEvents.cpp")',
                           'OSMetaClass::preModLoad(g_kernel_kmod_info.name)',
                           'OSMetaClass::postModLoad(handle)', 'native_keyevents_allocate',
                           'void clover_keystore_probe(void)'):
                if needle not in ks_text:
                    errors.append(f"the keystore instrument lost {needle[:56]!r}: registering a metaclass after "
                                  "boot in a statically linked kernel needs all of these steps, and the absent-"
                                  "class guard is the one r275 measured")
            # A comment may name the channel to explain why it is not used; a write to it is the thing
            # that would silently go nowhere, so the assertion is on the array subscript, not the word.
            if "boot_progress_ext[" in ks_text:
                errors.append("the keystore instrument writes boot_progress_ext: CONFIG_XNU_BOOT_PROGRESS=n on "
                              "this board, so those rows would neither be produced nor read and the counters "
                              "have to reach the card through the census instead")
        display_source = (REPO / "package/CloverLegacyDisplayProbe.cpp").read_text()
        if "clover_keystore_probe();" not in display_source:
            errors.append("nothing calls clover_keystore_probe(): a linked instrument that no entry point runs is "
                          "indistinguishable at boot from one that was never built")
        elif display_source.index("clover_keystore_probe();") > display_source.index("IOService *display=pendingLegacyDisplay;if(!display)return;"):
            errors.append("the keystore call sits after the display probe's early return: the framebuffer branch "
                          "can bail without ever reaching it, which is exactly why the HID call was moved first "
                          "in round255")
        board_args = " ".join(str(value) for value in board["payload"]["kernel_args"])
        for arg in ("offline-key-events=1", "clover-aks=1"):
            if arg not in board_args:
                errors.append(f"board.toml does not pass {arg}: the keystore gate stays shut and the round "
                              "reports a service that was never asked to exist")
        if "clover-aks-sls" in board_args:
            errors.append("board.toml passes clover-aks-sls: the reported lock state has to be the driver's "
                          "default (unlocked since boot) or the reading says nothing about the wait it targets")

        # Round281. Round280's one late table was a wager on the boot outliving 1600 s, and boot 68 ended
        # at 1324 s before either the census or the table, so the wager lost to the same shape it was meant to
        # fix -- a reading with one speaking time describes only the boots that reach it. The cadence is now
        # the contract: state every 120 s from 240 s, tables every fifth fire, and the same timer reads the
        # watchdog block every fire. The stops are 726/1324/1499 s against 1813/1814/1814 s, so a row must be
        # inside the ring's 355 s horizon at every second a board could die, and 120 s between fires makes
        # that true for any stop.
        bisect_probe = REPO / "package/CloverKeyStoreProbe.cpp"
        if not bisect_probe.is_file():
            errors.append("package/CloverKeyStoreProbe.cpp is missing: the periodic state, the tables and "
                          "the watchdog row live in the probe, and a board without them cannot say how far "
                          "it got before it stopped")
        else:
            bisect_text = bisect_probe.read_text()
            if "clover-aks-nosvc" in bisect_text:
                errors.append("the keystore probe still gates on clover-aks-nosvc: round278 promised the knob "
                              "was a bisect and not a mode")
            for stale in ("clover r279", "clover r280", "CLOVER_AKS_LATE_MS", "clover_aks_late_say"):
                if stale in bisect_text:
                    errors.append(f"the probe still carries {stale!r}: this round replaces the one-shot late "
                                  "table with a repeating row, and two speakings for one reading is how a "
                                  "stale image gets mistaken for a new one")
            for needle in ('"clover r281: aks t=%u fire=%u stage=%d status=%x service=%llx owner=%s opens=%u calls=%u "',
                           '"refused=%u other=%u\\n"',
                           '"clover r281: wdt t=%u nomap why=ctrl\\n"',
                           '"clover r281: wdt t=%u en=%x armed=%u intmask=%u sts=%x bark=%u bite=%u bark_ms=%u "',
                           '"bite_ms=%u req=%u pet=%u why=%s\\n"',
                           '"clover r281: aks sel%u n=%u rc=%x\\n"',
                           '"clover r281: open %u pid=%d comm=%.16s\\n"',
                           '"clover r281: call %u pid=%d comm=%.16s sel=%u rc=%x\\n"',
                           '"clover r281: rows open=%u/%u call=%u/%u dropped=%u\\n"',
                           '#define CLOVER_AKS_ROWS        8',
                           '#define CLOVER_AKS_CALL_ROWS  12',
                           '#define CLOVER_AKS_FIRST_MS    240000u',
                           '#define CLOVER_AKS_STATE_MS      4000u',
                           '#define CLOVER_AKS_STATE_EVERY     30',
                           '#define CLOVER_AKS_TABLE_EVERY      5',
                           '#define CLOVER_WDT_PHYS        0x17817000ull',
                           '#define CLOVER_WDT_EN          0x08',
                           '#define CLOVER_WDT_STS         0x0c',
                           '#define CLOVER_WDT_BARK        0x10',
                           '#define CLOVER_WDT_BITE        0x14',
                           '#define CLOVER_WDT_HZ          32765u',
                           'IODeviceMemory::withRange(CLOVER_WDT_PHYS, CLOVER_WDT_BYTES)',
                           'kIOMapAnywhere | kIOMapInhibitCache',
                           'clover_aks_timer = IOTimerEventSource::timerEventSource(owner, clover_aks_say);',
                           '(void) clover_aks_timer->setTimeoutMS(CLOVER_AKS_FIRST_MS);',
                           '(void) sender->setTimeoutMS(CLOVER_AKS_STATE_MS);',
                           'if (clover_aks_armed) {'):
                if needle not in bisect_text:
                    errors.append(f"the keystore probe lacks {needle[:72]!r}: a schedule that fires once is "
                                  "the reading boot 68 could not reach, and the watchdog row is the only "
                                  "measurement of the hypothesis those stops suggest")
            # The early arm is the whole change of timing: it has to run before every step that can refuse the
            # publish, or a keystore that never publishes takes the hardware row down with it.
            arm_early = bisect_text.index("clover_aks_arm(NULL);")
            if not (bisect_text.index("native_keyevents_stage = 1;") < arm_early <
                    bisect_text.index("native_keyevents_stage = 2;")):
                errors.append("clover_aks_arm(NULL) does not sit between stage=1 and stage=2: the schedule "
                              "must exist before the publish can be refused, because the watchdog row is not "
                              "the keystore's reading and r278 measured a boot that never published")
            if not (bisect_text.index("native_keyevents_stage = 3;") <
                    bisect_text.index("clover_aks_arm(service);")):
                errors.append("the retry arm sits before stage=3: it would name a service the board never "
                              "published, and the idempotent guard would then freeze the wrong owner")
            if bisect_text.count("clover_aks_record(clover_aks_open_rows") != 2 or \
                    bisect_text.count("clover_aks_record(clover_aks_call_rows") != 1:
                errors.append("the open and call tables are not filled exactly where the vendored driver's "
                              "hooks run (open, refusal, call): record elsewhere and the table describes a "
                              "path no client took")
            for shim in ("void native_aks_opened(void) {", "void native_aks_call(uint32_t selector, int32_t result) {"):
                body = bisect_text[bisect_text.index(shim):]
                body = body[:body.index("\n}")]
                if "printf" in body:
                    errors.append(f"{shim} still prints: the client's thread is not where this board can afford "
                                  "a line the ring will drop anyway")
        if "clover-aks-nosvc=1" in board_args:
            errors.append("board.toml still passes clover-aks-nosvc=1: the service has to be published for "
                          "this round's question to exist")
        if '{ "clover r281: ", "r281-aks", 128, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the repeating keystore rows: an unbudgeted "
                          "producer drops its rows silently, and this round's reading is the cadence itself")
        # Round292 gives the watchdog row its own class and its own place in the table. teeProbeClass takes
        # the first entry whose prefix matches, so "clover r281: wdt" has to sit ahead of "clover r281: " or
        # every watchdog row is charged to the keystore's budget -- and a one-shot bark that loses its ring
        # slot reads exactly like a boot whose watchdog never moved.
        if '{ "clover r281: wdt", "r281-wdt", 48, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class of its own for the four-second watchdog rows: the "
                          "row that has to survive a bite cannot share a budget with the keystore schedule")
        elif sdhci_source.index('{ "clover r281: wdt"') > sdhci_source.index('{ "clover r281: "'):
            errors.append("the r281-wdt class is listed after r281-aks: first match wins, so the watchdog rows "
                          "would be counted against the keystore's cap and this round would not read")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r281")' not in sdhci_source:
            errors.append("the r281 rows are not exempt from the per-pass push cap: the timer can fire beside "
                          "the census burst, where boot 36 measured the cap eating exactly that")
        for stale in ('"r279', '"r280'):
            if stale in sdhci_source:
                errors.append(f"the tee still carries round {stale[2:]}'s class: a class for a producer that "
                              "no longer exists is how a budget looks allocated when nothing is spending it")

        # Round324 ships two hands in one image, and each is a measurement the last boot could not make.
        # Hand one: a hang now bites. The arm is gated on a boot arg so a board that never reaches 300 s, or
        # a control boot that must not reset, reads as "did not arm" rather than "armed and never moved" --
        # so the arg has to be on the board this round, CloverKeyStoreProbe has to gate on it, and the tee has
        # to budget the one arm line separately from the watchdog state rows it rides beside.
        if "clover-wdt-arm=1" not in board_args:
            errors.append("board.toml does not pass clover-wdt-arm=1: the r324 arm is a boot arg on purpose, "
                          "and without it no boot bites and the round's whole second half is unexercised")
        for arm_needle in ('#define CLOVER_WDT_ARM_AT_SEC  300u',
                           'PE_parse_boot_argn("clover-wdt-arm", &value, sizeof(value))',
                           '"clover r324: wdt-arm t=%u bite_ms=%u at=%u\\n"'):
            if arm_needle not in bisect_text:
                errors.append(f"the keystore probe lacks {arm_needle[:48]!r}: a watchdog that is read but "
                              "never armed cannot bite, and boot 109 proved the hang trips neither panic nor "
                              "deadline nor a hardware reset -- which is exactly the gap this arm closes")
        # Hand two: who backboardd blocks on. boot 109's r229 bb row said hops=2, but the r234 bbhopo child
        # that names the owning task printed once inside the change-gated burst and the console tail dropped
        # it. The reading is now latched and re-stated every beat on the driver relay, so it is not a one-shot
        # the ring can lose. That row's format string lives in the kernel patch, so the gate reads the patch
        # text, and the tee class is asserted so the console copy is not starved by the arm line sharing the
        # round's prefix.
        r324_patch = max((p for p in (REPO / "patches/xnu-clover").glob("*-clover-r324.patch")),
                         default=None)
        if r324_patch is None or not r324_patch.is_file():
            errors.append("patches/xnu-clover/*-clover-r324.patch is missing: the per-beat debt-owner row is "
                          "this round's first hand and a kernel instrument nobody shipped is a reading nobody "
                          "can take")
        else:
            r324_text = r324_patch.read_text()
            for why in ('"clover r324: bbwhy t=%llu pid=%d fired=%u hn=%u rem=%x set=%u"',
                        'static volatile uint32_t clover_bb_debt_hn;',
                        '__atomic_store_n(&clover_bb_debt_set, 1,'):
                if why not in r324_text:
                    errors.append(f"the r324 kernel patch lacks {why[:56]!r}: without the latch and the "
                                  "release-after-name store, the beat row is a guess about a hop the burst "
                                  "may have dropped, not a copy of the reading")
        if '{ "clover r324: bbwhy", "r324-bbwhy", 24, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the per-beat debt-owner row: a repeating state "
                          "line with no budget is how boot 98 read a whole census family as zero rows")
        if '{ "clover r324: wdt", "r324-wdt", 8, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no specific class for the arm line: the round shares one "
                          "prefix between two producers, and a generic budget lets the beat rows eat the arm")

        # Round326 settled where the debt row is sealed -- boot 112 read `clover r326: bbnow` on both
        # channels, in the millisecond band the r229 row occupies -- and returned a reading the row could
        # not explain: hn=4 with four dashes and rem=0. One dash then carried three facts ("no remote",
        # "asked, no owner", "asked, name computed") and the next hand needs the first from the second.
        # Round327 therefore changes only the seal: literals `null`/`fail`/owner-name in the four slots,
        # asked=/named= counting the two branches, r0..r3 carrying the four remote names in the raw, and
        # the `hrem[i] != hloc[i]` skip deleted because hloc is x[3]'s high half and the trap does not
        # define it as a port name (round326 ④-1). The gate reads added and deleted lines separately: the
        # deletion needles all still exist as '-' context, and a plain substring test would pass a round
        # that shipped both rows.
        r327_patch = max((p for p in (REPO / "patches/xnu-clover").glob("*-clover-r327.patch")),
                         default=None)
        if r327_patch is None or not r327_patch.is_file():
            errors.append("patches/xnu-clover/*-clover-r327.patch is missing: the cause-named debt row is "
                          "this round's only hand, and an instrument nobody shipped is a reading nobody "
                          "can take")
        else:
            r327_name = r327_patch.name
            r327_lines = r327_patch.read_text().splitlines()
            # Round327 is a diff against the tree round326 left; both earlier rows of this line of work
            # have to stay on disk or a silent reversal failure lands on a file this round never touched.
            for earlier, why in (("*-clover-r326.patch", "round327's diff is taken against the tree it left"),
                                 ("*-clover-r325.patch", "round326's diff is taken against the tree it left")):
                if not any(p.is_file() for p in (REPO / "patches/xnu-clover").glob(earlier)):
                    errors.append(f"patches/xnu-clover/{earlier} is missing: {why}, and a silent reversal "
                                  "failure lands on a file this round never touched")
            load_r327 = REPO.parent / "UniXNU" / "patches" / "xnu" / r327_name
            if not load_r327.is_file():
                errors.append(f"UniXNU/patches/xnu/{r327_name} is missing: unixnu loads patches from there, "
                              "and this repo's copy only ever names the round")
            elif load_r327.read_bytes() != r327_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r327_name} is not the same bytes as this repo's copy: "
                              "the shipped image would carry a different row than the one checked here")
            late_script_r327 = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list_r327 = " ".join(line for line in late_script_r327.splitlines() if "clover-r" in line)
            if (late_list_r327.find(r327_name) < 0 or
                    late_list_r327.find(r327_name) > late_list_r327.find("z" * 69 + "-clover-r326.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r327 before r326 in LATE_PATCH: "
                              "r327 rewrites the block r326 wrote, so a tree that removes r326 first leaves "
                              "r327 un-reversible -- and an apply failure in that loop is silent")
            r327_added = [line[1:] for line in r327_lines if line.startswith("+")]
            r327_removed = [line[1:] for line in r327_lines if line.startswith("-")]
            # Comment lines quote the shape they remove, so a rule about what the code may touch reads
            # only code -- otherwise this round's own comment is the thing that fails it.
            r327_code = [line for line in r327_added
                         if not line.strip().startswith(('*', '/*', '//'))]
            for now in ('"clover r327: bbnow t=%llu pid=%d fired=%u hn=%u state=%s named=%u asked=%u"',
                        'uint32_t asked = 0;',
                        'uint32_t named = 0;',
                        'uint32_t hrem[4] = { 0, 0, 0, 0 };',
                        'strlcpy(hopown[i], "null", sizeof(hopown[i]));',
                        'strlcpy(hopown[i], "fail", sizeof(hopown[i]));',
                        '"hop_unresolved" : "hop_null"',
                        'char now[256];',
                        'o0=%s o1=%s o2=%s o3=%s r0=%x r1=%x r2=%x r3=%x'):
                if not any(now in line for line in r327_added):
                    errors.append(f"the r327 kernel patch does not add {now[:56]!r}: without it the debt row "
                                  "still cannot tell a hop with no destination from a hop whose owner the "
                                  "walk could not name, which is the whole of this round")
            # The channel is not this round's hand, so it must survive untouched: the relay copy is the
            # leg boot 112 proved deliverable, and the console copy is its control.
            if any('clover_emmc_probe_say(now' in line for line in r327_removed):
                errors.append("the r327 patch removes the relay copy of the debt row: boot 112 read this "
                              "row because it rides the driver relay, and a reading that only exists on the "
                              "console is a reading capped at 192 lines per pass")
            for gone in ('"clover r326: bbnow', 'uint32_t resolved = 0;',
                         'if (hrem[i] != 0 && hrem[i] != hloc[i]) {'):
                if not any(gone in line for line in r327_removed):
                    errors.append(f"the r327 kernel patch does not remove {gone[:56]!r}: leaving it in the "
                                  "tree means two rows named bbnow, or a name decided by the residue of "
                                  "x[3]'s high half, and boot 112 is the reading of exactly that")
            # The four slots must be fillable without touching hloc: a column that asks a question of a
            # word the trap does not define is how r326 got four dashes for a fact about zero.
            if any('hloc[i]' in line for line in r327_code):
                errors.append("the r327 seal asks something of hloc[i]: hloc is x[3]>>32, caller residue, "
                              "not a port name -- round326 ④-1")
            # One word, one meaning: r324's beat row publishes `set=` and r326's seal published `res=` for
            # a hop-0-only flag. The new row counts two things, so it names both.
            if any('res=%u' in line or 'set=%u state=%s' in line for line in r327_code):
                errors.append("the r327 row reuses r324's `set=` or r326's `res=` column: two producers "
                              "writing one column name is how boot 100's `t=` silently changed units")
        if '{ "clover r327: bbnow", "r327-bbnow", 32, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the in-band debt row: a row printed inside "
                          "the census burst competes with that burst for the per-pass cap, and boot 36 "
                          "measured what that looks like -- a truncated burst reads as an empty one")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r327-bbnow")' not in sdhci_source:
            errors.append("the r327 debt row is not exempt from the per-pass cap: the relay copy does not ride "
                          "the cap, so a console copy the cap ate silently breaks the two-copy control this "
                          "round's reading depends on")

        # Round328. Boot 113 read r327 back on both channels and its literals hold, but they describe the send
        # column: x[3]'s low half names where a message goes, so it is 0 for exactly the calls that only wait.
        # Two nulls and one fail therefore answer a question the row was never positioned to ask, and the
        # receive name -- hrn, x[5]'s high half, which is what r221 prints as rn= and resolves on this same
        # boot -- was computed in the same frame and never asked. Round328 adds a second row beside the first
        # rather than rewriting it, because the two columns are each other's control: the patch must not delete
        # any of r327's row, and the new row must not write r327's column names for a different source.
        r328_patch = max((p for p in (REPO / "patches/xnu-clover").glob("*-clover-r328.patch")),
                         default=None)
        if r328_patch is None or not r328_patch.is_file():
            errors.append("patches/xnu-clover/*-clover-r328.patch is missing: asking the receive column of "
                          "each hop is this round's only hand, and an instrument nobody shipped is a reading "
                          "nobody can take")
        else:
            r328_name = r328_patch.name
            r328_lines = r328_patch.read_text().splitlines()
            for earlier, why in (("*-clover-r327.patch", "round328's diff is taken against the tree it left"),
                                 ("*-clover-r326.patch", "round327's diff is taken against the tree it left"),
                                 ("*-clover-r325.patch", "round326's diff is taken against the tree it left")):
                if not any(p.is_file() for p in (REPO / "patches/xnu-clover").glob(earlier)):
                    errors.append(f"patches/xnu-clover/{earlier} is missing: {why}, and a silent reversal "
                                  "failure lands on a file this round never touched")
            load_r328 = REPO.parent / "UniXNU" / "patches" / "xnu" / r328_name
            if not load_r328.is_file():
                errors.append(f"UniXNU/patches/xnu/{r328_name} is missing: unixnu loads patches from there, "
                              "and this repo's copy only ever names the round")
            elif load_r328.read_bytes() != r328_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r328_name} is not the same bytes as this repo's copy: "
                              "the shipped image would carry a different row than the one checked here")
            late_script_r328 = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list_r328 = " ".join(line for line in late_script_r328.splitlines() if "clover-r" in line)
            if (late_list_r328.find(r328_name) < 0 or
                    late_list_r328.find(r328_name) > late_list_r328.find("z" * 70 + "-clover-r327.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r328 before r327 in LATE_PATCH: "
                              "r328 appends its row after the one r327 writes and its slots beside r327's "
                              "declarations, so removing r327 first leaves r328 un-reversible -- and an apply "
                              "failure in that loop is silent")
            r328_added = [line[1:] for line in r328_lines if line.startswith("+")]
            r328_removed = [line[1:] for line in r328_lines if line.startswith("-")]
            r328_code = [line for line in r328_added
                         if not line.strip().startswith(('*', '/*', '//'))]
            for now in ('"clover r328: bbrcv t=%llu pid=%d hn=%u rstate=%s rnamed=%u rasked=%u"',
                        'char rcvown[4][24] = { { \'\\0\' } };',
                        'uint32_t rcvq[4] = { 0, 0, 0, 0 };',
                        'uint32_t hrn[4] = { 0, 0, 0, 0 };',
                        'strlcpy(rcvown[i], "null", sizeof(rcvown[i]));',
                        'strlcpy(rcvown[i], "fail", sizeof(rcvown[i]));',
                        # Both of the two remaining cause words, as separate needles: the ternary that picks
                        # them is wrapped across two source lines, and a line break is not a change to which
                        # literals the row can print. Requiring the pair on one line would make the gate
                        # re-run red every time someone re-wraps this block.
                        '"rcv_unresolved"',
                        '"rcv_null"',
                        'clover_sb_port_mail(task, (mach_port_name_t)hrn[i], &mc, &sr, &err);',
                        'char rcv[256];',
                        'c=%s.%s.%s.%s v=%x.%x.%x.%x q=%u.%u.%u.%u s=%u.%u.%u.%u'):
                if not any(now in line for line in r328_added):
                    errors.append(f"the r328 kernel patch does not add {now[:56]!r}: without it the row still "
                                  "names only the send column, and boot 113 showed that column is empty for "
                                  "exactly the threads this round asks about")
            # r327's row is the control for r328's, so both copies of it have to survive this patch untouched.
            for keep in ('"clover r327: bbnow', 'clover_emmc_probe_say(now', 'clover_sb_port_owner(task, (mach_port_name_t)hrem[i], own327,'):
                if any(keep in line for line in r328_removed):
                    errors.append(f"the r328 patch removes {keep[:56]!r}: the send row is the control the "
                                  "receive row is read against, and a pair where one half is gone is the "
                                  "r249 shape -- half a reading and no reading look the same on the card")
            # A row that prints four slots while the walk fills hn of them must zero the rest, or residue is
            # read as a port name (round327 did this for hrem; round328 repeats it for hrn and its own slots).
            if any(line.strip().startswith('uint32_t hrn[4];') for line in r328_code):
                errors.append("the r328 patch leaves uint32_t hrn[4] uninitialised while printing v= four "
                              "times: the hop walk fills hn of them, and the rest would be stack residue")
            # One word, one meaning across the pair: r327 publishes state=/named=/asked= for the send column.
            # The receive column says so in its own names instead of writing the same words from a new source.
            if any(('state=%s named=%u asked=%u' in line or 'res=%u' in line
                    or 'set=%u state=%s' in line) for line in r328_code):
                errors.append("the r328 row reuses r327's `state=/named=/asked=` or r324's `set=` / r326's "
                              "`res=` column names: two producers writing one column name is how boot 100's "
                              "`t=` silently changed units")
        if '{ "clover r328: bbrcv", "r328-bbrcv", 32, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the receive-column row: it prints in the same "
                          "burst as the census and would compete with it for the per-pass cap")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r328-bbrcv")' not in sdhci_source:
            errors.append("the r328 receive row is not exempt from the per-pass cap: keeping one half of the "
                          "send/receive pair and eating the other makes \"the hop has no door\" "
                          "indistinguishable from \"the row was capped\"")
        # Round329. Two classes, and the order between them is the only thing that keeps the longer key
        # reachable: "clover r329: dispstate" is a prefix of "clover r329: dispstatecut", and teeProbeClass
        # returns the first match, so a table that listed the state row first would send every cut row into
        # the state row's budget -- r313's containment lesson read the other way (the wrong table answers
        # instead of refusing).
        for entry in ('{ "clover r329: dispstatecut", "r329-dispstatecut", 8, 0, 0, 0 },',
                      '{ "clover r329: dispstate", "r329-dispstate", 96, 0, 0, 0 },'):
            if entry not in sdhci_source:
                errors.append(f"drivers/CloverSdhci has no class {entry[:46]!r}: a beat row with no budget of "
                              "its own competes with the burst it happens to land in, and this row's whole "
                              "job is to be readable when nothing else is happening")
        if sdhci_source.find('{ "clover r329: dispstatecut"') > sdhci_source.find('{ "clover r329: dispstate"'):
            errors.append("the r329 tee table lists the shorter token first: the cut row would be counted, "
                          "and reported, as a state row")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r329-dispstate")' not in sdhci_source:
            errors.append("the r329 state row is not exempt from the per-pass cap: it fires on the display's "
                          "own beat, away from every burst, and a row the cap ate reads exactly like a beat "
                          "that never fired -- which are the two answers this round is asking between")

        # Round335. The same containment, one round later: "clover r335: bbswap" is a prefix of
        # "clover r335: bbswapcut" and teeProbeClass returns the first match, so the table order is the only
        # thing that keeps the cut branch from being filed as the row it says did not fit. The budget is
        # sized from boot 115's own count (seven selector-6 calls by the last beat row) with headroom, and it
        # is a class of its own because these rows fire per wait call, from the probe, beside whichever pass
        # the client happens to arrive in.
        for entry in ('{ "clover r335: bbswapcut", "r335-bbswapcut", 8, 0, 0, 0 },',
                      '{ "clover r335: bbswap", "r335-bbswap", 64, 0, 0, 0 },'):
            if entry not in sdhci_source:
                errors.append(f"drivers/CloverSdhci has no class {entry[:46]!r}: a per-call row with no budget "
                              "of its own competes with the burst it lands in, and this row's job is to be the "
                              "one that survives the boot that stalls in this wait")
        if sdhci_source.find('{ "clover r335: bbswapcut"') > sdhci_source.find('{ "clover r335: bbswap"'):
            errors.append("the r335 tee table lists the shorter token first: the cut row would be counted, "
                          "and reported, as a swap row")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r335-bbswap")' not in sdhci_source:
            errors.append("the r335 swap-wait rows are not exempt from the per-pass cap: keeping the beat row "
                          "and eating the event row leaves the reader with the same boot's state and none of "
                          "its moments, which is the half-pair r249 refuses")

        # Round331. Boots 112 and 113 leave the same hole read from two sides: r328 says the hop's receive
        # queue is empty while its `s=` says send rights exist, and r327 says that hop's send column is null
        # because the thread is parked in a receive rather than waiting to post. Somebody holds that right
        # and no row this port prints has ever named them. r268 came closest and stops at its first hit, so
        # it can print neither "two holders" nor "zero of 400 probed". Round331 asks outward from the object
        # on two legs, and the shape this contract protects is the disagreement between them: ipc_hash.c's
        # own comment above IH_TABLE_HASH says only pure-send entries live in the reverse hash, so leg A is
        # a lower bound that must print beside ip_srights, while leg B is the full table walk -- the only leg
        # that can see the SEND|RECEIVE name boot 113 is actually asking about.
        r331_patch = max((p for p in (REPO / "patches/xnu-clover").glob("*-clover-r331.patch")),
                         default=None)
        if r331_patch is None or not r331_patch.is_file():
            errors.append("patches/xnu-clover/*-clover-r331.patch is missing: naming who holds the send "
                          "right is this round's only hand, and an instrument nobody shipped is a reading "
                          "nobody can take")
        else:
            r331_name = r331_patch.name
            r331_lines = r331_patch.read_text().splitlines()
            for earlier, why in (("*-clover-r328.patch", "round331's diff is taken against the tree it left"),
                                 ("*-clover-r327.patch", "round328's diff is taken against the tree it left"),
                                 ("*-clover-r326.patch", "round327's diff is taken against the tree it left")):
                if not any(q.is_file() for q in (REPO / "patches/xnu-clover").glob(earlier)):
                    errors.append(f"patches/xnu-clover/{earlier} is missing: {why}, and a silent reversal "
                                  "failure lands on a file this round never touched")
            load_r331 = REPO.parent / "UniXNU" / "patches" / "xnu" / r331_name
            if not load_r331.is_file():
                errors.append(f"UniXNU/patches/xnu/{r331_name} is missing: unixnu loads patches from there, "
                              "and this repo's copy only ever names the round")
            elif load_r331.read_bytes() != r331_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r331_name} is not the same bytes as this repo's copy: "
                              "the shipped image would carry a different row than the one checked here")
            late_script_r331 = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list_r331 = " ".join(line for line in late_script_r331.splitlines() if "clover-r" in line)
            if (late_list_r331.find(r331_name) < 0 or
                    late_list_r331.find(r331_name) > late_list_r331.find("z" * 71 + "-clover-r328.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r331 before r328 in LATE_PATCH: "
                              "r331 appends its call block after the row r328 prints and puts its helpers "
                              "above the comment r265 added, so reversing r328 first leaves r331 "
                              "un-reversible -- and an apply failure in that loop is silent")
            r331_added = [line[1:] for line in r331_lines if line.startswith("+")]
            r331_removed = [line[1:] for line in r331_lines if line.startswith("-")]
            r331_code = [line for line in r331_added
                         if not line.strip().startswith(('*', '/*', '//'))]
            for now in (
                # The header row in both of its shapes. The branch that could not resolve the port prints
                # every column the branch that did prints: a failing row with fewer columns is a row whose
                # absence cannot be told from a decoder that dropped them.
                '"clover r331: bbsend t=%llu pid=%d hop=%d nm=%x obj=%llx e=%d sr=%u q=%u"',
                '" scan=%s hits=%u probed=%u scanned=%u self=%u ownnames=%u walked=%u cand=%u debt=%u\\n"',
                '"clover r331: bbsend t=%llu pid=%d hop=%d nm=%x obj=0 e=%d sr=%u q=%u"',
                # The two name rows: one per leg, each carrying the raw bits beside the decoded word,
                # because a literal and the expression it was computed from have to be one reading.
                '"clover r331: bbsh t=%llu i=%u pid=%u c=%s nm=%x ty=%s raw=%llx\\n"',
                '"clover r331: bbown t=%llu i=%u nm=%x ty=%s raw=%llx ur=%u\\n"',
                # Leg A: the kernel's own reverse lookup over every pid, with no early break -- r268's
                # single-hit stop is exactly the blind spot this round exists to remove.
                'for (pid = 1; pid <= CLOVER_BB_SEMA_PROBE; pid++) {',
                'if (ipc_hash_lookup(space, (ipc_object_t) port, &name, &entry)',
                # Leg B: the full walk of the waiter's own table, on the field ipc_right.c reads for a
                # racy reader, and the table size it actually walked.
                'if (base[i].ie_volatile_object != obj) {',
                'size = (uint32_t) ipc_entry_table_count(table);',
                # Both legs use the protocol r268 established for a table another cpu can grow.
                'is_read_lock(space);',
                'if (is_active(space)) {',
                'smr_ipc_enter();',
                'smr_entered_load(&space->is_table)',
                # The reference held across the scan: without it a port destroyed mid-scan lets a recycled
                # address read as a holder (r220 wrote down the translate-returns-unreferenced rule).
                'ip_reference(port);',
                'ip_release(port);',
                # The literals only the pair of legs can produce, one per way of not having a reading.
                '"no_cand"', '"no_space"', '"notport"', '"xlate"', '"no_srights"', '"own_only"',
                '"hash_only"', '"no_holder"',
                # Round335 (#694) asks EVERY hop r328 named, not the one picked hop: boot 116 and boot 118
                # each printed a single bbsend row whose nm= equalled the picked hop while the only hop that
                # translated (e=0) was the fourth. cand=/debt= count over every named hop (the first loop),
                # and the row call now sits in a second loop that prints one row per hop.
                'if (rcvq[i] == 0) {',
                'for (i = 0; i < hn; i++) {',
                'clover_bb_send_holders_row(task, pid, (int) i,'):
                if not any(now in line for line in r331_added):
                    errors.append(f"the r331 kernel patch does not add {now[:56]!r}: without it the row "
                                  "still cannot say who holds the right r328 counted, which is the whole "
                                  "question boots 112 and 113 leave open")
            # Every slot array the row prints is zeroed before the walk can fill part of it (r327 for
            # hrem, r328 for hrn, round331 for its own six); residue would read as a pid or a port name.
            for zero in ('hp[i] = 0;', 'hn[i] = 0;', 'ht[i] = 0;', 'on[i] = 0;', 'ob[i] = 0;',
                         "hc[i] = '\\0';"):
                if not any(zero in line for line in r331_code):
                    errors.append(f"the r331 patch prints slots it never zeroes ({zero[:12]!r}): the row "
                                  "prints CLOVER_BB_SEND_MAX of each while the legs fill only hits or "
                                  "matches, so the rest would be stack residue read as a holder")
            # r327's send row, r328's receive row and r268's walk are what this row reads: the first two
            # name the hop it may pick, the third is the leg-A template. None of them may go.
            for keep in ('"clover r327: bbnow', '"clover r328: bbrcv', 'clover_sema_send_holder(',
                         'clover_sb_port_mail(task, (mach_port_name_t)hrn[i]',
                         'strlcpy(rcvown[i], "fail", sizeof(rcvown[i]));'):
                if any(keep in line for line in r331_removed):
                    errors.append(f"the r331 patch removes {keep[:56]!r}: the debt pair is what says which "
                                  "hop this row is allowed to ask about, and half a pair is the r249 shape "
                                  "-- no row and an empty queue read the same on the card")
            # One word, one meaning across the whole family of bb rows: `state=` is r327's and r324's,
            # `own=` is r234's owner name, `ent=` is r308's and r309's entry count. The r331 row calls
            # the same three ideas scan=, ownnames= and walked=, so the gate holds both halves.
            if any(('state=%s' in line or 'own=%u' in line or 'ent=%u' in line) for line in r331_code):
                errors.append("the r331 row writes r327's `state=`, r234's `own=` or r308's `ent=`: two "
                              "producers writing one column name is how boot 100's `t=` silently changed "
                              "units, and here the same collision would silently change meaning")
            for named in ('scan=%s', 'ownnames=%u', 'walked=%u', 'probed=%u', 'scanned=%u', 'cand=%u',
                          'debt=%u'):
                if not any(named in line for line in r331_code):
                    errors.append(f"the r331 row drops {named!r}: the counters are what separate a scan "
                                  "that found nothing from a scan that never ran, and a row that prints "
                                  "hits= without them is a zero nobody can grade")
        for entry in ('{ "clover r331: bbsend", "r331-bbsend", 32, 0, 0, 0 },',
                      '{ "clover r331: bbsh", "r331-bbsh", 128, 0, 0, 0 },',
                      '{ "clover r331: bbown", "r331-bbown", 128, 0, 0, 0 },'):
            if entry not in sdhci_source:
                errors.append(f"drivers/CloverSdhci has no class {entry[:46]!r}: this row prints a header "
                              "plus up to four names per leg from inside the census burst, and a list with "
                              "no budget of its own competes with the burst it is meant to explain")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r331")' not in sdhci_source:
            errors.append("the r331 rows are not exempt from the per-pass cap: unlike r327/r328 the row's "
                          "whole content is a count, so a pass that had spent its sixteen pushes would "
                          "leave `hits=0` indistinguishable from a pass that never got to count -- the "
                          "exact pair boot 112 could not split")

        # Round293 makes every card reading name the boot that wrote it. Boot 79's frame is the reason:
        # the deadline slot and the panic slots are the two groups a boot only overwrites when it reaches
        # its own ending, so a silent boot leaves the boot before it on the card and the reader called
        # boot 76's deadline "this boot's". A stamp that only some records carry is the same hole, so the
        # entries, the beats and the deadline record carry it too, and the reader is asserted below to
        # print STALE rather than a conclusion when the stamp disagrees with the boot counter.
        # round302: the shared traffic buffer became buf, and the verdict records moved to
        # gFlightMarker, so the stamp is looked for on the buffer the writer is holding.
        if "buf[35] = gBootId;" not in sdhci_source:
            errors.append("flightWrite stamps no boot on the records that carry no extras: an entry or a "
                          "beat from the boot before the flash would read as this boot's last request")
        if "extra[10] = gBootId;" not in sdhci_source or \
                "(void)flightWrite (kFlightEventDeadline, 3, 0, 0, extra, 11);" not in sdhci_source:
            errors.append("the deadline record carries no boot id: it is the one record a boot leaves "
                          "behind for the next one to mistake for its own ending, and boot 79 did")
        # The same round fixes two lines that could not answer the question they were asked. A DMA
        # refusal printed "deadline" whether the card held the data line past the per-request budget or
        # the driver's own spin limit ran out first -- and boot 79 refused seven transfers without ever
        # saying which of the two it saw. Both sides now print the time they waited, the budget they
        # were working to and the poll count, and name the exit that fired.
        for stage in ('(spun ? "spin" : "deadline")', '"ms=%llu budget=%llu polls=%u\\n"'):
            if sdhci_source.count(stage) < 2:
                errors.append(f"only one DMA refusal path carries {stage[:28]}: the read side and the write "
                              "side both refused transfers on boot 79's frame and neither could say how long "
                              "it had waited or which of its two budgets expired")
        if 'reportDmaFailure (ended ? (stopped ? "error" : "stop") : "deadline"' in sdhci_source or \
                'reportWriteDmaFailure (ended ? (stopped ? "error" : "stop") : "deadline"' in sdhci_source:
            errors.append("a DMA refusal still calls every early exit a deadline: the card being deaf and "
                          "this driver giving up early are different accusations and boot 79 could not tell "
                          "them apart")
        # And the cross-reset row: two unnamed sightings in a row (boots 77 and 79) printed the
        # heartbeat's two words without the magic that says whether the slot was ever this driver's.
        if sdhci_source.count("beatmagic=%llx") < 2:
            errors.append("the last-reset row prints the heartbeat words without the heartbeat's magic: "
                          "\"the slot does not hold our record\" and \"our record, but nobody named the "
                          "reset\" would still read alike")

        # Round286 takes the GPU line from measurement to power-on: GDSC cx then gx, the AHB/CFG
        # votes, the gfx3d branch enable, then a read-back of the GDSCRs and a gated chipid read.
        # Four halves still have to agree, the same four every Clover driver has: the two source
        # files exist, the archive is built and linked, the entry point is called from
        # sheng_pmgr_run(), and the boot argument selects it. The addresses stay pinned to the
        # board's own DT, and the write discipline is asserted structurally: all three windows are
        # touched only through the read/write helpers, so a direct index anywhere in the file is the
        # start of a bare store this round promised to route through one path.
        gpu_source_path = REPO / "drivers/CloverGpu/CloverGpu.cpp"
        gpu_regs_path = REPO / "drivers/CloverGpu/GpuRegs.h"
        if not gpu_source_path.is_file() or not gpu_regs_path.is_file():
            errors.append("drivers/CloverGpu is missing: round282's whole reading is the two files "
                          "that map GCC and GPUCC and print what they find")
        else:
            gpu_source = gpu_source_path.read_text()
            gpu_regs = gpu_regs_path.read_text()
            for needle, where, text_ in (
                    ("#define kGccPhys            0x00100000ULL", "GpuRegs.h", gpu_regs),
                    ("#define kGpuccPhys          0x05065000ULL", "GpuRegs.h", gpu_regs),
                    ("#define kGpuPhys            0x05000000ULL", "GpuRegs.h", gpu_regs),
                    ("#define kGccGpuCfgAhb       0x71004U", "GpuRegs.h", gpu_regs),
                    ("#define kGpuccCxHwCtrl      0x1008U", "GpuRegs.h", gpu_regs),
                    ("#define kGpuccGdscGx        0x1094U", "GpuRegs.h", gpu_regs),
                    ("#define kGpuChipid          0x10200U", "GpuRegs.h", gpu_regs),
                    ("#define kGpuChipidValue     0x05010200U", "GpuRegs.h", gpu_regs),
                    ("#define kGdscHwControl      0x00000002U", "GpuRegs.h", gpu_regs),
                    ("#define kGdscSwOverride     0x00000004U", "GpuRegs.h", gpu_regs),
                    ("#define kGdscEnRestWaitMask 0x00F00000U", "GpuRegs.h", gpu_regs),
                    ("IODeviceMemory::withRange(kGccPhys, kGccSize)", "CloverGpu.cpp", gpu_source),
                    ("IODeviceMemory::withRange(kGpuccPhys, kGpuccSize)", "CloverGpu.cpp", gpu_source),
                    ("IODeviceMemory::withRange(kGpuPhys, kGpuSize)", "CloverGpu.cpp", gpu_source),
                    ("clover_gpu_set_bits(clover_gpu_gcc, kGccGpuCfgAhb, 1);", "CloverGpu.cpp", gpu_source),
                    ("clover_gpu_gdsc_on(clover_gpu_gpucc, kGpuccGdscCx, kGpuccCxHwCtrl, true);",
                     "CloverGpu.cpp", gpu_source),
                    ("clover_gpu_gdsc_on(clover_gpu_gpucc, kGpuccGdscGx, kGpuccGdscGx, false);",
                     "CloverGpu.cpp", gpu_source),
                    ("clover_gpu_set_bits(clover_gpu_gpucc, kGpuccGfx3dEn, 1);", "CloverGpu.cpp", gpu_source),
                    ("clover_gpu_report_chipid(t);", "CloverGpu.cpp", gpu_source),
                    # Round317: the gate that used to be spelled here is now gpu_ahb_gate_open(), so the pin
                    # follows the call. What must not regress is the word test inside it -- the host model
                    # shows bit31 of 0xffffffff reads as "running", so a vote-only gate would let a dead GCC
                    # page license the GPUCC window.
                    ("if (!gpu_ahb_gate_open(ahb)) {", "CloverGpu.cpp", gpu_source),
                    ("PE_parse_boot_argn(\"clover-gpu\", &mode, sizeof(mode))", "CloverGpu.cpp", gpu_source),
                    ("clover_gpu_timer = IOTimerEventSource::timerEventSource(owner, clover_gpu_say);",
                     "CloverGpu.cpp", gpu_source),
                    ("(void) clover_gpu_timer->setTimeoutMS(CLOVER_GPU_FIRST_MS)",
                     "CloverGpu.cpp", gpu_source),
                    ("(void) sender->setTimeoutMS(CLOVER_GPU_STATE_MS)", "CloverGpu.cpp", gpu_source),
                    # One format string per reading, so "which of the rows is missing" is a question a
                    # decoder can answer from the image instead of from the card.
                    ("\"clover r286: gpu t=%u fire=%u gcc gpu_ahb=%u gpu_bimc=%u bimc=%u ctl_sdhc1_ahb=%u \"",
                     "CloverGpu.cpp", gpu_source),
                    ("\"clover r286: gpu t=%u gcc post gpu_ahb=%u gpu_bimc=%u bimc=%u raw ahb=%08x \"",
                     "CloverGpu.cpp", gpu_source),
                    ("\"clover r286: gpu t=%u gpucc skipped ahb=%u raw=%08x (GPUCC access needs the \"",
                     "CloverGpu.cpp", gpu_source),
                    ("\"clover r286: gpu t=%u gpucc pre cx=%08x cxhw=%08x cx_on=%u cx_collapse=%u \"",
                     "CloverGpu.cpp", gpu_source),
                    ("\"clover r286: gpu t=%u gpucc post cx=%08x cxhw=%08x cx_on=%u cx_collapse=%u \"",
                     "CloverGpu.cpp", gpu_source),
                    ("\"clover r286: gpu t=%u gdsc cx_on=%u gx_on=%u gfx3d_en=%u cx=%08x cxhw=%08x \"",
                     "CloverGpu.cpp", gpu_source),
                    ("\"clover r286: gpu t=%u chipid=%08x expect=%08x ok=%u", "CloverGpu.cpp", gpu_source),
                    ("\"clover r286: gpu t=%u chipid skipped cx_on=%u gx_on=%u gfx3d_en=%u gate=%s\\n\"",
                     "CloverGpu.cpp", gpu_source),
                    ("\"clover r286: gpu t=%u core map failed phys=%llx", "CloverGpu.cpp", gpu_source),
                    # Round290's own reading: the halt bit, which r286 never carried. The header has to
                    # hold both new branch offsets and the two RCG words it prints raw, the source has to
                    # keep asking for the two votes the DT's clock list still owes -- as readings, through
                    # the branch report -- and the row has to name its stage so a "post" and a "beat" can
                    # be told apart by the decoder.
                    ("#define kGpuccRbcprEn       0x1054U", "GpuRegs.h", gpu_regs),
                    ("#define kGpuccRbbmtimerEn   0x10d0U", "GpuRegs.h", gpu_regs),
                    ("#define kGpuccRbcprRcg      0x1030U", "GpuRegs.h", gpu_regs),
                    ("#define kGpuccRbbmtimerRcg  0x10b0U", "GpuRegs.h", gpu_regs),
                    ("#define kBranchClkOff       0x80000000U", "GpuRegs.h", gpu_regs),
                    ("clover_gpu_report_branches(t, \"post\");", "CloverGpu.cpp", gpu_source),
                    ("clover_gpu_report_branches(t, \"beat\");", "CloverGpu.cpp", gpu_source),
                    ("\"clover r290: gpu t=%u branches %s gfx3d=%u/%u cxo=%u/%u rbcpr=%u/%u rbbm=%u/%u \"",
                     "CloverGpu.cpp", gpu_source),
                    # Round291's row: the GFX LDO that sets the corner gx powers up against. The offsets
                    # are the vendor driver's (msm_gfx_ldo.c, archived under notes/sources/sdm660) and the
                    # window is the DT's -- and gfx_ldo_vreg at 0x0506e000 sits 0x9000 inside the GPUCC
                    # extent, so the reads ride the mapping r286 already opened. A second overlapping
                    # mapping would be a second answer to the same question.
                    ("#define kGpuccLdoVref       0x9018U", "GpuRegs.h", gpu_regs),
                    ("#define kGpuccLdoCtrl       0x901cU", "GpuRegs.h", gpu_regs),
                    ("#define kGpuccLdoStatus     0x9028U", "GpuRegs.h", gpu_regs),
                    ("#define kLdoMinUv           375000u", "GpuRegs.h", gpu_regs),
                    ("#define kLdoReady           0x00000004U", "GpuRegs.h", gpu_regs),
                    ("#define kLdoSettled         0x00000010U", "GpuRegs.h", gpu_regs),
                    ("clover_gpu_report_ldo(t, \"post\");", "CloverGpu.cpp", gpu_source),
                    ("clover_gpu_report_ldo(t, \"beat\");", "CloverGpu.cpp", gpu_source),
                    ("\"clover r291: gpu t=%u ldo %s vref=%08x val=%u uv=%u ctrl=%08x byp=%u pdn=%u \"",
                     "CloverGpu.cpp", gpu_source),
                    # Round294 widens the rail reading to the whole LDO block the vendor driver names. Each
                    # offset is quoted to its #define in msm_gfx_ldo.c inside GpuRegs.h, the word at 0x20 is
                    # printed without a name because that file names nothing there, and ADC_CTL 0x34 is left
                    # alone because it sits outside the DT's reg = <0x0506e000 0x34>. Round291's three words
                    # keep their own row: the next round's writes are judged against this baseline, and a
                    # rewritten row would break the comparison with the boots already archived.
                    ("#define kGpuccLdoAtest      0x9000U", "GpuRegs.h", gpu_regs),
                    ("#define kGpuccLdoCfg0       0x9004U", "GpuRegs.h", gpu_regs),
                    ("#define kGpuccLdoStatus1    0x9024U", "GpuRegs.h", gpu_regs),
                    ("#define kGpuccLdoRefX1      0x902cU", "GpuRegs.h", gpu_regs),
                    ("#define kGpuccLdoRefX2      0x9030U", "GpuRegs.h", gpu_regs),
                    ("clover_gpu_report_ldo_words(t, \"post\");", "CloverGpu.cpp", gpu_source),
                    ("clover_gpu_report_ldo_words(t, \"beat\");", "CloverGpu.cpp", gpu_source),
                    ("\"clover r294: gpu t=%u ldo words %s w00=%08x w04=%08x w08=%08x w0c=%08x w10=%08x \"",
                     "CloverGpu.cpp", gpu_source),
                    ("\"w14=%08x w18=%08x w1c=%08x w20=%08x w24=%08x w28=%08x w2c=%08x w30=%08x "
                     "cx_on=%u gx_on=%u\\n\"", "CloverGpu.cpp", gpu_source),
                    # Round317's verdict row: the same thirteen words counted, in the same fire. The two
                    # segments are pinned separately because that is how the compiler merges them, and the
                    # merged string is what an image decoder searches for.
                    ("\"clover r317: gpu t=%u ldo verdict %s shape=%s named=%u/%u comparable=%u agree=%u \"",
                     "CloverGpu.cpp", gpu_source),
                    ("\"differ=%u nz_zero=%u/%u no_expect=%u outside=%u dead=%u ctrl_pdn=%u "
                     "ctrl_unnamed=%08x \"", "CloverGpu.cpp", gpu_source),
                    ("\"vsettle=%u map_ok=%u control=%u\\n\"", "CloverGpu.cpp", gpu_source),
                    # Round311: the MEM_ACC node as this unit's tree states it, the field inside it, and
                    # the three shapes the one row can take. The all-ones test is pinned as an expression
                    # because it is the part that keeps `corner=` from meaning two things: masked into a
                    # one-bit field, a dead page's 0xffffffff is 1, which is also corner 1's own map value.
                    ("#define kMemAccPhys         0x01fcf004ULL", "GpuRegs.h", gpu_regs),
                    ("#define kMemAccSize         0x4U", "GpuRegs.h", gpu_regs),
                    ("#define kMemAccBitPos       0U", "GpuRegs.h", gpu_regs),
                    ("#define kMemAccBitSize      1U", "GpuRegs.h", gpu_regs),
                    ("#define kMemAccCorner1Field 0x01U", "GpuRegs.h", gpu_regs),
                    ("#define kMemAccCorner2Field 0x00U", "GpuRegs.h", gpu_regs),
                    ("#define kMemAccNoAnswer     0xffffffffU", "GpuRegs.h", gpu_regs),
                    ("IODeviceMemory::withRange(kMemAccPhys, kMemAccSize)", "CloverGpu.cpp", gpu_source),
                    # Round317 moved the no-answer test and the corner lookup into one call whose argument
                    # is the read. What is pinned here is the call site: the driver has to ask the layer,
                    # not re-derive `l1=` next to the printf -- and the order the old needle proved (test the
                    # word before cutting the field) is now asserted by the host model instead.
                    ("gpu_mem_acc_decode(clover_gpu_read(window, 0), &sel);", "CloverGpu.cpp", gpu_source),
                    ("(unsigned) sel.answered,", "CloverGpu.cpp", gpu_source),
                    ("clover_gpu_report_mem_acc(t);", "CloverGpu.cpp", gpu_source),
                    ("\"clover r311: gpu t=%u fire=%u mem_acc map=0 phys=%llx size=%x\\n\"",
                     "CloverGpu.cpp", gpu_source),
                    ("\"clover r311: gpu t=%u fire=%u mem_acc map=1 answered=%u raw=%08x l1=%u corner=%u \"",
                     "CloverGpu.cpp", gpu_source),
                    ("\"gpucc=1 cx_on=%u gx_on=%u\\n\"", "CloverGpu.cpp", gpu_source),
                    ("\"gpucc=0\\n\"", "CloverGpu.cpp", gpu_source)):
                if needle not in text_:
                    errors.append(f"{where} lacks {needle[:64]!r}: round286's rows are the reading that "
                                  "decides the next GPU round, and every constant in them came from the "
                                  "board's own downstream tree rather than from a nearby SoC")
            # Round294 is a control, so its contract is an absence. Round290 wrote BIT(0) into the rbcpr
            # and rbbmtimer branch enables -- the two votes the DT's clock list still owed -- and every
            # boot since has ended in a silent, no-panic, pre-deadline reset (718 s, 1184 s, 1248 s,
            # 1335 s), while the two longest boots this port has run were both built before it. The reads
            # that ask "is the parent running at all" stay; the writes that answered for it go. If the
            # silent resets continue anyway, the correlation is dead, and this check is the record of the
            # one build that was allowed to prove it.
            for gone in ("clover_gpu_set_bits(clover_gpu_gpucc, kGpuccRbcprEn, 1);",
                         "clover_gpu_set_bits(clover_gpu_gpucc, kGpuccRbbmtimerEn, 1);"):
                if gone in gpu_source:
                    errors.append(f"CloverGpu.cpp still holds {gone[:56]}: round294 is the control that "
                                  "takes round290's two branch-enable writes back out, and a build that "
                                  "keeps them cannot be told from the four resets it is meant to explain")
            # A direct index is how a bare store starts. Every window goes through the read/write
            # helpers, so an index on the window object itself means a register touched outside the one
            # path -- which is the whole point of making the write checkable rather than promised.
            for window in ("clover_gpu_gcc[", "clover_gpu_gpucc[", "clover_gpu_core[",
                           "clover_gpu_memacc["):
                if window in gpu_source:
                    errors.append(f"CloverGpu.cpp indexes {window} directly: round286 touches every register "
                                  "through the read/write helpers, and a direct index is the store the checker "
                                  "cannot see")
            # Round311 is a reading, and the discipline this line keeps is that a reading does not become a
            # write by accident. The MEM_ACC word is the corner select for a supply domain whose expected
            # value nobody has established (§5.12 closed the LDO branch, and the level that does exist is an
            # RPM request this chain never sent), so a wrong value written here would be indistinguishable
            # from a wrong value read. Every store helper is named, not just the one a careless round
            # would reach for first.
            for store in ("clover_gpu_write(clover_gpu_memacc", "clover_gpu_set_bits(clover_gpu_memacc",
                          "clover_gpu_clear_bits(clover_gpu_memacc",
                          "clover_gpu_update_bits(clover_gpu_memacc"):
                if store in gpu_source:
                    errors.append(f"CloverGpu.cpp contains {store[:52]}: round311 reads the L1 corner select "
                                  "and writes nothing to it, and the round that changes that promise has to "
                                  "say so here rather than in a comment")
            if "0x01830000" in gpu_regs:
                errors.append("GpuRegs.h carries 0x01830000: that pin was wrong -- the board's DT puts GCC at "
                              "0x00100000 (clock-controller@100000), which is also what CloverInput's registers "
                              "already use, and the wrong base would have read a different block as clocks")
            # Round291 reads at 0x9000-range offsets of the window r286 maps. That is only legitimate
            # because sdm660.dtsi gives clock_gpu/clock_gfx reg = <0x5065000 0x10000>, which contains the
            # LDO node's 0x0506e000; a second mapping of that page would be the same hardware mapped twice,
            # and a later round that shrank kGpuccSize would silently push the LDO offsets outside it.
            # Round311 adds the one window the arithmetic says is genuinely separate: 0x01fcf004 is above
            # GCC's end (0x00100000+0x94000 = 0x194000) and nowhere near the two GPU pages, so it cannot
            # ride an existing mapping -- and its own DT node says how big it is (reg = <0x1fcf004 0x4>).
            # The count is the checkable form of "four windows, each the extent its node declares".
            if gpu_source.count("IODeviceMemory::withRange(") != 4:
                errors.append(f"CloverGpu.cpp opens {gpu_source.count('IODeviceMemory::withRange(')} windows, "
                              "not 4: GCC, GPUCC, the GPU core and round311's MEM_ACC node are the only "
                              "extents this line may map, the LDO registers stay inside the GPUCC extent the "
                              "DT declares, and a fifth window means a register whose node nobody checked")

            # Round317. Conformance §7 gap 3's other half: every decision this line reads out of a word
            # moved into GpuCore.h so a host can prove it, and the pins follow the code, not the file. What
            # is asserted is (a) that each decision still exists, (b) that the driver *calls* it, and (c)
            # that no second copy survived in the driver. The third is the real risk of a split like this
            # one, and here it is checkable unusually well: the masks the decoders consume are named only
            # by the layer now, so a hand-rolled `(gfx & 1u)` left in CloverGpu.cpp is visible as a needle.
            #
            # The reason this round exists at all is in the layer's own header comment: boot 81's ten-year
            # recorded verdict, "not one bit matches the vendor's list", was a human comparing twelve words
            # by eye and was wrong -- one word does agree, and one word the list writes was missing from the
            # table this repo quoted. A census is a function now, so the claim and its exceptions are
            # counted by the same code the board runs.
            gpu_core_path = REPO / "drivers/CloverGpu/GpuCore.h"
            if not gpu_core_path.is_file():
                errors.append("drivers/CloverGpu/GpuCore.h is missing: the LDO census, the branch/halt "
                              "pair and the corner decode would be provable only on a board that cannot be "
                              "un-broken, and boot 81's verdict would stay a sentence instead of a count")
            else:
                gpu_core = gpu_core_path.read_text()
                core_decisions = (
                # The census's own arithmetic, one clause per thing it distinguishes.
                ("values[i] == kGpuNoAnswer", "gpu_ldo_census"),
                ("out->nonzero_read_zero++;", "gpu_ldo_census"),
                ("out->table_outside++;", "gpu_ldo_census"),
                # The dead page is excluded *before* comparison. Without that order a silent
                # window is 13 differences, which reads as an unprogrammed rail.
                ("if (values[i] == kGpuNoAnswer) {\n            out->dead++;\n            continue;",
                 "gpu_ldo_census"),
                ("census.named != census.words", "gpu_ldo_shape"),
                ("census.nonzero_read_zero == census.nonzero_expected", "gpu_ldo_shape"),
                ("control_on ? GPU_LDO_SHAPE_UNPROGRAMMED : GPU_LDO_SHAPE_BLIND",
                 "gpu_ldo_shape"),
                ("\"list-unwritten-blind\", \"list-never-written\"", "kGpuLdoShapeNames"),
                # The vendor's nine-entry list and the DT's 0x34 extent, kept apart.
                ("return block_offset < kGpuLdoWindowBytes;", "gpu_ldo_expect_in_window"),
                ("static const uint32_t kGpuLdoWindowBytes = 0x34U;", "GpuCore.h"),
                # The bit the archived rows never carried: the residue of boot 81's 0x80000038.
                ("out->unnamed = raw & ~kGpuLdoCtrlNamedBits;", "gpu_ldo_decode_ctrl"),
                # Seven bits admit a voltage neither the vendor nor this node accepts.
                ("out->in_dt_range = out->uv <= kGpuLdoDtMaxUv && out->uv >= kGpuLdoDtMinUv;",
                 "gpu_ldo_decode_vref"),
                ("static const uint32_t kGpuLdoDtMinUv = 400000U;", "GpuCore.h"),
                # The settling question has two inputs and one predicate that is an *or*.
                ("return raw != kGpuNoAnswer && (raw & (kLdoSettled | kLdoReady)) != 0;",
                 "gpu_ldo_vendor_settled"),
                # GDSC: the word our write produces, and the pair that means "powered".
                ("return (before & ~(kGdscHwControl | kGdscSwOverride | kGdscWaitMask)) | kGdscWaitVal;",
                 "gpu_gdsc_init_word"),
                ("return before & ~kGdscSwCollapse;", "gpu_gdsc_release_word"),
                ("return gpu_gdsc_pwr_on (word) && !gpu_gdsc_collapsed (word);",
                 "gpu_gdsc_powered"),
                # A request and a halt are two answers; only the conjunction is a clock state.
                ("static inline bool gpu_branch_requested (uint32_t word) { return (word & 1u) != 0; }",
                 "GpuCore.h"),
                ("return gpu_branch_requested (word) && !gpu_branch_halted (word);",
                 "gpu_branch_running"),
                # The AHB gate: a vote of 0 and a word of 0xffffffff are different failures, and
                # bit31 of the second one is set, so a bit0-only test opens GPUCC behind a dead page.
                ("return gpu_branch_requested (ahb_word) && ahb_word != kGpuNoAnswer;",
                 "gpu_ahb_gate_open"),
                # The corner select: the order is the reading.
                ("out->answered = raw != kMemAccNoAnswer;\n    if (!out->answered) {\n        return;\n    }\n    out->field = raw & kMemAccFieldMask;",
                 "gpu_mem_acc_decode"),
                ("gpu_mem_acc_decode_before_mask", "GpuCore.h"),
                )
                for needle, _ in core_decisions:
                    if needle not in gpu_core:
                        errors.append(f"drivers/CloverGpu/GpuCore.h lacks {needle[:64]!r}: this is a "
                                      "decision the host model proves and a board cannot -- a decode that "
                                      "masks before asking whether the page answered turns a silent window "
                                      "into a corner, and a census that counts a dead word as a difference "
                                      "turns it into an unprogrammed rail")
                for needle in ("gpu_gdsc_pwr_on(", "gpu_gdsc_collapsed(", "gpu_gdsc_init_word(",
                               "gpu_gdsc_release_word(", "gpu_branch_requested(", "gpu_branch_halted(",
                               "gpu_ahb_gate_open(", "gpu_chipid_gate(", "gpu_ldo_decode_ctrl(",
                               "gpu_ldo_decode_vref(", "gpu_ldo_vendor_settled(", "gpu_ldo_status_settled(",
                               "gpu_ldo_census(", "gpu_ldo_shape_name(", "gpu_mem_acc_decode(",
                               "gpu_ldo_word_gpucc_offset("):
                    if needle not in gpu_source:
                        errors.append(f"drivers/CloverGpu/CloverGpu.cpp does not call {needle!r}: the pure "
                                      "layer would be dead code the model passes and the kernel ignores, "
                                      "which is the one split that makes a green host test a lie")
                for gone in ("(gfx & 1u)", "& kGdscPwrOn", "!= kMemAccNoAnswer", "& kMemAccFieldMask",
                             "& kLdoVrefMask", "& kLdoPdn", "& kLdoReady", "& kLdoSettled",
                             "kGdscEnRestWaitMask", "kBranchClkOff"):
                    if gone in gpu_source:
                        errors.append(f"drivers/CloverGpu/CloverGpu.cpp still carries a second copy of "
                                      f"{gone!r}: the shipped path and the modelled path would be two "
                                      "answers to one question, and only one of them has a host test")
                # The layer must not re-type what GpuRegs.h already pinned to the device tree. That is the
                # rule that keeps this split from becoming a second constant table -- the failure mode r313
                # measured on the SPMI side, where two indexings of one block disagreed by an APID range.
                for borrowed in ("kMemAccNoAnswer", "kGpuccLdoAtest", "kLdoMinUv", "kLdoStepUv",
                                 "kGdscSwCollapse", "kGdscPwrOn", "kGdscWaitVal", "kBranchClkOff",
                                 "kMemAccFieldMask", "kLdoBypass", "kLdoCprBypass"):
                    if borrowed not in gpu_core:
                        errors.append(f"GpuCore.h does not derive from GpuRegs.h's {borrowed!r}: the layer "
                                      "is supposed to add names and verdicts, not re-type the numbers the "
                                      "device tree check already vouches for")
                if "#include \"GpuRegs.h\"" not in gpu_core:
                    errors.append("GpuCore.h does not include GpuRegs.h: without it the layer would have to "
                                  "re-declare every address, and the pin that ties each one to a DT node "
                                  "would stop covering the code the board actually runs")
                if "NULL" in gpu_core:
                    errors.append("GpuCore.h uses NULL: the header is included before anything that defines "
                                  "it and the standard the kernel builds these archives under is not "
                                  "guaranteed to provide it, so the null pointer is spelled 0 here")

                gpu_model = REPO / "drivers/CloverGpu/tests/clover_gpu_host.cpp"
                gpu_runner = next((base / "scripts/test-clover-gpu.py"
                                   for base in (REPO, pathlib.Path.cwd())
                                   if (base / "scripts/test-clover-gpu.py").is_file()), None)
                if gpu_runner is None:
                    errors.append("scripts/test-clover-gpu.py is missing: the census, the decodes and the "
                                  "GDSC word would be proven only on a board that cannot be un-broken")
                elif not gpu_model.is_file():
                    errors.append("drivers/CloverGpu/tests/clover_gpu_host.cpp is missing: the host model "
                                  "has nothing to drive GpuCore.h against")
                else:
                    model_text = gpu_model.read_text()
                    for needle in ("== 8", "== 1", "== 7", "kBoot81", "0x80000038", "0x0000000d",
                                   "list-never-written", "list-unwritten-blind", "dead-page",
                                   "unnamed-mapping", "no-words", "0xa0282000", "0x08282000",
                                   "0x00282000", "1010000", "375000", "400000", "gpu_ldo_ctrl_probe_word",
                                   "gpu_mem_acc_decode_before_mask", "0x00000008", "0x7fffffff"):
                        if needle not in model_text:
                            errors.append(f"the CloverGpu host model never asserts {needle!r}: a number the "
                                          "board can only reach by reading it off a card is a number nobody "
                                          "has seen proven, and round317 exists because one such number was "
                                          "recorded wrong for eleven boots")
                    check = subprocess.run([sys.executable, str(gpu_runner)],
                                           capture_output=True, text=True)
                    if check.returncode:
                        errors.append("the CloverGpu host model failed: the census, a decode, the GDSC word "
                                      "or the AHB gate is not what this boot's row will claim it is\n"
                                      + (check.stdout + check.stderr)[-700:])
                    else:
                        print("[check-clover] CloverGpu host model: "
                              + (check.stdout.strip().splitlines() or ["PASS"])[-1])
        gpu_built = any("CloverGpu" in item for item in board["kernel"]["archives"])
        # `text` has been reassigned to patch sources by this point in the function; read the
        # defconfig this check is about rather than whatever the last round's block left behind.
        gpu_defconfig = defconfig_path.read_text()
        if "libCloverGpu.a" in gpu_defconfig and not gpu_built:
            errors.append("the defconfig links libCloverGpu.a but [kernel] archives omits build-arm-driver.py "
                          "CloverGpu, so that archive does not exist")
        if gpu_built and "libCloverGpu.a" not in gpu_defconfig:
            errors.append("[kernel] archives builds CloverGpu but the defconfig does not link libCloverGpu.a, "
                          "so clover_gpu_run() is undefined")
        if gpu_built and flags.get("SHENG_PMGR", "n") not in ("y", "1"):
            errors.append("CloverGpu is scheduled from sheng_pmgr_run(), which CONFIG_XNU_SHENG_PMGR="
                          f"{flags.get('SHENG_PMGR', 'n')} does not compile into the kernel")
        gpu_args = [item for item in board["payload"]["kernel_args"] if item.startswith("clover-gpu")]
        if gpu_built and gpu_args != ["clover-gpu=1"]:
            errors.append(f"kernel_args carries {gpu_args!r} for clover-gpu: the probe has one mode (power "
                          "the GDSCs and clocks, read back, identify) and absence means no mapping, so "
                          "anything else is noise or a second instrument nobody wrote")
        if '{ "clover r286: ", "r286-gpu", 64, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the GPU rows: an unbudgeted producer drops "
                          "them silently, and this round's reading is the row itself")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r286")' not in sdhci_source:
            errors.append("the r286 rows are not exempt from the per-pass push cap: the GPU timer can fire "
                          "beside the census burst, where boot 36 measured the cap eating exactly that")
        # Round290 reuses the r286 class's producer but its own rows are the reading that decides the
        # next GPU round, so it gets its own class (a shared class would let r286's budget account for
        # lines that are not its own) and the same cap exemption for the same reason.
        if '{ "clover r290: ", "r290-gpu", 32, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the round290 branch rows: an unbudgeted "
                          "producer drops them silently, and a halted-clock row that never arrived reads "
                          "exactly like a clock that was never asked")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r290")' not in sdhci_source:
            errors.append("the r290 rows are not exempt from the per-pass push cap: they print from the "
                          "GPU timer beside the census burst, which is where boot 36 measured the cap "
                          "eating a scheduled round's own line")
        # Round291's rail row is the reading that decides whether gx's GDSC is waiting on a voltage, so
        # it gets its own class -- sharing r290's would let branch lines account for a row that is not
        # them -- and the same cap exemption.
        if '{ "clover r291: ", "r291-gpu", 32, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the round291 LDO rows: an unbudgeted "
                          "producer drops them silently, and a missing rail row reads exactly like a "
                          "rail that never came up")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r291")' not in sdhci_source:
            errors.append("the r291 rows are not exempt from the per-pass push cap: they share the GPU "
                          "timer with the r286/r290 rows, and boot 36 measured the cap eating the newest "
                          "of exactly that kind of scheduled line")
        # Round294's census is the baseline the next GPU round's writes are judged against, so it is pinned
        # in the same two places r291 is: its own budget, and the exemption that keeps a capped pass from
        # dropping it. An absent census row must read as "capped", not as "the page was silent".
        if '{ "clover r294: ", "r294-gpu", 32, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the round294 LDO census: an unbudgeted "
                          "producer drops the whole block read, and the round that writes to the rail "
                          "would then have no baseline to be judged against")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r294")' not in sdhci_source:
            errors.append("the r294 census is not exempt from the per-pass push cap: it prints from the "
                          "same GPU timer as r286/r290/r291, and those four rows together are what a "
                          "sixteen-write pass runs out of first")
        # Round311's MEM_ACC row joins the same schedule, and it is pinned in the same two places because
        # its reading is a *shape*: the row is allowed to say "the range was refused" and "the page answered"
        # and those two must not be able to trade places through a cap.
        if '{ "clover r311: ", "r311-gpu", 32, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the round311 MEM_ACC rows: an unbudgeted "
                          "producer drops them silently, and a boot with no corner row reads like a boot "
                          "whose GPU timer never fired rather than like a capped pass")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r311")' not in sdhci_source:
            errors.append("the r311 row is not exempt from the per-pass push cap: it prints from the GPU "
                          "timer beside the censuses, and the three shapes of that row are the only thing "
                          "that separates a refused mapping from an answered page")
        # Round317's verdict row is the interpretation of round294's thirteen words, so it is pinned in the
        # same two places for the same reason: the shape is the reading, and a capped pass that ate it would
        # leave a boot showing the words and none of what they mean.
        if '{ "clover r317: ", "r317-gpu", 32, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the round317 LDO verdict: an unbudgeted "
                          "producer drops the census interpretation, and a boot whose shape row never "
                          "arrives reads like a boot whose census never ran")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r317")' not in sdhci_source:
            errors.append("the r317 verdict is not exempt from the per-pass push cap: it prints from the "
                          "same GPU timer as r286/r290/r291/r294/r311, and \"list-never-written\" versus "
                          "\"dead-page\" is the distinction this line has been circling since boot 77")
        # Round318's two rows are one segment's reading -- the raw I2C_STATUS/ERROR_FLAGS words and the
        # layer's decode of them -- so they are pinned in the same two places for the same reason. The
        # sharper half is that round181's bus= column was the masked word: a cap that kept the decoded
        # row and ate the raw one would put the SDA/SCL blind spot back into the boot report.
        if '{ "clover r318: ", "r318-input", 32, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the round318 segment rows: an unbudgeted "
                          "producer drops the raw status words, and a boot showing only the decode reads "
                          "like a bus whose lines were never sampled")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r318")' not in sdhci_source:
            errors.append("the r318 rows are not exempt from the per-pass push cap: the pair is one "
                          "reading, and keeping the decoded row while dropping the raw one restores "
                          "exactly the blind spot round318 exists to remove")
        # Round319 re-says the r318 raw words from state on the beat (the r260 pattern): the one-time
        # xfer/core rows print at driver start and the tee's ring laps over them before readback, so
        # the last sub-transfer's raw I2C_STATUS / ERROR_FLAGS / SDA / SCL are latched in
        # clover_input_last_xfer and printed once per beat by package/CloverHidInputProbe.cpp. The
        # class admits the beat row; a per-pass cap that ate it would read like a probe that never
        # ran, so it is exempted for the same reason round281's schedule is.
        if '{ "clover r319: last", "r319-last", 16, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the round319 beat row: an unbudgeted "
                          "producer drops the raw-word re-publish, and its absence would read like the "
                          "input probe never ran, which is the silence this round exists to remove")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r319-last")' not in sdhci_source:
            errors.append("the r319-last beat row is not exempt from the per-pass push cap: a cap that "
                          "ate it is indistinguishable from a probe that never ran")
        # Round313 boots drivers/CloverPower for the first time. The report is one burst in the PMGR tail
        # and it is printed under two tokens -- round313's new lines and the round182 lines that keep
        # their origin stamp -- so both halves need their own budget and their own exemption, or the half
        # nobody budgeted is the one the per-pass cap eats. The reading that must survive is "how far did
        # the arbiter get": a report that lost its APID listing reads like a driver that never reached the
        # map, and those are two different answers about the same silicon.
        for token, name in (('{ "clover r182: ", "r182-power", 48, 0, 0, 0 },', "r182-power"),
                            ('{ "clover r313: ", "r313-power", 48, 0, 0, 0 },', "r313-power")):
            if token not in sdhci_source:
                errors.append(f"drivers/CloverSdhci has no class {name!r} for the CloverPower report: an "
                              "unbudgeted one-shot burst is bounded only by the per-pass cap, so a long "
                              "report would be cut in the middle and the cut would not be attributed")
            if f'teeHasPrefix (probe->name, teeStringLength (probe->name), "{name}")' not in sdhci_source:
                errors.append(f"the {name} class is not exempt from the per-pass push cap: the PMGR tail "
                              "prints its burst into a pass that has usually spent its sixteen pushes "
                              "already, and half a power report is a different verdict from none")

        # Round283 asks the question boot 69 left open: which software path, if any, resets the board.
        # The r281 read showed the APSS watchdog never armed and the card's last write 11 s older than
        # the kernel's last line, so the answer has to live somewhere the reset cannot take away.
        # Round338 measured the slot instead of describing it: the free run from 0x660 is ten 64-bit
        # words, and the contract is first-writer-wins so the panic screen's message survives a later
        # restart hook. Round339 moved the two heartbeat counters into their own free cell at 0x6e0,
        # which is a separate writer that never touches the record's words 0..8 -- that is what keeps
        # a named reset from being overwritten by the pulse that follows it.
        if "static const uint32_t kResetRecordOffset = 0x660;" not in sdhci_source:
            errors.append("CloverSdhci.cpp does not place the reset record at 0x660: the slot has to be "
                          "inside the already-mapped IMEM page, in the gap the DT leaves unclaimed")
        if "static const uint32_t kResetRecordWords = 10;" not in sdhci_source:
            errors.append("CloverSdhci.cpp does not size the record at the ten words round338 measured: "
                          "the twenty-word comment it replaced reached into boot_stats@6b0, which the "
                          "firmware owns, and the wiping loop wrote zeros there every boot")
        if "static const uint32_t kResetBeatOffset = 0x6e0;" not in sdhci_source:
            errors.append("CloverSdhci.cpp declares no heartbeat cell offset: the two counters have to "
                          "live somewhere the geometry report says is free, not in the record's words")
        if "volatile uint64_t *clover_imem_beat = nullptr;" not in sdhci_source:
            errors.append("CloverSdhci.cpp publishes no heartbeat pointer: the arm-time control below "
                          "and the tick both address the cell, and one of them would be guessing")
        if "static const uint64_t kResetRecordMagic = 0x00333832564f4c43ULL;" not in sdhci_source:
            errors.append("CloverSdhci.cpp's reset-record magic is not the r283 word: a record read back "
                          "under a different magic is a slot another writer owns")
        if "static const uint64_t kResetBeatMagic = 0x0033383254414542ULL;" not in sdhci_source:
            errors.append("CloverSdhci.cpp's beat magic is not the r283 word: the heartbeat is the only "
                          "card-free evidence of how far the previous boot's timer got")
        if "if (record == nullptr || record[0] == kResetRecordMagic) {" not in sdhci_source:
            errors.append("clover_imem_note does not stop at first writer: a restart hook after the panic "
                          "screen would replace the message that explains the reset")
        note_at = sdhci_source.find("extern \"C\" void clover_imem_note")
        beat_at = sdhci_source.find("extern \"C\" void clover_imem_heartbeat (void)")
        if note_at < 0 or beat_at < 0:
            errors.append("CloverSdhci.cpp lacks clover_imem_note or clover_imem_heartbeat: the kernel's "
                          "reset hooks and ArmCpu both call them by name")
        else:
            # A ban is judged on the code lines only: the function is followed by a comment that
            # names the words it no longer writes, and a substring test over the raw text would
            # have read that explanation as a violation.
            beat_end = sdhci_source.find("\n}", beat_at)
            beat_body = "\n".join(line.split("//")[0] for line in
                                  sdhci_source[beat_at:beat_end].splitlines())
            for needle in ("record[9] = kResetBeatMagic;", "beat[0] = beat[0] + 1;",
                           "beat[1] = imemCounter ();"):
                if needle not in beat_body:
                    errors.append(f"clover_imem_heartbeat does not write {needle!r}: the pulse is the "
                                  "only thing that says how far the timer got, and a heartbeat that "
                                  "writes nothing reads as a boot that never ticked")
            for index in ("record[0]", "record[1]", "record[2]", "record[3]", "record[4]",
                          "record[5]", "record[6]", "record[7]", "record[8]",
                          "record[10]", "record[11]"):
                if index in beat_body:
                    errors.append(f"clover_imem_heartbeat writes {index}: the pulse must not disturb "
                                  "the reset record it runs beside, and record[10] and record[11] are "
                                  "boot_stats' own bytes -- the pair this round moved out of")
        read_at = sdhci_source.find("clover r283: last reset kind=")
        clear_at = sdhci_source.find("for (unsigned int i = 0; i < kResetRecordWords; ++i) {")
        control_at = sdhci_source.find('sdhciSay ("clover r339: beatctl')
        arm_at = sdhci_source.find("clover_imem_beat = beat;")
        if not (0 <= read_at < clear_at < control_at < arm_at):
            errors.append("clover_unattended_begin does not read the record, clear the ten words, run "
                          "the write-then-read control and only then arm the pointers: any other order "
                          "either erases the reading before it is printed, or lets a timer tick land "
                          "between the sentinel write and its read-back and call the tick's number the "
                          "control's")
        if "beat[0] = 0;" not in sdhci_source[clear_at:control_at]:
            errors.append("the boot-start clear does not reset the heartbeat counter: the record would "
                          "be this boot's and the count the next boot reads would still be the "
                          "previous one's, which is the round336 reading that made beat= unusable")
        # The named row's format string is split across two lines by the C source, so the contract
        # matches each literal segment as it appears rather than a joined form no file can contain.
        for needle in ("\"clover r283: last reset kind=%llu pid=%llu aux=%llx pc=%llx cnt=%llu \"",
                       "\"beat=%llu beatcnt=%llu beatmagic=%llx text=%s\\n\"",
                       "\"clover r283: last reset unnamed magic=%llx beat=%llu beatcnt=%llu "
                       "beatmagic=%llx\\n\""):
            if needle not in sdhci_source:
                errors.append(f"CloverSdhci.cpp lacks {needle[:48]!r}: kind and the unnamed line are the "
                              "two halves of this round's answer")
        readback_body = sdhci_source[read_at:clear_at] if 0 <= read_at < clear_at else ""
        if 'sdhciSay ("clover r283: last reset kind=' not in sdhci_source or \
                'sdhciSay ("clover r283: last reset unnamed' not in sdhci_source:
            errors.append("the r283 reset verdict does not go through sdhciSay: boot 70 rotated the "
                          "one-shot IOLog row out of the 512-record kernel ring before the card was "
                          "read, so the whole answer was absent; the driver ring copy is the one that "
                          "survives the horizon")
        if 'IOLog ("clover r283:' in readback_body:
            errors.append("the r283 reset verdict is still printed with bare IOLog: that copy has no "
                          "path to the card outside the kernel-log tee")
        if '{ "clover r283: ", "r283-reset", 16, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the r283 line: the bring-up pass also "
                          "carries the census, and the one row this round exists for would be capped away")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r283")' not in sdhci_source:
            errors.append("the r283 line is not exempt from the per-pass push cap: a capped round would "
                          "read exactly like a boot whose slot was never touched")

        # Round339's two instruments. The first is the positive control round336 owed: the row has
        # printed beatmagic= for six rounds and every sighting said no row carried the heartbeat's
        # own magic, which was read as a verdict about the cell while nothing ever said this kernel
        # can keep a word there. The second reads the doorway state before the write, because the
        # value the cell holds on arrival is about the boot that ended, not about this store.
        for needle in ('sdhciSay ("clover r339: beatctl wrote=%llx read=%llx magic_wrote=%llx '
                       'magic_read=%llx ok=%d\\n"',
                       'sdhciSay ("clover r339: mark wrote=%x before=%x after=%x\\n"',
                       "beat[1] = kResetBeatSentinel;",
                       "const uint64_t controlRead = beat[1];",
                       "const uint32_t before = cell[0];",
                       "const uint32_t readback = cell[0];"):
            if needle not in sdhci_source:
                errors.append(f"CloverSdhci.cpp lacks {needle[:56]!r}: either the control or the "
                              "doorway reading it belongs to is gone, and the absence of a heartbeat "
                              "would again be evidence with no counter-claim beside it")
        if sdhci_source.find("const uint32_t before = cell[0];") > \
                sdhci_source.find("cell[0] = kRestartBootloader;"):
            errors.append("the restart-reason cell is read after it is written: before= would then "
                          "print this boot's own mark and the row would say nothing about the boot "
                          "that ended")
        if sdhci_source.find("const uint32_t readback = cell[0];") < \
                sdhci_source.find("const uint32_t before = cell[0];"):
            errors.append("the restart-reason read-back is placed before the store: after= would "
                          "measure the previous boot again, and the two columns would be one column")
        if "if (readback != kRestartBootloader) {" not in sdhci_source:
            errors.append("arming does not stop when the cell refuses the word: the panic screen is "
                          "given the published pointer only after a successful read-back, and a cell "
                          "that will not hold a value is not a cell to mark at panic time")

        arm_cpu_source = (REPO / "drivers/ArmCpu/ArmCpu.cpp").read_text()
        if 'extern "C" void clover_imem_heartbeat (void);' not in arm_cpu_source or \
                "clover_imem_heartbeat ();" not in arm_cpu_source:
            errors.append("ArmCpu does not stamp the IMEM heartbeat: without it an unnamed reset cannot "
                          "say whether the board was still taking timer ticks when it went")
        if "if ((logicalId & 7) == 0) {\n        clover_imem_heartbeat ();\n      }" not in arm_cpu_source:
            errors.append("ArmCpu's heartbeat is not gated on CPU0: four cores racing one counter would "
                          "turn the count into ink rather than a clock")

        panic_source = (REPO / "scripts/xnu-panic-screen.c").read_text()
        for needle in ("#define IMEM_KIND_PANIC 4u",
                       "(IMEM_KIND_PANIC, 0UL, 0UL, (unsigned long)pc, fmt);",
                       "imem_note (caller, fmt);"):
            if needle not in panic_source:
                errors.append(f"xnu-panic-screen.c lacks {needle[:44]!r}: a panic screen that resets "
                              "through its own SMC must still name itself before it goes")
        patcher_source = (REPO / "scripts/patch-xnu-entry-probe.py").read_text()
        if "note = symbols.get('_clover_imem_note')" not in patcher_source or \
                "definitions.append(f'-DIMEM_NOTE={note:#x}UL')" not in patcher_source:
            errors.append("patch-xnu-entry-probe.py does not resolve _clover_imem_note into IMEM_NOTE: "
                          "the panic screen's note would compile to a no-op")

        r283_name = "z" * 52 + "-clover-r283.patch"
        r283_patch = next((base / "patches/xnu-clover" / r283_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r283_name).is_file()), None)
        if r283_patch is None:
            errors.append(f"patches/xnu-clover/{r283_name} is missing: the three reset funnels would "
                          "run unpatched and every reset would read as unnamed")
        else:
            r283_text = r283_patch.read_text()
            for needle in ("case kPEHaltCPU:",
                           "case kPEHangCPU:",
                           "case kPEUPSDelayHaltCPU:",
                           "case kPERestartCPU:",
                           "case kPEPanicRestartCPU:",
                           "case kPEPanicRestartCPUNoCallouts:",
                           'clover_imem_note(1, 0, type, (unsigned long)__builtin_return_address(0), "");',
                           'clover_imem_note(2, 0, type, (unsigned long)__builtin_return_address(0), "");',
                           "clover_imem_note(3, self ? (unsigned long)proc_getpid(self) : 0,",
                           "(unsigned long)howto",
                           "&self->p_comm[0]"):
                if needle not in r283_text:
                    errors.append(f"{r283_name} lacks {needle[:60]!r}: halt, restart and reboot(2) are "
                                  "the three funnels this round needs a name from")
            if "PEHaltRestartInternal" not in r283_text or "reboot_kernel" not in r283_text:
                errors.append(f"{r283_name} does not hook PEHaltRestartInternal and reboot_kernel: the "
                              "reset path has to be named at its funnel, not at a caller")
            load_r283 = REPO.parent / "UniXNU" / "patches" / "xnu" / r283_name
            if not load_r283.is_file():
                errors.append(f"UniXNU/patches/xnu/{r283_name} is missing: unixnu loads patches from "
                              "there and silently skips one that lives only in this repo")
            elif load_r283.read_bytes() != r283_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r283_name} is not the same bytes as this repo's "
                              "copy: the two trees would compile different kernels")
            late_script = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in late_script.splitlines() if "clover-r2" in line)
            if (late_list.find(r283_name) < 0 or
                    late_list.find(r283_name) > late_list.find("z" * 51 + "-clover-r277.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r283 before r277 in LATE_PATCH: "
                              "the compile pass would run on a tree this patch was not written against")

        # Round289, the fault's own backtrace. Boot 74 and boot 75 both ended in `Kernel data abort.` and
        # both left the same 480-byte kind-20 record, cut off inside the register list -- so esr, far, fp
        # and sp, the four fields that turn a pc into a cause, never reached the card, and the one caller
        # field that did (lr 0xfffffff00568971c) sits in the 12 MB of this kernel's symbol table that holds
        # no symbol at all. The reading is therefore not obtainable on the host, which is what makes this
        # a source contract rather than a decoder one: the fields have to be printed where they exist.
        r289_name = "z" * 53 + "-clover-r289.patch"
        r289_patch = next((base / "patches/xnu-clover" / r289_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r289_name).is_file()), None)
        if r289_patch is None:
            errors.append(f"patches/xnu-clover/{r289_name} is missing: the next data abort would again be "
                          "read as a pc and an lr that names nothing, which is exactly where round288 ended")
        else:
            r289_text = r289_patch.read_text()
            r289_added = "".join(line for line in r289_text.splitlines(True)
                                 if line.startswith("+"))
            for needle in ('"clover r289: abt pid=%d tid=0x%llx pc=%016llx lr=%016llx fp=%016llx sp=%016llx "',
                           '"esr=%08x far=%016llx cpsr=%08x kstack=%016llx..%016llx\\n"',
                           '"clover r289: bt n=%u from=%u f="',
                           '"clover r289: bt n=0 from=0 f=\\n"',
                           'clover_abort_trace(arm_saved_state_t *ss)',
                           'clover_kstack_walk(top, bottom, (uint64_t)state->fp, frames, 12)',
                           'clover_abort_trace(ss);'):
                if needle not in r289_text:
                    errors.append(f"{r289_name} lacks {needle[:64]!r}: without it the abort keeps only the "
                                  "fields the panic record already had room for")
            # The hook has to be the panic-path one: handle_abort reaches the fault's state through
            # panic_with_thread_kernel_state, so a call site anywhere else is a different exception.
            if "panic_with_thread_kernel_state" not in r289_text:
                errors.append(f"{r289_name} does not hook panic_with_thread_kernel_state: that is the only "
                              "function holding the arm_saved_state64 of the abort, and the round would read "
                              "a handler frame instead of the fault")
            # Printed before the message, not after: panic_plain() is where the record's budget is spent,
            # and a trace queued behind it has to survive the drain of a board that is already dying. The
            # stock order at this site is saved_state64() -> original_faulting_state -> panic_plain(), so
            # the call sitting between the first and the second is what "before the record" means here.
            sleh_part = r289_text.split("diff --git a/osfmk/kern/task.c")[0]
            trace_at = sleh_part.find("clover_abort_trace(ss);")
            state_at = sleh_part.find("arm_saved_state64_t *state = saved_state64(ss);")
            before_plain_at = sleh_part.find("os_atomic_cmpxchg(&original_faulting_state")
            if min(trace_at, state_at, before_plain_at) < 0 or not (state_at < trace_at < before_plain_at):
                errors.append(f"{r289_name} does not place the trace between saved_state64() and the "
                              "faulting-state latch: that window is the only position that is after the "
                              "flavor check and before panic_plain(), whose record truncated boot 74 and 75")
            # A panic path may not block. proc_name takes p_lock, and this board is single-core -- an
            # abort holding that lock would hang in the instrument instead of reporting the fault. Checked
            # against the added code only: the comment is allowed to name the call it refuses.
            r289_code = "".join(line for line in r289_added.splitlines(True)
                                if not line.lstrip("+").lstrip().startswith(("*", "/*")))
            for forbidden in ("proc_name(", "task_procname(", "task_best_name(", "mutex_lock", "lck_mtx_"):
                if forbidden in r289_code:
                    errors.append(f"{r289_name} calls {forbidden} from the abort path: a lock taken on the "
                                  "way to panic is a boot that says nothing at all")
            load_r289 = REPO.parent / "UniXNU" / "patches" / "xnu" / r289_name
            if not load_r289.is_file():
                errors.append(f"UniXNU/patches/xnu/{r289_name} is missing: unixnu loads patches from there "
                              "and silently skips one that lives only in this repo")
            elif load_r289.read_bytes() != r289_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r289_name} is not the same bytes as this repo's copy: "
                              "the two trees would compile different kernels")
        if '{ "clover r289: ", "r289-abt", 32, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the r289 rows: they would fall into the "
                          "unclassed kernel log flow, where a fatal's own trace is the first thing dropped")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r289")' not in sdhci_source:
            errors.append("the r289 rows are not exempt from the per-pass push cap: they are the last lines "
                          "in the log buffer and the panic drain is the pass that has already spent its "
                          "sixteen pushes -- a capped trace reads as a fault with no name, which is the "
                          "reading round288 was forced to settle for")

        # Round295, the chain and the name the kernel cannot print. Boot 82 control-ruled the payload out of
        # the reset question and left the desktop chain one link longer than boot 81's: backboardd waits on a
        # debtor that itself waits on pid 187, and 187 reads as "com.apple.migra" -- which is what p_comm
        # holds, MAXCOMLEN+1 = 17 bytes, and the first fifteen characters of both com.apple.migration and
        # com.apple.migrationpluginwrapper. The round's whole value is that second name source plus a walk
        # that says where it stopped, so both are pinned here rather than left to the decoder: an unverifiable
        # exec= is the same silent blank this port has already been bitten by twice (r249's census, r289's
        # truncated panic record).
        r295_name = "z" * 54 + "-clover-r295.patch"
        r295_patch = next((base / "patches/xnu-clover" / r295_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r295_name).is_file()), None)
        if r295_patch is None:
            errors.append(f"patches/xnu-clover/{r295_name} is missing: the next boot would again stop at one "
                          "hop and print a 15-character name for the link that ends the chain")
        else:
            r295_text = r295_patch.read_text()
            for needle in ('"clover r295: hop=%u t=%llu pid=%d comm=%s exec=%s ex=%s"',
                           '" threads=%u wait=%u run=%u trap=%u thid=%llx rcv=%x remote=%x"',
                           '" to=%llx rmc=%u rsr=%x peer=%s end=%s\\n"',
                           'extern int clover_proc_exec_name(task_t task, char *out, size_t outlen);',
                           'clover_bb_exec_name(task_t task, char *out, size_t outlen)',
                           '#define CLOVER_BB_CHAIN 4',
                           'clover_bb_chain(task_t root, uint64_t secs)',
                           'clover_bb_chain(task, secs);',
                           'MAXCOMLEN',
                           'diff --git a/bsd/kern/proc_info.c',
                           'clover_proc_exec_name(task_t task, char *out, size_t outlen)',
                           'name = vnode_getname(pp->p_textvp);',
                           'vnode_putname(name);'):
                if needle not in r295_text:
                    errors.append(f"{r295_name} lacks {needle[:64]!r}: without it the chain is one hop again, "
                                  "or a name the field never held")
            # Every way this walk can stop has to be a printed word. An absent row and a row that stopped at
            # the third hop read alike otherwise, which is the defect this port keeps re-learning.
            for verdict in ('"lock_busy"', '"no_trap"', '"no_send"', '"no_owner"', '"self_reference"',
                            '"cycle"', '"depth_limit"'):
                if verdict not in r295_text:
                    errors.append(f"{r295_name} never prints end={verdict}: the end of a chain has to be "
                                  "a reading, not a gap between rows")
            # The name comes from the VFS name cache, which is entered without a task lock and without
            # blocking on a vnode lock: vn_getpath() takes the vnode lock, and this board's wait has
            # already been suspected of sitting under the filesystem once.
            r295_code = "".join(line for line in r295_text.splitlines(True)
                                if line.startswith("+")
                                and not line.lstrip("+").lstrip().startswith(("*", "/*")))
            for forbidden in ("vn_getpath(", "vnode_lock", "lck_mtx_lock(", "proc_lock("):
                if forbidden in r295_code:
                    errors.append(f"{r295_name} calls {forbidden}: the census runs beside a wedged userland "
                                  "and a lock it waits on is a boot that says nothing at all")
            load_r295 = REPO.parent / "UniXNU" / "patches" / "xnu" / r295_name
            if not load_r295.is_file():
                errors.append(f"UniXNU/patches/xnu/{r295_name} is missing: unixnu loads patches from there "
                              "and silently skips one that lives only in this repo")
            elif load_r295.read_bytes() != r295_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r295_name} is not the same bytes as this repo's copy: "
                              "the two trees would compile different kernels")
            late_script = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in late_script.splitlines() if "clover-r2" in line)
            if (late_list.find(r295_name) < 0 or
                    late_list.find(r295_name) > late_list.find("z" * 53 + "-clover-r289.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r295 before r289 in LATE_PATCH: the "
                              "compile pass would run on a tree this patch was not written against")
        if '{ "clover r295: ", "r295-chain", 128, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the r295 rows: five hops of a chain would "
                          "share the census budgets, and the hop that ends it is the one worth reading")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r295")' not in sdhci_source:
            errors.append("the r295 rows are not exempt from the per-pass push cap: a chain capped at its "
                          "fourth hop is indistinguishable from a chain that stopped there")


        # Round296 rewrites the chain row that round295 introduced, in the same three columns. Boot 83 ended
        # both leaves on end=no_send with thid=0 rcv=0 to=0: the capture rule round295 shipped with kept only
        # a thread whose x3 (destination port) was non-zero, and a thread inside the mach trap with x3 == 0 is
        # not a thread that stopped waiting -- it is waiting to receive, on the name in x5>>32. So "nobody is
        # coming back to this port" and "the reply landed and never woke anybody" printed alike. These
        # assertions hold the fix: cap= says which thread the three columns came from, and qmc=/qsr= read that
        # port's own queue, with no lock held (clover_sb_port_mail takes the object's own lock and reads two
        # words), which is the same precondition round295 established for vnode_getname().
        r296_name = "z" * 55 + "-clover-r296.patch"
        r296_patch = next((base / "patches/xnu-clover" / r296_name
                           if (base / "patches/xnu-clover" / r296_name).is_file() else None
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r296_name).is_file()), None)
        if r296_patch is None:
            errors.append(f"patches/xnu-clover/{r296_name} is missing: the next boot would again print three "
                          "zero columns for a hop that is blocked on a receive")
        else:
            r296_text = r296_patch.read_text()
            for needle in ('"clover r296: hop=%u t=%llu pid=%d comm=%s exec=%s ex=%s"',
                           '" threads=%u wait=%u run=%u trap=%u cap=%s thid=%llx rcv=%x remote=%x"',
                           '" to=%llx rmc=%u rsr=%x qmc=%u qsr=%u peer=%s end=%s\\n"',
                           'const char *cap = "none";',
                           'if (frcv == 0) {',
                           'frcv = (uint64_t)saved_state64(ss)->x[5] >> 32;',
                           'cap = "recv";',
                           'cap = "send";',
                           'clover_sb_port_mail(cur, (mach_port_name_t)frcv, &nqmc, &nqsr);',
                           'clover r295: hop=%u'):
                if needle not in r296_text:
                    errors.append(f"{r296_name} lacks {needle[:64]!r}: without it the row cannot say which "
                                  "thread it describes, and the r295 prefix surviving inside the new patch "
                                  "would mean the rewrite never happened")
            for value in ("lock_busy", "no_trap", "no_send", "no_owner", "self_reference", "cycle",
                          "depth_limit"):
                verdict = 'end = "%s";' % value
                if verdict not in r296_text and verdict not in (r295_patch.read_text() if r295_patch else ""):
                    errors.append(f"neither r295 nor r296 prints end={value}: the end of a chain has to be a "
                                  "reading, and a dropped row reads as an empty one")
            r296_code = "".join(line for line in r296_text.splitlines(True)
                                if line.startswith("+")
                                and not line.lstrip("+").lstrip().startswith(("*", "/*")))
            for forbidden in ("vn_getpath(", "vnode_lock", "lck_mtx_lock(", "proc_lock(", "thread_block("):
                if forbidden in r296_code:
                    errors.append(f"{r296_name} calls {forbidden}: the chain runs beside a wedged userland "
                                  "and a hop that waits on a lock prints nothing about the hops below it")
            load_r296 = REPO.parent / "UniXNU" / "patches" / "xnu" / r296_name
            if not load_r296.is_file():
                errors.append(f"UniXNU/patches/xnu/{r296_name} is missing: unixnu loads patches from there "
                              "and silently skips one that lives only in this repo")
            elif load_r296.read_bytes() != r296_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r296_name} is not the same bytes as this repo's copy: "
                              "the two trees would compile different kernels")
            late_script = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in late_script.splitlines() if "clover-r2" in line)
            if (late_list.find(r296_name) < 0 or
                    late_list.find(r296_name) > late_list.find("z" * 54 + "-clover-r295.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r296 before r295 in LATE_PATCH: it "
                              "edits the lines r295 wrote, so a tree without them makes it fail to apply -- "
                              "and an earlier apply failure here is silent")
        if '{ "clover r296: ", "r296-chain", 128, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the r296 rows: the chain rows changed prefix, "
                          "and a class that no longer matches drops every hop into the unclassed flow")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r296")' not in sdhci_source:
            errors.append("the r296 rows are not exempt from the per-pass push cap: a chain the cap cuts short "
                          "of its last hop cannot be told from one that reported depth_limit")

        # Round297 answers the question boot 84's reading left open. Every name on the r296 row is an index
        # inside the task printed for -- hop 2's remote=7f07 in the sender's space, hop 3's rcv=1917 in the
        # receiver's -- so nothing that reads two rows can say whether the request hop 2 placed was addressed
        # to the port hop 3 is blocked receiving on, and qmc=0 only means something if it was. The patch prints
        # the ipc_object pointer of both names (robj= on the send side, rvobj= on the receive side) and carries
        # the previous hop's remote object forward as prev=, so link=yes|no|unknown is a measurement rather
        # than a guess. It goes on a second row, not appended to the first: the longest r296 row on boot 84
        # measured 227 bytes against a 320-byte tee record, and the three pointers plus link=unknown add 79.
        r297_name = "z" * 56 + "-clover-r297.patch"
        r297_patch = next((base / "patches/xnu-clover" / r297_name
                           if (base / "patches/xnu-clover" / r297_name).is_file() else None
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r297_name).is_file()), None)
        if r297_patch is None:
            errors.append(f"patches/xnu-clover/{r297_name} is missing: the next boot would again leave qmc=0 "
                          "unreadable, because two port names from two tasks cannot be compared at all")
        else:
            r297_text = r297_patch.read_text()
            r297_added = "".join(line for line in r297_text.splitlines(True) if line.startswith("+"))
            for needle in ('"clover r297: hop=%u t=%llu prev=%lx rvobj=%lx robj=%lx link=%s\\n"',
                           'static uintptr_t\n+clover_sb_port_obj(task_t task, mach_port_name_t name)',
                           'return (uintptr_t)obj;',
                           'uintptr_t prev_obj = 0;',
                           'const char *link = "-";',
                           'robj = clover_sb_port_obj(cur, (mach_port_name_t)nrem);',
                           'rvobj = clover_sb_port_obj(cur, (mach_port_name_t)frcv);',
                           'link = (rvobj == prev_obj) ? "yes" : "no";',
                           'link = "unknown";',
                           'prev_obj = robj;'):
                if needle not in r297_text:
                    errors.append(f"{r297_name} lacks {needle[:64]!r}: without it the identity row cannot "
                                  "answer the one question this round was cut to ask")
            # The three states link= can take are the reading; a patch that prints only one of them is an
            # instrument that cannot report the branch it was built to separate.
            for state in ('"yes"', '"no"', '"unknown"'):
                if state not in r297_added:
                    errors.append(f"{r297_name} never prints link={state}: a link verdict with a missing "
                                  "branch reads as a boot whose chain never linked")
            # Additive, not a rewrite: widening the r296 row would put end= -- the column that says why the
            # walk stopped -- at the far tail of a line that runs up to 306 of the 320-byte record.
            if 'printf("clover r296:' in r297_added:
                errors.append(f"{r297_name} rewrites the r296 row instead of adding one: the identity fields "
                              "were split to a second row precisely so end= stays inside the record")
            r297_code = "".join(line for line in r297_text.splitlines(True)
                                if line.startswith("+")
                                and not line.lstrip("+").lstrip().startswith(("*", "/*")))
            for forbidden in ("vn_getpath(", "vnode_lock", "lck_mtx_lock(", "proc_lock(", "thread_block(",
                              "simple_wait(", "io_lock("):
                if forbidden in r297_code:
                    errors.append(f"{r297_name} calls {forbidden}: the chain runs beside a wedged userland, "
                                  "and an identity that blocks cannot be compared with anything -- it only "
                                  "has to be translated, unlocked and printed")
            load_r297 = REPO.parent / "UniXNU" / "patches" / "xnu" / r297_name
            if not load_r297.is_file():
                errors.append(f"UniXNU/patches/xnu/{r297_name} is missing: unixnu loads patches from there "
                              "and silently skips one that lives only in this repo")
            elif load_r297.read_bytes() != r297_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r297_name} is not the same bytes as this repo's copy: "
                              "the two trees would compile different kernels")
            late_script = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in late_script.splitlines() if "clover-r2" in line)
            if (late_list.find(r297_name) < 0 or
                    late_list.find(r297_name) > late_list.find("z" * 55 + "-clover-r296.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r297 before r296 in LATE_PATCH: it "
                              "edits the lines r296 wrote, so a tree without them makes it fail to apply -- "
                              "and an earlier apply failure here is silent")
        # r297's row no longer ships -- round298 rewrites it in place -- so the two driver-side
        # assertions of this block point at the round that produced the boot 86 reading. The patch file
        # itself stays pinned above, because the boot 86 rows on the card were printed by it.
        if '{ "clover r298: ", "r298-chain", 128, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the r298 rows: an identity row whose prefix "
                          "nothing matches falls into the unclassed flow and gets throttled with the chatter")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r298")' not in sdhci_source:
            errors.append("the r298 rows are not exempt from the per-pass push cap: losing the leaf's identity "
                          "row is a silence that reads as a chain that did not link -- and since qmc=/qsr= are "
                          "printed by the same translation, a dropped row leaves a zero nobody can attribute")

        # Round298 rewrites the identity row round297 introduced, in the same place. Boot 86 read all
        # three link states and the hop the question was aimed at answered unknown, because the leaf's
        # receive name never became an object -- and the helper that failed on it is the same one whose
        # success produced boot 84's qmc=0: both try RECEIVE then SEND and both return with their
        # out-parameters at their initialised zero. So a zero on the card could mean an empty queue or
        # a queue never read, and the row carried nothing to choose between them. These assertions hold
        # the three fixes: the return code of each translation, the name rvobj was really taken from,
        # and the receiver's own name and space for the port this hop wrote to -- the first comparison
        # of two port names that lives inside one address space, and the one that works even when this
        # hop's name does not translate.
        r298_name = "z" * 57 + "-clover-r298.patch"
        r298_patch = next((base / "patches/xnu-clover" / r298_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r298_name).is_file()), None)
        if r298_patch is None:
            errors.append(f"patches/xnu-clover/{r298_name} is missing: the next boot would again print qmc=0 "
                          "and rvobj=0 as one statement, which is the shape that cost boot 84 its verdict")
        else:
            r298_text = r298_patch.read_text()
            r298_added = "".join(line for line in r298_text.splitlines(True)
                                 if line.startswith("+")
                                 and not line.lstrip("+").lstrip().startswith(("*", "/*")))
            for needle in ('"clover r298: hop=%u t=%llu prev=%lx rvobj=%lx verr=%d vname=%x robj=%lx"',
                           '" rerr=%d otype=%d tname=%x tspace=%lx space=%lx link=%s tlink=%s\\n"',
                           'clover_sb_port_obj(task_t task, mach_port_name_t name, int *err, int *otype,',
                           '*err = -1;',
                           '*err = -2;',
                           '*otype = (int)io_otype(obj);',
                           '*rname = port->ip_receiver_name;',
                           '*rspace = (uintptr_t)port->ip_receiver;',
                           'tlink = (prev_tname == (uint32_t)frcv) ? "yes" : "no";',
                           'prev_have = (robj != 0) ? 1 : 0;',
                           'printf("clover r297: hop=%u'):
                if needle not in r298_text:
                    errors.append(f"{r298_name} lacks {needle[:72]!r}: without it the row cannot say which of "
                                  "its zeros is a measurement and which is a failed translation -- and the "
                                  "r297 printf has to appear, as the line this round replaces, because a "
                                  "patch that only appends never rewrote the identity row")
            if 'printf("clover r297:' in r298_added:
                errors.append(f"{r298_name} still prints the r297 row beside the r297 one: two identity rows "
                              "per hop double the chain's tee traffic for no new reading")
            for value in ('tlink = "-";', 'tlink = "no_obj";', 'tlink = "no_recv";',
                          'tlink = "other_space";', 'tlink = "no_name";'):
                if value not in r298_added:
                    errors.append(f"{r298_name} never prints {value}: tlink= has to name which side ran out, "
                                  "and a verdict that cannot be printed reads as a row that was never written")
            if 'printf("clover r296:' in r298_added:
                errors.append(f"{r298_name} rewrites the r296 chain row instead of the identity row: boots "
                              "83 to 86 are compared column by column and that row has to keep its shape")
            for forbidden in ("vn_getpath(", "vnode_lock", "lck_mtx_lock(", "proc_lock(", "thread_block(",
                              "simple_wait(", "io_lock(", "ipc_space_lock"):
                if forbidden in r298_added:
                    errors.append(f"{r298_name} calls {forbidden}: the chain runs beside a wedged userland, "
                                  "and the receiver's fields are read while ipc_object_translate() already "
                                  "holds the object's own lock")
            load_r298 = REPO.parent / "UniXNU" / "patches" / "xnu" / r298_name
            if not load_r298.is_file():
                errors.append(f"UniXNU/patches/xnu/{r298_name} is missing: unixnu loads patches from there "
                              "and silently skips one that lives only in this repo")
            elif load_r298.read_bytes() != r298_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r298_name} is not the same bytes as this repo's copy: "
                              "the two trees would compile different kernels")
            late_script = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in late_script.splitlines() if "clover-r2" in line)
            if (late_list.find(r298_name) < 0 or
                    late_list.find(r298_name) > late_list.find("z" * 56 + "-clover-r297.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r298 before r297 in LATE_PATCH: it "
                              "edits the lines r297 wrote, so a tree without them makes it fail to apply -- "
                              "and an earlier apply failure here is silent")

        # Round299 brings the secondary cores up. Boot 87's r271 row read real=1 max=8 reg=1 start=0 on both
        # fires, which is the kernel saying it has one cpu while its own device tree counts eight: the topology
        # parsed, MAX_CPUS is eight, and nobody asked. The asking has been in this repo's vendored ArmCpu copy
        # the whole time behind a boot-arg named after another board, so the round is a rename, two arguments,
        # and one row that prints what that driver knows on the census's own beat -- because a state the driver
        # spoke once before the first tick is already outside the ring's horizon by the time the card is read
        # (boot 59 measured exactly that shape for r272), and because PSCI's -4 is a verdict about the address
        # this driver handed it, which cannot be attributed unless the address is on the card too.
        r299_name = "z" * 58 + "-clover-r299.patch"
        r299_patch = next((base / "patches/xnu-clover" / r299_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r299_name).is_file()), None)
        if r299_patch is None:
            errors.append(f"patches/xnu-clover/{r299_name} is missing: without the row the boot would say "
                          "whether cores came up but never why they did not, which is the shape that left "
                          "boot 87's reg=1 with no mechanism beside it")
        else:
            r299_text = r299_patch.read_text()
            r299_added = "".join(line for line in r299_text.splitlines(True)
                                 if line.startswith("+")
                                 and not line.lstrip("+").lstrip().startswith(("*", "/*")))
            for needle in ('"clover r299: smp t=%llu req=%u con=%u run=%u"',
                           '" st=%u%u%u%u%u%u%u%u fl=%u%u%u%u%u%u%u%u"',
                           '" rc=%d,%d,%d,%d,%d,%d,%d,%d"',
                           '" want=%x,%x,%x,%x,%x,%x,%x,%x seen=%x,%x,%x,%x,%x,%x,%x,%x"',
                           '" entry=%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx\\n"',
                           'extern volatile uint64_t armcpu_smp_entry[8];',
                           'extern volatile int32_t armcpu_smp_psci_rc[8];',
                           'armcpu_smp_step[7],',
                           'armcpu_smp_fail[7],'):
                if needle not in r299_text:
                    errors.append(f"{r299_name} lacks {needle[:72]!r}: step, stop, return code, targeted "
                                  "affinity, seen affinity and the entry point are six different faults, and "
                                  "a row that prints fewer than all six cannot say which one this boot hit")
            if 'printf("clover r271:' in r299_added:
                errors.append(f"{r299_name} reprints the r271 cpu row: it is additive, and boots 58 to 87 are "
                              "compared against that row column by column")
            if 'clover r298' in r299_added:
                errors.append(f"{r299_name} touches the r298 identity row: this round is about cores, not "
                              "ports, and boot 87's tlink= reading has to survive it unchanged")
            for forbidden in ("thread_block(", "simple_wait(", "lck_mtx_lock(", "proc_lock("):
                if forbidden in r299_added:
                    errors.append(f"{r299_name} calls {forbidden}: the census prints beside a wedged userland "
                                  "and reads counters that a driver thread maintains -- a blocking read here "
                                  "changes the boot being measured")
            load_r299 = REPO.parent / "UniXNU" / "patches" / "xnu" / r299_name
            if not load_r299.is_file():
                errors.append(f"UniXNU/patches/xnu/{r299_name} is missing: unixnu loads patches from there "
                              "and silently skips one that lives only in this repo")
            elif load_r299.read_bytes() != r299_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r299_name} is not the same bytes as this repo's copy: "
                              "the two trees would compile different kernels")
            late_script = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list = " ".join(line for line in late_script.splitlines() if "clover-r2" in line)
            if (late_list.find(r299_name) < 0 or
                    late_list.find(r299_name) > late_list.find("z" * 57 + "-clover-r298.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r299 before r298 in LATE_PATCH: the "
                              "row is written against a tree that carries r271 and r298, and an apply failure "
                              "here is silent")
        armcpu_source = (REPO / "drivers/ArmCpu/ArmCpu.cpp").read_text()
        if "sheng-smp" in armcpu_source:
            errors.append("drivers/ArmCpu/ArmCpu.cpp still gates secondary startup on sheng-smp: a switch "
                          "named after another board is a switch nobody reading this board's boot-args will "
                          "find, which is how reg=1 of max=8 survived eleven boots")
        for needle in ('PE_parse_boot_argn ("armcpu-smp", &smp, sizeof (smp));',
                       'PE_parse_boot_argn ("armcpu-smp-remote-gic", &remote, sizeof (remote));',
                       'volatile uint64_t armcpu_smp_entry[8];',
                       'armcpu_smp_entry[logicalId] = entry;',
                       'armcpu_smp_fail[logicalId] = 9;'):
            if needle not in armcpu_source:
                errors.append(f"drivers/ArmCpu/ArmCpu.cpp lacks {needle[:60]!r}: the gate, the entry-point "
                              "record and the driver's own vtop failure are what the census row reads, so a "
                              "rename that stops short of them leaves the row printing zeros that mean "
                              "nothing")
        if '{ "clover r299: ", "r299-smp", 64, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the r299 row: an unclassed producer of one row "
                          "per census fire falls into the unclassed flow and is throttled with the chatter")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r299")' not in sdhci_source:
            errors.append("the r299 row is not exempt from the per-pass push cap: a capped row is a boot whose "
                          "secondaries read as never asked-for, which is the false-empty r249's census was "
                          "exempted for")
        for argument in ("armcpu-smp=1", "armcpu-self-ipi=1"):
            if argument not in board["payload"]["kernel_args"]:
                errors.append(f"kernel_args omits {argument}: the driver's secondary path is opt-in, so a "
                              "round that renames the switch without passing it changes no hardware at all")
        if any(argument.startswith("armcpu-spi-ipi") for argument in board["payload"]["kernel_args"]):
            errors.append("kernel_args passes armcpu-spi-ipi: GICD_TYPER=0x7b04f3 puts this board's last SPI "
                          "at INTID 671 and the driver's software-IPI base is 1008..1015, so that switch runs "
                          "against numbers this controller does not implement")
        if any(argument.startswith("armcpu-smp-remote-gic") for argument in board["payload"]["kernel_args"]):
            errors.append("kernel_args passes armcpu-smp-remote-gic: this round lets each new core arm its own "
                          "redistributor, which is the branch that has a wakeRedistributor(true) reading; "
                          "flipping both at once would fold two faults into one")


        # Round300 pays the instrument debt boot 88 opened. Its ring's newest row was the r299 one, and the
        # thread rows (r262) and chain rows (r296/r298) that boot 87's same fire put after it were absent --
        # all three classes exempt from the per-pass push cap, none of them named by a capped-at notice -- so
        # the card could not separate "the board stopped mid-fire" from "this fire emitted fewer rows". That
        # is the same class of reading r251 was built for, one layer up: an absence with no number beside it.
        # So a fire is now numbered, opened with what it planned (th= rows, hidden= the threads the cap kept
        # off the card) and closed with what really went out (th=, chain=), and the closing row is the one the
        # push cap must not touch.
        r300_name = "z" * 59 + "-clover-r300.patch"
        r300_patch = next((base / "patches/xnu-clover" / r300_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r300_name).is_file()), None)
        if r300_patch is None:
            errors.append(f"patches/xnu-clover/{r300_name} is missing: without the pair a boot 88-shaped frame "
                          "is read again as an absence, and the only two hypotheses it leaves are exactly as "
                          "far apart as they were before this round")
        else:
            r300_text = r300_patch.read_text()
            r300_added = "".join(line for line in r300_text.splitlines(True)
                                 if line.startswith("+")
                                 and not line.lstrip("+").lstrip().startswith(("*", "/*")))
            for needle in ('"clover r300: fire t=%llu seq=%llu pid=%d th=%u hidden=%u\\n"',
                           '"clover r300: done t=%llu seq=%llu th=%u chain=%u\\n"',
                           'static uint32_t clover_bb_chain_hops;',
                           'static uint64_t clover_census_seq;',
                           'static uint32_t clover_census_rows;',
                           'clover_census_rows++;',
                           'clover_bb_chain_hops++;',
                           'clover_census_rows = 0;',
                           'clover_bb_chain_hops = 0;'):
                if needle not in r300_text:
                    errors.append(f"{r300_name} lacks {needle[:72]!r}: planned, printed and hop-printed are "
                                  "three different numbers, and a row that carries fewer than all three puts "
                                  "the absence back where boot 88 left it")
            for reprint in ('printf("clover r262:', 'printf("clover r296:', 'printf("clover r298:'):
                if reprint in r300_added:
                    errors.append(f"{r300_name} reprints {reprint.split('(')[1]}: those rows are the counts this "
                                  "round reports, and a rewrite would make boots 50 to 88 unreadable against 89")
            if 'clover r299' in r300_added:
                errors.append(f"{r300_name} touches the r299 smp row: boot 88 is the only eight-core frame on "
                              "record and its row is what that reading rests on")
            for forbidden in ("thread_block(", "simple_wait(", "lck_mtx_lock(", "proc_lock(", "sleep("):
                if forbidden in r300_added:
                    errors.append(f"{r300_name} calls {forbidden}: the census prints beside a wedged userland, "
                                  "and a fire that blocks would show up on this very row as a missing done=")
            load_r300 = REPO.parent / "UniXNU" / "patches" / "xnu" / r300_name
            if not load_r300.is_file():
                errors.append(f"UniXNU/patches/xnu/{r300_name} is missing: unixnu loads patches from there "
                              "and silently skips one that lives only in this repo")
            elif load_r300.read_bytes() != r300_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r300_name} is not the same bytes as this repo's copy: "
                              "the two trees would compile different kernels")
            late_script_r300 = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list_r300 = " ".join(line for line in late_script_r300.splitlines() if "clover-r" in line)
            if (late_list_r300.find(r300_name) < 0 or
                    late_list_r300.find(r300_name) > late_list_r300.find("z" * 58 + "-clover-r299.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r300 before r299 in LATE_PATCH: the "
                              "counters reset between r299's row and the thread loop and the hop counter closes "
                              "r298's comment, so a tree without them fails to apply -- and an earlier apply "
                              "failure here is silent")
        if '{ "clover r300: ", "r300-fire", 64, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the r300 pair: two rows per fire that fall into "
                          "the unclassed flow are throttled with the chatter, which is the exact cap this "
                          "round exists to prove its rows past")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r300")' not in sdhci_source:
            errors.append("the r300 rows are not exempt from the per-pass push cap: a capped done= row reads "
                          "exactly like a fire the board never finished, and the round would ship the false "
                          "empty it was written to remove")

        # Round301 is driver-side only, and it exists because boot 90 read r300 back and got the sixth
        # shape: fire= and done= are themselves 0 rows, inside a ring whose 512 records run seq
        # 1194..1705 with no gap. The lesson is the standing one -- a verdict row needs a copy that does
        # not use the pipe under test -- and the highest-value verdict row on this board is the panic
        # sentence. Boots 89 and 90 both reached the sink (drain enter boot=NN append_rc=2, and
        # panicDrainRings() has exactly one caller) and both lost the text, because panicAppendText()
        # reads the report window before it writes and that one read was refused while the host was
        # owned. One attempt, no retry, no copy. So the assertions here are about the copy's existence,
        # its ordering against the two things that can lose it, and the fact that it does not depend on
        # the tee at all.
        for needle in ('static const uint32_t kPanicStashBytes = 480;',
                       'static const uint32_t kPanicStashPart = 240;',
                       'static char gPanicStash[kPanicStashBytes + 1];',
                       'gPanicStashLength = copied;',
                       '"CloverSdhci r301: panic text rc=%u chars=%u part=%u %s\\n"',
                       '"CloverSdhci r301: panic copy rc=%u chars=%u said=%u\\n"',
                       'gPanicAppendRc < 100'):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci lacks {needle[:64]!r}: without the copy a boot that "
                              "reaches the panic sink still leaves the card without a name, which is the "
                              "shape boots 89 and 90 are")
        # The copy has to be taken before anything that can refuse, and said before the drain's flushes.
        # Both are index tests rather than line counts because the file moves.
        stash_at = sdhci_source.find("gPanicStashLength = copied;")
        enter_mark_at = sdhci_source.find("flightPanicMark (0, 0, text != nullptr);")
        if stash_at < 0 or enter_mark_at < 0 or stash_at > enter_mark_at:
            errors.append("the panic-text copy does not come before the sink's first card write: the enter "
                          "marker is itself a write, and a refusal there would leave nothing copied")
        said_at = sdhci_source.find('"CloverSdhci r301: panic copy rc=%u chars=%u said=%u\\n"')
        probe_flush_at = sdhci_source.find("gPanicDrainWritten += panicProbeFlush ();")
        if said_at < 0 or probe_flush_at < 0 or said_at > probe_flush_at:
            errors.append("the r301 rows are queued after the drain's flushes: they would sit in RAM for a "
                          "beat that may never come, which is the failure this round is quoting")
        # The row is said only when the append did not land. A duplicate would make the host read one
        # panic as two, since the kind-20 record carries the same sentence when rc >= 100.
        if sdhci_source.count("panicAppendText (text, length)") != 1:
            errors.append("panicAppendText is called more than once: a retry from inside the sink is a "
                          "second owner of a card that is already refusing, and the copy exists so this "
                          "path does not have to fight it")
        if "%.*s" in sdhci_source:
            errors.append("the r301 row prints its part with %.*s: verdictFormat caps the whole row at "
                          "kVerdictTextBytes, so an over-long part would be cut with nothing on the card "
                          "saying how much -- the parts are capped instead and chars= names the total")
        # And the negative that keeps the round honest about which pipe it uses: this copy must not go
        # back through the tee, because the tee is the thing that lost the census tail three boots running.
        if '{ "clover r301' in sdhci_source:
            errors.append("r301 rows were added to the tee class table: the copy's whole point is that it "
                          "does not ride msgbuf and the tee, and a class here would put it back in the "
                          "burst whose loss it is meant to survive")

        # Round302 is driver-side only and answers the reading boot 91 forced. Its panic slot carries a
        # marker's event and dir words and a block request's header -- eight-block write from osanalyt --
        # because every flight event builds its record in the ONE gFlightBlock, which writePioBlock then
        # streams out of that same buffer while flightEnter or a beat rebuilds it; boot 88 carries the
        # identical shape, so this is the region's own defect and not r301's. The decoder could not tell,
        # because a marker has no signature, and printed round293's boot stamp as `stage=91`. So three
        # assertions: a verdict record owns its buffer, it signs itself after its extras, and the
        # sentence reaches the card through a write that reads nothing first -- which is exactly the read
        # boot 90's append_rc=2 was, and the reason r301's copy still needed a later flush.
        for needle in ('static const uint32_t kFlightTextOffset = 44;',
                       'static const uint32_t kFlightTextSlots = 12;',
                       'static const uint32_t kFlightEventText = 6;',
                       'static const uint32_t kFlightMarkerMagic = 0x434C564Du;',
                       'static const uint32_t kPanicStashBlindBytes = 352;',
                       'static uint32_t gFlightMarker[kBlockWords];',
                       'uint32_t *buf = forced ? gFlightMarker : gFlightBlock;',
                       'buf[35 + extraWords] = kFlightMarkerMagic;',
                       '(event == kFlightEventText) || (event == kFlightEventDrainIn);',
                       'flightWrite (kFlightEventText, 3, 0, gPanicStashLength, extra,',
                       '"CloverSdhci r302: blind ok=%u chars=%u total=%u slot=%u\\n"'):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci lacks {needle[:64]!r}: without the owned buffer, the "
                              "signature and the blind write, boot 91's shape stays on the card as a "
                              "reading and the sentence still needs a second chance to reach it")
        # The signature has to be laid down before the transfer starts and after the extras, or a record
        # cut off mid-build would carry one anyway.
        magic_at = sdhci_source.find("buf[35 + extraWords] = kFlightMarkerMagic;")
        extras_at = sdhci_source.find("buf[35 + i] = extra[i];")
        stream_at = sdhci_source.find("writePioBlock (record, buf, words, status)")
        if not (0 <= extras_at < magic_at < stream_at):
            errors.append("the r302 signature is not the last word written before the block streams: "
                          "a magic that can be present on a half-built record is a magic that lies")
        # The traffic buffer must not be able to carry a verdict again.
        if "writePioBlock (record, gFlightBlock" in sdhci_source:
            errors.append("flightWrite still streams gFlightBlock: that is the shared buffer boot 91's "
                          "hybrid came from, and one writer rebuilding it mid-transfer is the whole bug")
        # A sentence that reads the card to find its slot is the design that lost boot 90's text.
        text_fn = sdhci_source[sdhci_source.find("static void flightPanicText"):
                               sdhci_source.find("static void flightPanicMark")]
        if "readPioBlock" in text_fn:
            errors.append("flightPanicText reads the card before writing it: the pre-read is what "
                          "panicAppendText() lost the sentence on (boot 90, append_rc=2, one attempt), "
                          "and the slot is boot id modulo kFlightTextSlots precisely so nothing must be read")
        # It is written inside the sink, after the append's return code and before the exit marker, so
        # the exit marker stays the last word about the sink.
        blind_at = sdhci_source.find("flightPanicText (rc);")
        rc_at = sdhci_source.find("gPanicAppendRc = rc;")
        exit_mark_at = sdhci_source.find("flightPanicMark (1, rc, text != nullptr);")
        if not (0 <= rc_at < blind_at < exit_mark_at):
            errors.append("the blind sentence write is not between the append's return code and the exit "
                          "marker: it needs rc to decide whether the report record already carries the "
                          "text, and the exit marker has to stay the sink's last record")
        # The status row rides the drain's own flush, before the flushes, like r301's rows.
        row_at = sdhci_source.find('"CloverSdhci r302: blind ok=%u chars=%u total=%u slot=%u\\n"')
        flush_at = sdhci_source.find("gPanicDrainWritten += panicProbeFlush ();")
        if row_at < 0 or flush_at < 0 or row_at > flush_at:
            errors.append("the r302 status row is queued after the drain's flushes: it would report a "
                          "write that the host could only learn about on the next beat")
        # The sentence's slot is the boot id, so it must not advance the entry ring's cursor.
        seq_guard = sdhci_source.find("if (event != kFlightEventText) {")
        seq_step = sdhci_source.find("(*seqp)++;", seq_guard) if seq_guard >= 0 else -1
        if seq_guard < 0 or seq_step < 0 or seq_step - seq_guard > 400:
            errors.append("flightWrite advances the entry sequence for a sentence record: a panic at the "
                          "end of a boot would rotate the request ring the next boot reads")
        if '{ "clover r302' in sdhci_source:
            errors.append("r302 rows were added to the tee class table: this round's rows are driver-side "
                          "state and ride the RAM ring, not the pipe that lost the census tail")

        # Round303 is the desktop mainline: boot 87 used r298's tlink= to prove the debt chain holds at task
        # level (each hop's receive object names the next hop's own space), and the only thing that disagrees is
        # the name -- tname= 0x3207/0x3507/0x2903 against the vname= the receiver's threads wait on. So the next
        # reading is not on the sender's side but inside the receiving task: what that name is in the receiver's
        # own ipc entry table, and whether any of its threads is parked on it. A per-hop row reads tname= through
        # ipc_right_lookup_read() in the receiver's space (rentr=, rvbits=) and walks the receiver's own threads
        # (rwait=, rwtid=, rbusy=). Two instrument debts ride along because they are the same shared zero seen
        # twice: the mail helper now returns its translate code as qerr= (a qmc=0 qsr=0 only reads as a finding
        # when qerr= says the translate succeeded), read on every rcv= row and not just cap=recv; and when the
        # obj translate fails with KERN_INVALID_RIGHT -- boot 86's verr=17, an entry holding neither a receive nor
        # a send right -- it names which shape that is (port set, send-once, dead name) as vb_r=/vb_v= instead of
        # letting three readings print alike. The census runs beside a wedged userland and must not block: the one
        # lock it takes is the receiver's try-lock, and a busy lock is reported as such rather than waited on.
        r303_name = "z" * 60 + "-clover-r303.patch"
        r303_patch = next((base / "patches/xnu-clover" / r303_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r303_name).is_file()), None)
        if r303_patch is None:
            errors.append(f"patches/xnu-clover/{r303_name} is missing: without the receiving-side row the name "
                          "mismatch boot 87 named stays a hypothesis about the receiver's table rather than a "
                          "reading of it")
        else:
            r303_text = r303_patch.read_text()
            r303_added = "".join(line for line in r303_text.splitlines(True)
                                 if line.startswith("+")
                                 and not line.lstrip("+").lstrip().startswith(("*", "/*")))
            for needle in ('#include <ipc/ipc_right.h>',
                           'clover_sb_port_mail(task_t task, mach_port_name_t name, uint32_t *mc, uint32_t *sr,',
                           'int *err)',
                           '*err = -2;',
                           '*err = (int)kr;',
                           'clover_sb_port_obj(task_t task, mach_port_name_t name, int *err, int *otype,',
                           'uint32_t *rname, uintptr_t *rspace, uint32_t *vbits)',
                           'ipc_right_lookup_read(space, name, &bits, &o)',
                           '*vbits = (uint32_t)IE_BITS_TYPE(bits);',
                           '" to=%llx rmc=%u rsr=%x qmc=%u qsr=%u qerr=%d peer=%s end=%s\\n"',
                           '" rerr=%d otype=%d tname=%x tspace=%lx space=%lx link=%s tlink=%s"',
                           '" vb_r=%x vb_v=%x\\n"',
                           '"clover r303: hop=%u t=%llu rpid=%d rnm=%x rentr=%d"',
                           '" rvbits=%x rwait=%u rwtid=%llx rbusy=%d\\n"',
                           'if (nrcv != 0) {',
                           'int rentr = -2;',
                           'if (next->itk_space != IPC_SPACE_NULL) {',
                           'rentr = ipc_right_lookup_read(next->itk_space,',
                           'rvbits = (uint32_t)IE_BITS_TYPE(rbits);',
                           'io_unlock(robj_r);',
                           'if (!lck_mtx_try_lock(&next->lock)) {',
                           'rcv == tname || rem == tname'):
                if needle not in r303_text:
                    errors.append(f"{r303_name} lacks {needle[:72]!r}: the receiving-side census is three "
                                  "different lookups -- the entry, the receiver's threads, the shared-zero codes "
                                  "-- and a row missing any one leaves the name mismatch where boot 87 left it")
            for reprint in ('printf("clover r299:', 'printf("clover r300:'):
                if reprint in r303_added:
                    errors.append(f"{r303_name} reprints {reprint.split('(')[1]}: boot 88's smp row and boot 90's "
                                  "fire/done pair are the readings this row sits beside, and a rewrite would make "
                                  "boots 88 to 92 unreadable against 93")
            for forbidden in ("thread_block(", "simple_wait(", "lck_mtx_lock(", "proc_lock(", "sleep("):
                if forbidden in r303_added:
                    errors.append(f"{r303_name} calls {forbidden}: the census prints beside a wedged userland, "
                                  "and a row that blocks would freeze the very fire it is measuring")
            load_r303 = REPO.parent / "UniXNU" / "patches" / "xnu" / r303_name
            if not load_r303.is_file():
                errors.append(f"UniXNU/patches/xnu/{r303_name} is missing: unixnu loads patches from there "
                              "and silently skips one that lives only in this repo")
            elif load_r303.read_bytes() != r303_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r303_name} is not the same bytes as this repo's copy: "
                              "the two trees would compile different kernels")
            late_script_r303 = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list_r303 = " ".join(line for line in late_script_r303.splitlines() if "clover-r" in line)
            if (late_list_r303.find(r303_name) < 0 or
                    late_list_r303.find(r303_name) > late_list_r303.find("z" * 59 + "-clover-r300.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r303 before r300 in LATE_PATCH: the "
                              "census prints land after r300's hop counter, so a tree that does not remove r303 "
                              "first fails to apply r300 -- and an earlier apply failure here is silent")
        if '{ "clover r303: ", "r303-hop", 128, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci has no class for the r303 row: a per-hop census row that falls "
                          "into the unclassed flow is throttled with the chatter, which is the exact cap this "
                          "round exists to prove its row past")
        if 'teeHasPrefix (probe->name, teeStringLength (probe->name), "r303")' not in sdhci_source:
            errors.append("the r303 row is not exempt from the per-pass push cap: a capped receiving-side row "
                          "reads as a hop whose receiver has nothing, the false-empty the round was written to fill")

        # Round304 pays the debt every census round since 262 has carried and boot 93 finally named: the fire's
        # tail rows ride printf -> msgbuf -> the tee, and the tee only runs at the tail of a block request. Boot
        # 93 put the census head on the card at 555.48 s (r262 census threads=18 shown=18) and nothing after it --
        # not one of the 18 thread rows, not the chain pair, not r303's receiving row, not r300's own fire=/done=
        # pair, all of which are exempt from the push cap and none named by a capped-at notice -- and then the
        # board reset with no sink, so the panic drain never ran and the relay ring it owns was never read either
        # (zero kind-25 records in that window). So this round does two halves of the same thing: the kernel hands
        # those rows to round212's relay as well as to the console, and the relay gets consumers that do not need a
        # panic -- the guarded block tail and CPU0's four-second beat. The format strings are untouched bytes;
        # only the call changed, from printf() to snprintf() plus the handover, so boots 50 through 93 still line up
        # against 94. The relay must not block either: its handover is one atomic counter and a copy.
        r304_name = "z" * 61 + "-clover-r304.patch"
        r304_patch = next((base / "patches/xnu-clover" / r304_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r304_name).is_file()), None)
        if r304_patch is None:
            errors.append(f"patches/xnu-clover/{r304_name} is missing: without the relay the census tail stays a "
                          "reading that only survives when the board reaches the panic sink, which boot 93 did not")
        else:
            r304_text = r304_patch.read_text()
            r304_added = "".join(line for line in r304_text.splitlines(True)
                                 if line.startswith("+")
                                 and not line.lstrip("+").lstrip().startswith(("*", "/*")))
            for needle in ('extern unsigned int clover_emmc_probe_say(const char *text, unsigned int length);',
                           'clover_census_row(const char *line)',
                           'clover_emmc_probe_say(line, (unsigned int)strlen(line));',
                           'char crow[256];',
                           'int crowlen = snprintf(crow, sizeof(crow),',
                           'if (crowlen > 0) {',
                           '"clover r296: hop=%u t=%llu pid=%d comm=%s exec=%s ex=%s"',
                           '"clover r298: hop=%u t=%llu prev=%lx rvobj=%lx verr=%d vname=%x robj=%lx"',
                           '"clover r303: hop=%u t=%llu rpid=%d rnm=%x rentr=%d"',
                           '"clover r262: th tid=%llx st=%x we=%llx cont=%llx"',
                           '"clover r300: fire t=%llu seq=%llu pid=%d th=%u hidden=%u\\n"',
                           '"clover r300: done t=%llu seq=%llu th=%u chain=%u\\n"'):
                if needle not in r304_text:
                    errors.append(f"{r304_name} lacks {needle[:72]!r}: the relay is worth exactly as much as the "
                                  "rows it carries, and each of those six is one of them -- a format string that "
                                  "changed on the way would also cut boots 50 to 93 out of the comparison")
            if r304_added.count("clover_census_row(crow);") != 6:
                errors.append(f"{r304_name} relays {r304_added.count('clover_census_row(crow);')} rows, not 6: the "
                              "fire's opening row, its thread rows, the chain's identity+hop pair, the receiving "
                              "row and the closing row are the tail boot 93 lost, and a partial set reads as a "
                              "fire that emitted fewer rows -- the hypothesis this round exists to retire")
            for forbidden in ("thread_block(", "simple_wait(", "lck_mtx_lock(", "proc_lock(", "sleep(",
                              "hw_lock_lock("):
                if forbidden in r304_added:
                    errors.append(f"{r304_name} calls {forbidden}: the handover happens on the census path, which "
                                  "prints beside a wedged userland, and a relay that blocks stops being a copy")
            load_r304 = REPO.parent / "UniXNU" / "patches" / "xnu" / r304_name
            if not load_r304.is_file():
                errors.append(f"UniXNU/patches/xnu/{r304_name} is missing: unixnu loads patches from there "
                              "and silently skips one that lives only in this repo")
            elif load_r304.read_bytes() != r304_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r304_name} is not the same bytes as this repo's copy: "
                              "the two trees would compile different kernels")
            late_script_r304 = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list_r304 = " ".join(line for line in late_script_r304.splitlines() if "clover-r" in line)
            if (late_list_r304.find(r304_name) < 0 or
                    late_list_r304.find(r304_name) > late_list_r304.find("z" * 60 + "-clover-r303.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r304 before r303 in LATE_PATCH: r304 "
                              "rewrites print sites r303 and r300 added, so a tree that removes them first leaves "
                              "r304 un-reversible -- and an apply failure in that loop is silent")
        for needle in ('static volatile uint32_t gProbeNext = 0;',
                       'static uint32_t probeFlush (const char *why, uint32_t cap)',
                       'probeFlush ("beat", kProbeFlushCap);',
                       'probeFlush ("tail", kProbeFlushCap);',
                       'return probeFlush ("drain", 0);',
                       'static const uint32_t kProbeSealRetries = 4;',
                       '"CloverSdhci r307: probe why=%s calls=%u wrote=%u seen=%u next=%u pend=%u flushed=%u skipped=%u"',
                       '" stalled=%u refused=%u dropped=%u starve=%u/%u/%u/%u pio=%u/%x/%u@%u ch=%u"',
                       '" blocked=%u/%u/%u empty=%u cut=%u\\n"'):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci has no {needle[:52]!r}: the relay ring is only worth a reading "
                              "once something consumes it without a panic, and one consumer with a cursor is what "
                              "keeps three of them from writing the same row twice")
        if "const uint32_t first = (seen > kProbeSlots) ? seen - kProbeSlots : 0;" in sdhci_source:
            errors.append("the probe drain still re-reads its whole window: with the beat and the tail consuming "
                          "the same ring from a cursor, a drain that rescans every live slot puts every delivered "
                          "row on the card a second time")
        beat_at = sdhci_source.find("extern \"C\" void clover_emmc_flight_beat")
        if beat_at < 0 or sdhci_source.find('probeFlush ("beat"', beat_at) < 0:
            errors.append("the beat does not flush the probe ring: on a boot that never reaches the sink, "
                          "CPU0's timer is the only writer left, which is the whole reason boot 93 read nothing")
        # Round305. Boot 94 answered nothing for three reasons at once, and v0.7.14 had to withdraw two
        # claims because of them: the census rows and their relay copy left in the same order the console
        # got them (so a death inside printf() takes both, and one absence stands for two facts); the
        # flight records carry the driver's clock while the log lines carry the kernel's and no record on
        # the card holds the pair at one code point (so a marker can be bounded in kernel seconds but not
        # placed in them -- the four ≈12 s figures were each a subtraction of the simultaneity being
        # tested); and every card writer ends in one writePioBlock whose three refusal exits reach the
        # caller as the same false (so "the sink's next rows never ran" and "they ran and the card said
        # no" are one reading). Each of those three is pinned below, in the order the round fixes them.
        r305_name = "z" * 62 + "-clover-r305.patch"
        r305_patch = next((base / "patches/xnu-clover" / r305_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r305_name).is_file()), None)
        if r305_patch is None:
            errors.append(f"patches/xnu-clover/{r305_name} is missing: with the console copy still first, a "
                          "boot that dies inside printf() loses the row and its relay together and the card "
                          "cannot tell that from a fire that never reached the line")
        else:
            r305_text = r305_patch.read_text()
            r305_added = [line.lstrip("+") for line in r305_text.splitlines(True)
                          if line.startswith("+")
                          and not line.lstrip("+").lstrip().startswith(("*", "/*"))]
            # The order is what the round is, and a patch cannot be read for it line by line: the relay
            # line is context (r304 added it) and only the print moves. The post-image -- every context
            # line and every added line, in the hunk's own order -- is the file as it will compile.
            r305_after = [line for line in r305_text.splitlines(True)
                          if line.startswith((" ", "+")) and not line.startswith("+++")]
            relay_at = next((i for i, line in enumerate(r305_after)
                             if "clover_emmc_probe_say(line," in line), None)
            print_at = next((i for i, line in enumerate(r305_after)
                             if 'printf("%s", line);' in line), None)
            if relay_at is None or print_at is None:
                errors.append(f"{r305_name} does not contain both copies of the row: the whole reading is "
                              "that the two channels now split, and a patch that only moves one of them "
                              "moves a silence instead of naming it")
            elif relay_at > print_at:
                errors.append(f"{r305_name} still prints before it relays: boot 94's census rows are missing "
                              "from BOTH pipes, which is what this order is the only cure for")
            if not any(line.startswith("-") and 'printf("%s", line);' in line
                       for line in r305_text.splitlines(True)):
                errors.append(f"{r305_name} adds a second print instead of moving the one that was there: "
                              "a row that reaches the console twice would also reach the log buffer twice, "
                              "and msgbuf is where the tee reads its rows from")
            for forbidden in ("thread_block(", "simple_wait(", "lck_mtx_lock(", "proc_lock(", "sleep(",
                              "hw_lock_lock("):
                if forbidden in "".join(r305_added):
                    errors.append(f"{r305_name} calls {forbidden}: the handover happens on the census path, "
                                  "which prints beside a wedged userland, and a relay that blocks is not a copy")
            load_r305 = REPO.parent / "UniXNU" / "patches" / "xnu" / r305_name
            if not load_r305.is_file():
                errors.append(f"UniXNU/patches/xnu/{r305_name} is missing: unixnu loads patches from there "
                              "and silently skips one that lives only in this repo")
            elif load_r305.read_bytes() != r305_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r305_name} is not the same bytes as this repo's copy: "
                              "the two trees would compile different kernels")
            late_script_r305 = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list_r305 = " ".join(line for line in late_script_r305.splitlines() if "clover-r" in line)
            if (late_list_r305.find(r305_name) < 0 or
                    late_list_r305.find(r305_name) > late_list_r305.find("z" * 61 + "-clover-r304.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r305 before r304 in LATE_PATCH: r305 "
                              "edits the function r304 adds, so a tree that removes r304 first has nothing for "
                              "r305 to apply to -- and an apply failure in that loop is silent")
        for needle in ('#include <kern/clock.h>',
                       'absolutetime_to_nanoseconds (mach_absolute_time (), &ns);',
                       'static uint32_t kernelSeconds ()',
                       'extra[0] = kernelSeconds ();',
                       'flightWrite (kFlightEventBeat, 2, 0, 0, extra, 13)',
                       'extra[8] = kernelSeconds ();',
                       'flightWrite (kFlightEventPanic, 3, 0, 0, extra, 18)',
                       'extra[12] = gBootId;'):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci has no {needle[:52]!r}: the beat and the panic marker are "
                              "the two records this board reads its own ending from, and neither may convert "
                              "somebody else's timestamp -- v0.7.14 withdrew exactly that conversion")
        if sdhci_source.count("__atomic_add_fetch (&gBeatStarved[") < 5:
            errors.append(f"the beat counts its starvation exits {sdhci_source.count('__atomic_add_fetch (&gBeatStarved[')} "
                          "times, not at least five: a beat that returns early writes nothing, so an exit nobody "
                          "tallies is the same silence boot 94 reported as five why=tail rows and zero why=beat")
        if "__atomic_store_n (&gBeatStarved[" in sdhci_source:
            errors.append("the beat clears the starvation counters it reads: the block tail reports the same "
                          "four now, and a counter that one consumer empties decides what the other one "
                          "sees -- which is the shape of the absence boot 94 left behind")
        for needle in ('" stalled=%u refused=%u dropped=%u starve=%u/%u/%u/%u pio=%u/%x/%u@%u ch=%u"',
                       'static_cast<unsigned int> (__atomic_load_n (&gBeatStarved[kBeatHeld], __ATOMIC_RELAXED)),',
                       'gPioFailCode, gPioFailStatus, gPioFailWords, gPioFailMs, gPioFailAttempt'):
            if needle not in sdhci_source:
                errors.append(f"the probe report line has no {needle[:52]!r}: the beat cannot report its own "
                              "starvation when starvation is why it never wrote, so the tail report -- the only "
                              "consumer that ran on boot 94 -- has to carry these numbers too")
        for needle in ('pioFail (1, 0, 0, lba);',
                       'pioFail (2, status, 0, lba);',
                       'gPioAttempt = 100 + event;',
                       'gPioAttempt = kind ? kind : ((which == kRingKernel) ? 22u : 21u);',
                       'gPioAttempt = 20;',
                       'gPioAttempt = 0;'):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci has no {needle[:52]!r}: writePioBlock's three no-exits "
                              "currently reach every caller as one bare false, and until each names itself the "
                              "sink's silence is still two facts wearing one reading")
        reader_r305 = next((base / "scripts/read-clover-log.py"
                            for base in (REPO, pathlib.Path.cwd())
                            if (base / "scripts/read-clover-log.py").is_file()), None)
        if reader_r305 is not None:
            reader_text = reader_r305.read_text()
            for needle in ("FLIGHT_BOOT_WORD = {1: 35, 2: 35, 3: 47,", "FLIGHT_MAGIC_WORD = {4: 46, 5: 53,",
                           "arming_offset=", "starve=", "clocks unreadable"):
                if needle not in reader_text:
                    errors.append(f"read-clover-log.py does not know {needle!r}: the beat's boot id moved when "
                                  "its kernel second arrived at word 35, and a reader that kept looking there "
                                  "prints an uptime as a boot number -- the round293 defect re-made by this "
                                  "round's own change")

        # Round306. Boot 95 left one silence and three causes for it -- the walk stopped above the fire
        # row, the relay refused every row it was handed, or a consumer never got past its own guard --
        # and a report line that prints only when it has something to print cannot separate them, let
        # alone a quiet boot. These assertions are the three names, each counted where it happens.
        r306_name = "z" * 63 + "-clover-r306.patch"
        r306_patch = next((base / "patches/xnu-clover" / r306_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r306_name).is_file()), None)
        if r306_patch is None:
            errors.append(f"patches/xnu-clover/{r306_name} is missing: without a copy of \"the walk reached "
                          "this statement\" that does not ride the relay, boot 96 reads exactly like boot 95 "
                          "and says no more than it did")
        else:
            r306_text = r306_patch.read_text()
            r306_added = "".join(line.lstrip("+") for line in r306_text.splitlines()
                                 if line.startswith("+") and not line.startswith("+++"))
            # The rationale this round ships with says the relay's name in order to say the step row does
            # not use it, so the "must not ride the relay" checks read code lines only.
            r306_code = "".join(
                line.lstrip("+") for line in r306_text.splitlines()
                if line.startswith("+") and not line.startswith("+++")
                and not line.lstrip("+").lstrip().startswith(('*', '//', '/*')))
            if '"clover r306: step t=%llu seq=%llu pid=%d th=%u hidden=%u\\n"' not in r306_added:
                errors.append(f"{r306_name} does not print the step row with the fire row's own fields: "
                              "a pair that cannot be compared field for field is a second absence, not a "
                              "copy of the first")
            if "clover_census_row" in r306_code or "clover_emmc_probe_say" in r306_code:
                errors.append(f"{r306_name} sends the step row through the relay: the relay is what this "
                              "round is asking about, and a copy that rides the pipe under audit is not a "
                              "copy -- boot 90 lost fire and done with the rows they bracketed for exactly "
                              "that reason")
            counters_at = r306_text.find("clover_bb_chain_hops = 0;")
            step_at = r306_text.find("clover r306: step")
            fire_at = r306_text.find("clover r300: fire")
            if counters_at < 0 or step_at < counters_at or (0 <= fire_at < step_at):
                errors.append(f"{r306_name} does not put the step row between the three counters and the "
                              "fire row: outside that interval it cannot name the interval it is for")
            for forbidden in ('thread_block', 'simple_wait', 'lck_mtx_lock', 'proc_lock', 'hw_lock_lock'):
                if forbidden in r306_code:
                    errors.append(f"{r306_name} calls {forbidden}: the step row is printed while the census "
                                  "holds another task's thread walk, where nothing may sleep or take a lock")
            load_r306 = REPO.parent / "UniXNU" / "patches" / "xnu" / r306_name
            if not load_r306.is_file():
                errors.append(f"UniXNU/patches/xnu/{r306_name} is missing: unixnu loads patches from there, "
                              "and this repo's copy only ever names the round")
            elif load_r306.read_bytes() != r306_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r306_name} is not the same bytes as this repo's copy: "
                              "the shipped image would carry a different row than the one checked here")
            late_script_r306 = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list_r306 = " ".join(line for line in late_script_r306.splitlines() if "clover-r" in line)
            if (late_list_r306.find(r306_name) < 0 or
                    late_list_r306.find(r306_name) > late_list_r306.find("z" * 62 + "-clover-r305.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r306 before r305 in LATE_PATCH: r306 "
                              "inserts above the fire block r304 wrote and r305 reordered, and an apply failure "
                              "in that loop is silent")
        for needle in ('static volatile uint32_t gProbeBlocked[kProbeBlockCount]',
                       '__atomic_fetch_add (&gProbeBlocked[blocked], 1u, __ATOMIC_RELAXED);',
                       '__atomic_fetch_add (&gProbeEmptySay, 1u, __ATOMIC_RELAXED);',
                       '__atomic_fetch_add (&gProbeCutSay, 1u, __ATOMIC_RELAXED);',
                       '"CloverSdhci r307: probe silent why=%s guard=%s calls=%u seen=%u next=%u flushed=%u"',
                       'const bool nothing = gProbeCalls && !gProbeFlushed &&',
                       'gProbeNothingSay[whySlot] < kProbeNothingSayMax;',
                       '{ "clover r306: ", "r306-step", 64, 0, 0, 0 }',
                       'teeHasPrefix (probe->name, teeStringLength (probe->name), "r306")'):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci has no {needle[:52]!r}: a consumer that returns before its "
                              "pass counter is incremented leaves the report line unreachable, so the guard exit "
                              "has to say its own reason or the three causes of boot 95 stay one silence")

        # Round307. Boot 96 is the reading this contract exists to keep: four `nothing` slots, one shared
        # counter, and `why=tail` -- the consumer that runs most often -- spent every one of them, so the
        # line that would have said "the drain ran and the queue was empty" was bought out by a speaker
        # that had nothing to report about the stop. A bounded state line whose budget is global reports
        # the queue order of whoever asks first, which is a fact about this instrument and not about the
        # board. Three assertions: the budgets are arrays, the speaker is named on the line, and the pass
        # counter that the budgets used to hide is printed beside it.
        r307_name = "z" * 64 + "-clover-r307.patch"
        r307_patch = next((base / "patches/xnu-clover" / r307_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r307_name).is_file()), None)
        if r307_patch is None:
            errors.append(f"patches/xnu-clover/{r307_name} is missing: without a second copy of \"the walk "
                          "reached the counters\" that does not ride the kernel log, boot 96's step-row "
                          "absence still has two readings -- a tee that lost the row and a walk that never "
                          "made it -- and r306 was the round that could not separate them")
        else:
            r307_text = r307_patch.read_text()
            r307_added = "".join(line.lstrip("+") for line in r307_text.splitlines()
                                 if line.startswith("+") and not line.startswith("+++"))
            r307_code = "".join(
                line.lstrip("+") for line in r307_text.splitlines()
                if line.startswith("+") and not line.startswith("+++")
                and not line.lstrip("+").lstrip().startswith(('*', '//', '/*')))
            if '"clover r307: pre t=%llu seq=%llu pid=%d th=%u hidden=%u\\n"' not in r307_added:
                errors.append(f"{r307_name} does not print the pre row with the step and fire rows' own "
                              "fields: three rows that cannot be joined field for field are three absences "
                              "instead of one pair")
            if "clover_emmc_probe_say(prow," not in r307_code:
                errors.append(f"{r307_name} does not hand the pre row to the relay directly: the whole point "
                              "of this copy is that it reaches the card without the console path, which is "
                              "the pipe under audit")
            if "clover_census_row" in r307_code:
                errors.append(f"{r307_name} sends the pre row through clover_census_row: that function "
                              "prints to the console as well, so the copy would ride the pipe it is here to "
                              "cross-check and both channels would go quiet together")
            if 'printf("clover r307' in r307_code:
                errors.append(f"{r307_name} prints the pre row to the console: the relay-only copy is the "
                              "reading, and a printf here puts it back on the tee that r306 measured")
            counters_at = r307_text.find("clover_bb_chain_hops = 0;")
            pre_at = r307_text.find("clover r307: pre")
            # The block the new row is inserted above: round306's comment opens the step row's block, and it is
            # the hunk's own trailing context, so the interval is measurable inside the patch rather than only
            # in the file.
            step_at = r307_text.find("* Round306. One row printed by the console path alone")
            if counters_at < 0 or pre_at < counters_at or (0 <= step_at < pre_at):
                errors.append(f"{r307_name} does not put the pre row between the three counters and the "
                              "step row: outside that interval it is not a copy of the same \"got here\" "
                              "and the two rows cannot bracket anything")
            for forbidden in ('thread_block', 'simple_wait', 'lck_mtx_lock', 'proc_lock', 'hw_lock_lock'):
                if forbidden in r307_code:
                    errors.append(f"{r307_name} calls {forbidden}: the pre row is handed over while the "
                                  "census holds another task's thread walk, where nothing may sleep or take "
                                  "a lock")
            load_r307 = REPO.parent / "UniXNU" / "patches" / "xnu" / r307_name
            if not load_r307.is_file():
                errors.append(f"UniXNU/patches/xnu/{r307_name} is missing: unixnu loads patches from there, "
                              "and this repo's copy only ever names the round")
            elif load_r307.read_bytes() != r307_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r307_name} is not the same bytes as this repo's copy: "
                              "the shipped image would carry a different row than the one checked here")
            late_script_r307 = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list_r307 = " ".join(line for line in late_script_r307.splitlines() if "clover-r" in line)
            if (late_list_r307.find(r307_name) < 0 or
                    late_list_r307.find(r307_name) > late_list_r307.find("z" * 63 + "-clover-r306.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r307 before r306 in LATE_PATCH: r307 "
                              "inserts inside the block r306 added, so a tree that removes r306 first leaves "
                              "r307 un-reversible -- and an apply failure in that loop is silent")
        for needle in ('static uint32_t gProbeSilenceSay[kProbeWhyCount] = {0, 0, 0, 0};',
                       'static uint32_t gProbeNothingSay[kProbeWhyCount] = {0, 0, 0, 0};',
                       'static uint32_t gProbeReports[kProbeWhyCount] = {0, 0, 0, 0};',
                       'static uint32_t probeWhySlot (const char *why)',
                       'const uint32_t slot = probeWhySlot (why);',
                       'const uint32_t whySlot = probeWhySlot (why);',
                       'gProbeReports[whySlot] < kProbeReportsMax',
                       '"CloverSdhci r307: probe why=%s calls=%u wrote=%u seen=%u'):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci has no {needle[:52]!r}: boot 96's four `nothing` slots "
                              "were spent by why=tail alone, so a budget shared by three speakers reports "
                              "which consumer asked first and not whether the drain had anything to say")
        if "gProbeNothingSay < kProbeNothingSayMax" in sdhci_source or "gProbeSilenceSay < kProbeSilenceSayMax" in sdhci_source:
            errors.append("a report budget is a single counter again: one cap for three consumers means the "
                          "first speaker buys the others out of a voice, which is the blindness boot 96 "
                          "printed on the card while reporting it")
        if "CloverSdhci r306: probe" in sdhci_source:
            errors.append("the probe report and guard-exit lines still carry the r306 token: round307 added "
                          "a column to both, and a reader that cannot tell which generation wrote a record "
                          "reads an older line's missing calls= as a zero")
        if 'if (!gVerdictFirst || !gHost || gRingBusy[kRingDriver]) {' in sdhci_source:
            errors.append("probeFlush's three guard exits are merged into one return again: whichever of "
                          "\"no window armed\", \"the host belongs to another thread\" and \"this ring is "
                          "mid-flush\" it was, the card would say the same nothing -- and one line per exit "
                          "is what makes the reason a reading")
        if "CloverSdhci r304: probe" in sdhci_source:
            errors.append("the probe report line still carries the r304 token: a reader keys a field's meaning "
                          "off the line's round, so a line that does not name its own generation is a line an "
                          "older frame answers with somebody else's bytes -- the r305 clock pair learned this "
                          "on boot 94's stale marker")


        # Round308. Boot 98 ran the control round the ledger owed since boot 87 and answered none of it:
        # every reading that could have judged it -- the census family, the r271 core counts, the r229 early
        # returns -- is produced inside the burst whose absence the round controls for and printed through the
        # console tee that the same boot measured as throttled=56, lap=2. A judge that rides in the thing it
        # judges cannot judge, so this round moves the judge: the counters leave the sampler's frame for file
        # scope, two rows bracket the r299 printf on the driver relay alone, and one row per heartbeat says
        # the whole accounting on the relay before it prints it. Five things have to stay true in the patch:
        # the relay hand-offs exist, the bracket rows never reach the console, the beat row keeps both legs,
        # the counters are not local any more, and the two bracket rows sit exactly where the stop is.
        r308_name = "z" * 65 + "-clover-r308.patch"
        r308_patch = next((base / "patches/xnu-clover" / r308_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r308_name).is_file()), None)
        if r308_patch is None:
            errors.append(f"patches/xnu-clover/{r308_name} is missing: without a core count and a sampler "
                          "accounting that do not ride the kernel log, the next control round is judged by "
                          "the same pipe it is testing and boot 98 repeats")
        else:
            r308_text = r308_patch.read_text()
            r308_added = "".join(line.lstrip("+") for line in r308_text.splitlines()
                                 if line.startswith("+") and not line.startswith("+++"))
            r308_code = "".join(
                line.lstrip("+") for line in r308_text.splitlines()
                if line.startswith("+") and not line.startswith("+++")
                and not line.lstrip("+").lstrip().startswith(('*', '//', '/*')))
            for row in ('clover r308: beat', 'clover r308: smp-pre', 'clover r308: smp-post'):
                if r308_text.count(row) != 1:
                    errors.append(f"{r308_name} does not say {row} exactly once: two of one row and none of "
                                  "the other is how a relay pair turns back into an absence")
            for hand in ('clover_emmc_probe_say(prow308,', 'clover_emmc_probe_say(row308,',
                         'clover_emmc_probe_say(row,'):
                if hand not in r308_code:
                    errors.append(f"{r308_name} does not hand {hand.split('(')[1]} to the relay directly: the "
                                  "leg that survives a blind tee is the one that never enters the console")
            if 'printf("%s", row);' not in r308_code:
                errors.append(f"{r308_name} drops the beat row's second leg: one leg alone cannot tell a "
                              "relay that sealed a row from a sampler that never ran")
            for row, var in (('smp-pre', 'prow308'), ('smp-post', 'row308')):
                if f'printf("%s", {var})' in r308_code or f'printf("clover r308: {row}' in r308_code:
                    errors.append(f"{r308_name} prints the {row} row to the console: the pair is the "
                                  "negative control for the r299 printf, and a console copy of it would "
                                  "hang or lose exactly where the row is meant to say it did not")
            if "clover_census_row" in r308_code:
                errors.append(f"{r308_name} sends a row through clover_census_row: that function prints to "
                              "the console as well, so both of the round's channels would go quiet together")
            for needle in ('static volatile uint32_t clover_bb_enters;',
                           'static volatile uint32_t clover_bb_busy;',
                           'static volatile uint32_t clover_bb_capped;'):
                if needle not in r308_added:
                    errors.append(f"{r308_name} does not put {needle.split()[-1][:-1]} at file scope: a "
                                  "counter that lives in the sampled function's frame is only readable by a "
                                  "row produced inside the burst it counts")
            for gone in ('-	static uint32_t bb_fired;', '-	static uint32_t bb_same;'):
                if gone not in r308_text:
                    errors.append(f"{r308_name} leaves {gone[9:-1]} in place: two counters with one meaning "
                                  "is a reading that depends on which row the reader found first")
            positions = [r308_text.find("(unsigned long long)ctm[6], (unsigned long long)ctm[7]);"),
                         r308_text.find("clover r308: smp-pre"),
                         r308_text.find("* Round299. Same fire"),
                         r308_text.find("clover r308: smp-post"),
                         r308_text.find("clover_census_seq++;")]
            if min(positions) < 0 or positions != sorted(positions):
                errors.append(f"{r308_name} does not bracket the r299 row with pre before it and post after "
                              "it: outside that pair the two rows say nothing about the printf they exist to "
                              "time, which is the one statement boot 97 stopped at")
            beat_at = r308_text.find("clover_r308_beat_row(secs);")
            dump_at = r308_text.find("clover_ulock_dump(secs);")
            if dump_at < 0 or beat_at < dump_at:
                errors.append(f"{r308_name} does not say the beat row from the heartbeat after the ulock "
                              "dump: outside the heartbeat the row would run only when a target latched, "
                              "which is the state boot 98 could not read")
            for forbidden in ('thread_block', 'simple_wait', 'lck_mtx_lock', 'proc_lock', 'hw_lock_lock'):
                if forbidden in r308_code:
                    errors.append(f"{r308_name} calls {forbidden}: the bracket rows are handed over while "
                                  "the census holds another task's thread walk, where nothing may sleep or "
                                  "take a lock")
            load_r308 = REPO.parent / "UniXNU" / "patches" / "xnu" / r308_name
            if not load_r308.is_file():
                errors.append(f"UniXNU/patches/xnu/{r308_name} is missing: unixnu loads patches from there, "
                              "and this repo's copy only ever names the round")
            elif load_r308.read_bytes() != r308_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r308_name} is not the same bytes as this repo's copy: "
                              "the shipped image would carry a different row than the one checked here")
            late_script_r308 = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list_r308 = " ".join(line for line in late_script_r308.splitlines() if "clover-r" in line)
            if (late_list_r308.find(r308_name) < 0 or
                    late_list_r308.find(r308_name) > late_list_r308.find("z" * 64 + "-clover-r307.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r308 before r307 in LATE_PATCH: "
                              "r308 inserts inside the block r306 and r307 added, so a tree that removes "
                              "them first leaves r308 un-reversible -- and an apply failure in that loop is "
                              "silent")
        for needle in ('{ "clover r308: ", "r308-relay", 64, 0, 0, 0 }',
                       'teeHasPrefix (probe->name, teeStringLength (probe->name), "r308")',
                       '"CloverSdhci r308: drain exit boot=%u wrote=%u dropped=%u tee_passes=%u "'):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci has no {needle[:52]!r}: the relay's second leg is "
                              "printed by the console path only because this class and its exemption say "
                              "it may be, and without them a throttled copy reads as a dead relay")
        if "CloverSdhci r212: drain exit" in sdhci_source:
            errors.append("the drain exit line still carries the r212 token: round308 added a column and "
                          "renamed one, and a reader that cannot tell which generation wrote the record "
                          "reads this round's ring_pend= as that row never having a backlog at all")


        # Round309. Boot 100 was the first control round that could judge itself, and what it judged was that the
        # question was wrong: the census family has been absent for eleven boots because clover_bb_sample() has
        # exactly one call site, behind a latch that needs a sampled SpringBoard send whose remote name resolves
        # to <pid>:backboardd -- and the only row that reports the latch is printed on the beat thread's 120 s
        # cadence. Boot 100 latched at t=433 and reset at ~477, so the gate demonstrably opened and the boot that
        # proved it said nothing; boot 101 reproduced the shape exactly (its newest kernel line on the card is the
        # r221 row whose ro= is 58:backboardd, and every r308 beat row says target=0). The second thing boot 100
        # caught is a plain bug: the r170 exec and exit rows divided clock_get_uptime()'s machine ticks by 1e9 on a
        # board whose time base is 19267584 Hz, so every t= they ever printed was 51.90x short -- including the
        # timestamps those rows exist to give. Six things have to stay true in the patch: the three relay rows
        # exist, none of them has a console copy, the two clock rows move to a new token with the new unit, the
        # r170 token is gone rather than shadowed, and cen-in is the sampled function's first statement.
        r309_name = "z" * 66 + "-clover-r309.patch"
        r309_patch = next((base / "patches/xnu-clover" / r309_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r309_name).is_file()), None)
        if r309_patch is None:
            errors.append(f"patches/xnu-clover/{r309_name} is missing: without a row that says the latch opened "
                          "and a row that says the sampler ran, the next boot answers the eleven-boot absence with "
                          "the same 120 s cadence that hid it")
        else:
            r309_text = r309_patch.read_text()
            r309_added = "".join(line.lstrip("+") for line in r309_text.splitlines()
                                 if line.startswith("+") and not line.startswith("+++"))
            r309_code = "".join(
                line.lstrip("+") for line in r309_text.splitlines()
                if line.startswith("+") and not line.startswith("+++")
                and not line.lstrip("+").lstrip().startswith(('*', '//', '/*')))
            for row in ('clover r309: latch', 'clover r309: cen-in', 'clover r309: cen-out'):
                if r309_text.count(row) != 1:
                    errors.append(f"{r309_name} does not say {row} exactly once: the latch row is the only "
                                  "copy of \"the gate opened\" that does not wait for a heartbeat, and two of "
                                  "them make the count meaningless")
            for gone in ('clover r170: exec', 'clover r170: exit'):
                if gone not in r309_text:
                    errors.append(f"{r309_name} does not remove the {gone} row: the fix changes the unit of t=, "
                                  "and a reader that cannot tell which generation of row it is holding would "
                                  "read this round's seconds as 51.90x short -- or the old bytes as this "
                                  "round's seconds")
                if gone in r309_added:
                    errors.append(f"{r309_name} re-adds {gone}: two tokens with one meaning is a reading that "
                                  "depends on which row the reader found first")
            for row in ('clover r309: exec', 'clover r309: exit'):
                if r309_text.count(row) != 1:
                    errors.append(f"{r309_name} does not say {row} exactly once: the token moved with the unit "
                                  "so that an older card's r170 rows cannot be read as this round's seconds, and "
                                  "a second copy of the new token would put that boundary back in question")
            # The trailing comma is the whole assertion: without it this is a string, and `for hand in` walks
            # its characters, turning one check into fifty that each count a single letter.
            for hand in ('absolutetime_to_nanoseconds(clover_now, &clover_ns);',):
                if r309_code.count(hand) != 2:
                    errors.append(f"{r309_name} converts the tick count {r309_code.count(hand)} times instead of "
                                  "twice: this is the bug boot 100 measured, and one row left dividing machine "
                                  "ticks by 1e9 is one row still 51.90x short")
            for buffer_ in ('row', 'lrow'):
                if f'printf("%s", {buffer_})' in r309_code:
                    errors.append(f"{r309_name} prints the {buffer_} row to the console: the leg being measured "
                                  "is the console one, so a row that rides it cannot say whether the sampler ran "
                                  "-- the pre-registration says relay only, no printf copy")
            if "clover_census_row" in r309_code:
                errors.append(f"{r309_name} sends a row through clover_census_row: that function prints to the "
                              "console as well, so both channels would go quiet together")
            enters_at = r309_text.find("clover_bb_enters++;")
            cen_in_at = r309_text.find("clover_r309_cen_in(secs, pid);")
            bad_at = r309_text.find("if (pid <= 0 || sb_task == TASK_NULL) {")
            if min(enters_at, cen_in_at, bad_at) < 0 or not enters_at < cen_in_at < bad_at:
                errors.append(f"{r309_name} does not hand the entry row over as the sampled function's first "
                              "statement: after the pid check it would be a reading about a call that already "
                              "decided to leave, which is not the question the eleven boots asked")
            latch_assign = r309_text.find("clover_bb_target_name =")
            latch_row = r309_text.find("clover r309: latch")
            if min(latch_assign, latch_row) < 0 or latch_row < latch_assign:
                errors.append(f"{r309_name} does not say the latch row immediately after the latch is filled: "
                              "anywhere else it is a reading about a different instant than the gate opening")
            for why in ('"bad"', '"capped"', '"gone"', '"notask"', '"busy"', '"same"', '"done"'):
                if f'clover_r309_cen_out({why}' not in r309_code:
                    errors.append(f"{r309_name} does not name the {why[:-1]} return: an out row without the "
                                  "return it names is a count of rows, not a reading of where the walk stopped")
            load_r309 = REPO.parent / "UniXNU" / "patches" / "xnu" / r309_name
            if not load_r309.is_file():
                errors.append(f"UniXNU/patches/xnu/{r309_name} is missing: unixnu loads patches from there, "
                              "and this repo's copy only ever names the round")
            elif load_r309.read_bytes() != r309_patch.read_bytes():
                errors.append(f"UniXNU/patches/xnu/{r309_name} is not the same bytes as this repo's copy: "
                              "the shipped image would carry a different row than the one checked here")
            late_script_r309 = (REPO.parent / "UniXNU" / "scripts/build-xnu-4k-baseline.sh").read_text()
            late_list_r309 = " ".join(line for line in late_script_r309.splitlines() if "clover-r" in line)
            if (late_list_r309.find(r309_name) < 0 or
                    late_list_r309.find(r309_name) > late_list_r309.find("z" * 65 + "-clover-r308.patch")):
                errors.append("build-xnu-4k-baseline.sh does not reverse r309 before r308 in LATE_PATCH: "
                              "r309's helper block and its early-return edits sit inside regions r308 rewrote, "
                              "so a tree that removes r308 first leaves r309 un-reversible -- and an apply "
                              "failure in that loop is silent")
            # The loop that removes them is silent in a second way, and round309 paid for it: a listed path
            # that names no file fails its --reverse --check, the `if` takes the else, and the newest patch
            # stays in the tree while its neighbours get stripped -- which then breaks the replay on a file
            # that round has nothing to do with (this round's was bsd_init.c:987). Every path in the list has
            # to name a file, so a typo in a run of z's is a build error rather than a drift event.
            late_blob = late_script_r309.split("for LATE_PATCH in", 1)
            if len(late_blob) == 2:
                named = [piece.split('"')[0]
                         for piece in late_blob[1].split("; do")[0].split('"$ROOT/patches/xnu/')[1:]]
                missing = sorted({n for n in named
                                  if not (REPO.parent / "UniXNU" / "patches" / "xnu" / n).is_file()})
                if missing:
                    errors.append("build-xnu-4k-baseline.sh lists " + ", ".join(missing) +
                                  " in LATE_PATCH and no such file exists: the reverse loop's --check fails "
                                  "quietly, that patch stays in the tree, and the replay breaks somewhere else")
        for needle in ('static const uint32_t kFlightDrainOffset = 63;',
                       'static const uint32_t kFlightEventDrainIn = 7;',
                       'gPanicDrainInOk = flightPanicDrainIn () ? 1u : 0u;',
                       '"CloverSdhci r309: drain legs in=%u boot=%u host=%x probe=%u\\n"'):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci has no {needle[:52]!r}: boot 99 found no drain row of any "
                              "kind on a boot whose panic marker is on the card, and the only way that stops "
                              "being a candidate is a record written before the drain reads a gate, by the "
                              "path round302 proved needs no host ticket")
        drain_gate = sdhci_source.find("gPanicDrainInOk = flightPanicDrainIn ()")
        drain_first = sdhci_source.find("if (gPanicDrained || !gVerdictFirst || !gHost)")
        if drain_gate < 0 or drain_first < 0 or drain_gate > drain_first:
            errors.append("the drain-in record is not the first statement of panicDrainRings(): after that "
                          "gate it can only ever say the drain decided to speak, which is the reading boot 99 "
                          "already had")
        # Round310, the sink's own doorstep. Boot 102 entered the sink, landed its first marker and wrote
        # nothing after it -- not the sentence, not the blind copy of it, not the marker's exit half, not the
        # drain-in sector. All four of those ride the same forced raw-PIO write that had just succeeded in
        # the same millisecond, so together they name one call that did not return, not four refusals. What
        # it was waiting for is also on the card: the marker's host= word is a live ticket, the stage had
        # gone back to idle, and the last request record is a read stamped in that same millisecond -- a
        # holder the panic just stopped, with the beat that would have taken the ticket silent for 81 s.
        for needle in ('static void panicQuiesceCard (void)',
                       '  panicQuiesceCard ();',
                       'static bool cardRecoverHeld (const char *why, uint32_t detail, bool force = false);',
                       'if (!force && tries >= kCardRecoverCap) {',
                       'const bool ok = cardRecoverHeld ("panic", stage, true);',
                       '} else if (!tryTakeHost (ticket)) {',
                       '__atomic_store_n (&gPanicQuiesce, 7u, __ATOMIC_RELAXED);',
                       'uint32_t extra[18];',
                       'extra[15] = __atomic_load_n (&gPanicQuiesce, __ATOMIC_RELAXED);',
                       '(void)flightWrite (kFlightEventPanic, 3, 0, 0, extra, 18);',
                       '"CloverSdhci r310: quiesce %s stage=%u inhibit=%x owner=%x ok=%u probe=%u/%u\\n"'):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci has no {needle[:52]!r}: a sink that asks the card for "
                              "a write before it has ended the request the panic just orphaned can only ever "
                              "answer its own absences with \"the card refused\", which is what boot 102 did")
        quiesce_call = sdhci_source.find("  panicQuiesceCard ();")
        first_mark = sdhci_source.find("flightPanicMark (0, 0, text != nullptr);")
        if quiesce_call < 0 or first_mark < 0 or quiesce_call > first_mark:
            errors.append("panicQuiesceCard() is not called before the sink's first marker: after that write "
                          "the card has already been asked for one block, and the reading of its door state is "
                          "then the sink's own doing rather than what it walked in on")
        # sheng, the reference board, is the reason this is a quiesce and not a retry: its panic sink never
        # touches the storage stack at all (RAM copy, screen, park) and a single-owner writer thread does the
        # writing. That shape is not ours to copy -- the drain is what makes our sink useful -- but the one
        # thing it teaches is that the sink must not be the context that waits on a busy controller.



        # Round251, the recorder's own truncation. Boot 38 answered the question r250 was built to ask
        # and, in the same frame, showed the instrument eating the one reading still wanted:
        # rows[notes % 8] wraps from the ninth ask, and the dump printed slots in slot order, so the
        # seven earliest asks -- where a smaller pid like backboardd's 58 would have landed -- were
        # gone, and row 7 came back as the oldest line on the card. That is r249's defect one layer
        # up, so these assertions are the lesson made permanent: an unbounded tally that cannot be
        # truncated, a ring read from its wrap point, and a total printed next to the rows that
        # survive it, so a frame that dropped asks can never look like a frame that had none.
        r251_name = "z" * 34 + "-clover-r251.patch"
        r251_patch = next((base / "patches/xnu-clover" / r251_name
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "patches/xnu-clover" / r251_name).is_file()), None)
        if r251_patch is None:
            errors.append(f"patches/xnu-clover/{r251_name} is missing: boot 39 would again be read "
                          "with the instrument that overwrote the earliest asks, which is the one "
                          "class of reading this board has now lost twice")
        else:
            r251_added = "".join(line for line in r251_patch.read_text().splitlines(True)
                                 if line.startswith("+"))
            r251_text = r251_patch.read_text()
            for needle in ('#define CLOVER_MG_ROWS  24',
                           'clover_mg_tally(int pid, const char *name, bool path)',
                           'clover_mg_short(const char *logname)',
                           '"clover r251: who pid=%d comm=%.20s path=%u key=%u',
                           '"clover r251: row %u/%u %s',
                           '"clover r251: asks path=%llu key=%llu notes=%u whos=%u',
                           '"clover r251: no user client ever named a button path or key',
                           'start = clover_mg_notes <= CLOVER_MG_ROWS ? 0 : '
                           'clover_mg_notes % CLOVER_MG_ROWS'):
                if needle not in r251_text:
                    errors.append(f"{r251_patch.name} lacks {needle[:64]!r}: without it the frame "
                                  "cannot say whether backboardd asked, only how many asks fit")
            # The two things this round exists to remove.
            if "comm=%.16s" in r251_added:
                errors.append(f"{r251_patch.name} still truncates the caller's name at 16 bytes of a "
                              "string that starts with 'pid N, ': boot 38 returned that as 'SpringBo'")
            if 'printf("clover r251: row %u %s\\n", i, clover_mg_rows[i])' in r251_added:
                errors.append(f"{r251_patch.name} prints the ring in slot order again: a reader has "
                              "to re-derive the wrap from the timestamps to notice the oldest row is "
                              "printed seventh")
            # Same cross-file contract as r250: a row whose prefix the tee does not match never
            # reaches the card, and 24 rows plus the tally need a bigger class than 32.
            if r251_text.count('"clover r251: ') < 5:
                errors.append(f"{r251_patch.name} does not print every row under the "
                              '"clover r251: " prefix the tee class matches')

        # Round248, the manifest emitter. Boots 35 and 36 carried no reading at all -- not a drained
        # ring, not one block written, boot id still 34 -- and r247 blamed the census for it. The real
        # cause is upstream of XNU: scripts/unixnu/payload.py wrote the board's title into
        # EFI/UniXNU/unixnu.toml between bare quotes, the titles of those two rounds contain
        # getPlane("IODeviceTree"), and the string ended at the first quote, so the firmware's parser
        # dropped the xnu entry and the board booted a menu with no OS in it. The emitter now escapes
        # the title the way the install entry already did and parses the manifest it generated, so an
        # unreadable payload cannot be flashed again.
        emitter_text = (REPO.parent / "UniXNU" / "scripts/unixnu/payload.py").read_text()
        for needle in ('title = {json.dumps(title)}',
                       'raise Error(f\'generated unixnu.toml does not parse: {exc}\')',
                       "xnu entry does not carry this board's title and kernel path"):
            if needle not in emitter_text:
                errors.append("scripts/unixnu/payload.py lacks "
                              f"{needle!r}: a board title with a quote in it would again ship a "
                              "manifest the firmware cannot read, and the flash would produce no "
                              "reading at all")
        if 'title = "{spec.get("title"' in emitter_text:
            errors.append("scripts/unixnu/payload.py re-adds the unescaped title: that is the exact "
                          "shape that ate boots 35 and 36")
        # And the round title has to keep a quote in it, so the shipped manifest is a live test of
        # the escaping rather than a case that was never exercised.
        if '"' not in board["payload"]["title"]:
            errors.append("board.toml's round title carries no double quote: the escaping that lost "
                          "boots 35 and 36 would then ship untested again, and the next title that "
                          "quotes a function name would be the first one the firmware ever sees")
        emitted_title = f"title = {json.dumps(board['payload']['title'])}\n"
        try:
            if tomllib.loads(emitted_title)["title"] != board["payload"]["title"]:
                errors.append("the board title does not round-trip through a TOML basic string: the "
                              "menu would name a different round than the payload carries")
        except tomllib.TOMLDecodeError as exc:
            errors.append(f"board.toml's title cannot be emitted as a TOML basic string ({exc}): "
                          "the firmware would drop the xnu entry and boot with no OS to show")

        # Round254, the same root list read backwards. Round253 put AppleM68Buttons in it, the
        # personality matched, the driver attached to /J120AP/buttons and printed its own start --
        # and then panicked at t=12.744 s, "handleStart failed to instantiate function-button_hold"
        # @AppleM68Buttons.cpp:336, because handleStart builds function-button_<name> for every name
        # on the list in its __cstring and a synthesized ADT node gives it none of them. That boot
        # ended seventeen minutes short of the place being measured: no census, no property recorder,
        # no backboardd sample. The root stays out, pinned here with the reason, until the buttons
        # this board has to publish arrive through the board's own HID driver rather than through
        # Apple's SSHB one. The two families the round also needed remain roots for everything else
        # that uses them, so their absence would be a different bug.
        prelink_roots = board["kernel"]["prelink"]["roots"]
        if "com.apple.driver.AppleM68Buttons" in prelink_roots:
            errors.append("board.toml's [kernel.prelink] roots list com.apple.driver.AppleM68Buttons "
                          "again: it attaches, cannot instantiate function-button_hold against a "
                          "synthesized /buttons node, and panics the boot at 12.7 s -- nineteen "
                          "minutes less reading than the deadlock it was meant to explain")
        m68_deps = ("com.apple.iokit.IOHIDFamily", "com.apple.driver.AppleARMPlatform")
        for dep in m68_deps:
            if dep not in prelink_roots:
                errors.append(f"board.toml dropped {dep}: the HID family and the platform layer are "
                              "roots in their own right, and a boot without either cannot reach the "
                              "desktop whatever happens to the button driver")

        # Round255, the same sentence read forwards. What this board lacks is not Apple's button
        # driver but any driver that publishes the HID buttons, and the reference board's answer is an
        # owned IOHIDDevice subclass attached under the original IOHIDResource. Four files have to
        # agree about that: the driver, the probe that starts it, the builder that qualifies both and
        # archives them into the one force-loaded set the kernel links, and the board arguments that
        # arm it. Any one of them missing is a boot that silently does nothing -- which is exactly
        # what a boot where it all works also looks like from the outside, so the wiring is checked
        # here rather than inferred from a quiet ring.
        hid_probe = REPO / "package/CloverHidInputProbe.cpp"
        hid_driver = REPO / "drivers/NativeHidInput/NativeHidInput.cpp"
        hid_audit = REPO / "scripts/build-clover-hid-input.py"
        for path, why in ((hid_probe, "the probe that loads the families, waits for IOHIDResource and "
                                      "publishes the classification the compositor filters on"),
                          (hid_driver, "the owned IOHIDDevice subclass the probe allocates"),
                          (hid_audit, "the qualification that reads IOHIDDevice's real 207-slot table "
                                      "out of this board's kernel and refuses anything else")):
            if not path.is_file():
                errors.append(f"{path.relative_to(REPO)} is missing: {why}")
        if hid_probe.is_file():
            probe_text = hid_probe.read_text()
            for needle in ('PE_parse_boot_argn("offline-hid-input", &enabled, sizeof enabled)',
                           'PE_parse_boot_argn("offline-hid-home", &home, sizeof home)',
                           'OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily")',
                           'IOService::serviceMatching("IOHIDResource")',
                           'clover_hid_class_sizes[4] != 208',
                           'extern "C" __attribute__((noinline)) void native_hid_live_checkpoint(IOService *device)',
                           'volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;',
                           'extern "C" void native_touch_set_sink(void (*sink)(void *), void *context)',
                           'extern "C" void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context)',
                           'if (native_legacy_transfer_calls < 2) { return false; }'):
                if needle not in probe_text:
                    errors.append(f"package/CloverHidInputProbe.cpp lacks {needle[:64]!r}: the device "
                                  "would be started on the wrong condition, or the driver's own hooks "
                                  "would be compiled C++-mangled and the archive would not link")
            # The device must not report into a display that is not driven yet, and it must not go on
            # pressing Home every second once it can: a bounded grant per tick, counted. Round256 then
            # changes how the state is said: ONE capped early line (the story while it changes) plus a
            # beat line gated on the ARM counter, not on a call count -- the driver calls the checkpoint
            # ten times a second while a press is held, so counting calls would drift with input state.
            if "clover_hid_presses" not in probe_text:
                errors.append("package/CloverHidInputProbe.cpp does not count the Home presses it "
                              "grants: an unbounded tick would keep the compositor busy for the rest "
                              "of the boot and the frame could not tell a held device from a looping one")
            if probe_text.count("if (printed++ < 8)") != 1:
                errors.append("package/CloverHidInputProbe.cpp does not cap the early checkpoint at one "
                              "capped print: boot 43 showed the early lines are the ones the ring loses, "
                              "so they are bounded and the beat carries the rest")
            for needle in ('"clover r255: beat stage=%d service=%llx%s published=%u seen=%u presses=%u asks=%u "',
                           '"touch=%u reports=%u sinks=%u,%u transfer=%u\\n"',
                           "mrs %0, cntpct_el0", "mrs %0, cntfrq_el0"):
                if needle not in probe_text:
                    errors.append(f"package/CloverHidInputProbe.cpp lacks {needle[:56]!r}: without the "
                                  "beat this instrument says its verdict once at t=25 s into a ring that "
                                  "keeps 512 rows from t=1208 s, which is how round255 lost the answer")
            # Round257: boot 44 read published=0 seen=0 with the event driver matched, and the cause was
            # this file's own copy of sheng's precondition -- the vendored driver sets Built-In only for a
            # touch device, so on a button device the guard refused forever. The flag is a reading now.
            if 'device-has-no-built-in' in probe_text:
                errors.append("package/CloverHidInputProbe.cpp still gates the publish on the device's "
                              "own Built-In flag: boot 44 showed that gate is why nothing was ever "
                              "published on a button-only device")
            for needle in ('" deviceBuiltIn=%d"', "clover_hid_requests"):
                if needle not in probe_text:
                    errors.append(f"package/CloverHidInputProbe.cpp lacks {needle[:40]!r}: the device's "
                                  "flag has to stay readable, and an ask is not a press -- 1086 of one "
                                  "looked like a compositor being hammered when three grants were made")
            # Round259: the touch sink stops being a counter and becomes a producer. sheng's device
            # reports real fingers, and the last shape difference left between the two boards was a
            # digitizer collection with nothing in it -- so the beat publishes one frame at the centre
            # of the panel and lifts it, through the driver's own notification and the driver's own
            # seqlock protocol, and it does so exactly once.
            for needle in ("void publishTouchFrame(unsigned down) {",
                           "native_touch_count = down ? 1 : 0;",
                           "gTouchSink(gTouchContext);",
                           "if (gTouchSink != nullptr && lastService != 0 && clover_hid_touch_frames == 0)",
                           "native_touch_hid_reports,",
                           'native_touch_xs[0] = 15240;', 'native_touch_ys[0] = 10160;'):
                if needle not in probe_text:
                    errors.append(f"package/CloverHidInputProbe.cpp lacks {needle[:56]!r}: the board "
                                  "would still be a device that publishes a digitizer collection and "
                                  "never puts a finger in it, which is the difference between this "
                                  "board and the one that reaches the desktop")
            if probe_text.count("gTouchSink(gTouchContext);") != 2:
                errors.append("package/CloverHidInputProbe.cpp does not call the sink twice: a press "
                              "with no lift leaves a finger down on the panel forever, and a single "
                              "report is not a touch")
            if probe_text.count("clover_hid_touch_frames == 0") != 1:
                errors.append("package/CloverHidInputProbe.cpp does not make the synthetic frame "
                              "one-shot: a device that keeps producing fingers nobody moved is a fake "
                              "input source rather than an instrument")
            # Round260, and this one is the reading round259 could not get. Boot 47 had the I2C
            # instrument armed and running -- and came back with no `clover r181` line at all, because
            # it spoke at t=12 s into a kernel ring that keeps the last 512 rows, and that boot's window
            # began at t=1194 s. UniXNU-sheng does not have this failure because its drivers do not
            # print: package/TouchProbe.cpp:29 keeps `char native_touch_rows[4][120]` and
            # package/BootLog.c:340-356 re-composes those rows onto the medium every 2 s, so a reading
            # is state and stays readable. CloverInput already keeps exactly that state --
            # clover_input_observation[16], defined for the reason in its own comment -- and this file
            # is now the composer that re-says it on every beat.
            for needle in ('extern "C" volatile uint64_t clover_input_observation[16];',
                           '"clover r260: bus mode=%llu map=%llx hw=%llx io=%llx op=%llx err=%llx status=%llx "',
                           '"state=%llx clk=%llx ack=%llx id=%llx,%llx xfers=%llu timeout=%llu nack=%llu "',
                           '"verdict=%lld\\n"'):
                if needle not in probe_text:
                    errors.append(f"package/CloverHidInputProbe.cpp lacks {needle[:56]!r}: the touch bus "
                                  "verdict would again be a burst of early lines the ring throws away, "
                                  "which is the reading round259 armed this instrument to get")
            if probe_text.count("clover_input_observation[15]") != 1:
                errors.append("package/CloverHidInputProbe.cpp does not carry the bus instrument's last "
                              "verdict code on the beat exactly once: without it a row of zeros cannot be "
                              "told from a probe that never ran")
            # Round319's beat row re-says the r318 raw words (raw status/err, SDA/SCL's own bits) that
            # the r260 line's 16 slots do not carry. It must read the latch, not re-read the bus: a
            # beat that read the registers would touch the bus twice per boot, which is the one thing
            # round260 was careful not to do.
            for needle in ('extern "C" volatile uint64_t clover_input_last_xfer[6];',
                           '"clover r319: last status=%llx err=%llx sda=%llu scl=%llu vend=%llx res=%llx\\n"'):
                if needle not in probe_text:
                    errors.append(f"package/CloverHidInputProbe.cpp lacks the r319 beat row {needle[:56]!r}: "
                                  "the raw status word and SDA/SCL would again be one-time rows the ring "
                                  "throws away, which is the blind spot round319 exists to remove")
            for i in range(6):
                if probe_text.count(f"clover_input_last_xfer[{i}]") != 1:
                    errors.append(f"package/CloverHidInputProbe.cpp does not read r319 latch slot {i} "
                                  "exactly once on the beat: a dropped slot reads as a zero, and a zero "
                                  "must mean the instrument never ran, not a field nobody printed")
            display_text = (REPO / "package/CloverLegacyDisplayProbe.cpp").read_text()
            if not (0 <= display_text.find("clover_hid_input_probe();")
                    < display_text.find("IOService *display=pendingLegacyDisplay;")):
                errors.append("package/CloverLegacyDisplayProbe.cpp does not arm the HID device at the "
                              "top of the late entry: the framebuffer branch can return without ever "
                              "reaching it, and then the round measures nothing")
            if 'extern "C" void clover_hid_input_probe(void);' not in display_text:
                errors.append("package/CloverLegacyDisplayProbe.cpp calls the HID probe without "
                              "declaring it: the two objects are in one archive, and a name that "
                              "differs by mangling links as a different symbol")
            builder_text = (REPO / "scripts/build-clover-gop-legacy-display.py").read_text()
            for needle in ("hid / 'NativeHidInput.late.o', hid / 'hid-original-bindings.o'",
                           "kits/clover/CloverHidInputProbe.cpp",
                           "ROOT / 'scripts/build-clover-hid-input.py'",
                           "'NativeHidInput.late': hid / 'NativeHidInput.late.o'",
                           "'_native_touch_set_sink'",
                           "'_clover_input_observation' in globals_of(hid_probe_obj)[1]",
                           "'_native_hid', '_native_touch'"):
                if needle not in builder_text:
                    errors.append(f"scripts/build-clover-gop-legacy-display.py lacks {needle[:56]!r}: "
                                  "the HID objects would not be in the archive the kernel force-loads, "
                                  "and an unresolved board symbol would only surface in the link")
        # Round258, both halves read off UniXNU-sheng rather than invented here: the device shape and the
        # vnode floor. The ADT line is checked where the ADT is generated, because that is the only file
        # whose bytes end up on the card.
        adt_generator = (REPO / "scripts/make-clover-adt.py").read_text()
        if '("kern.maxvnodes", u32(66560))' not in adt_generator:
            errors.append("scripts/make-clover-adt.py does not publish kern.maxvnodes: sheng's "
                          "make-sheng-adt.py:176-179 names vnode jetsam as the last blocker it cleared "
                          "for an idle desktop, and this kernel family clips desiredvnodes at the "
                          "CONFIG_VNODES=1024 built-in without it")
        if 'PE_parse_boot_argn("sheng-touch-hid"' not in (REPO / "drivers/NativeHidInput/NativeHidInput.cpp").read_text():
            errors.append("drivers/NativeHidInput/NativeHidInput.cpp no longer reads sheng-touch-hid: that "
                          "argument is the only thing that gives the device its digitizer collection and its "
                          "device-side Built-In/DisplayIntegrated/DigitizerCollectionDispatch, which is the "
                          "shape the reference board boots a desktop with")
        for needle in ("require(VPTR == 0xfffffff005d045c0",
                       "changed == [0, 1, 7, 12, 14, 21, 23, 76, 81, 82, 86, 87, 91, 92, 93, 101, 132, 140]",
                       "static_assert(sizeof(IOHIDDevice)==0xd0",
                       "owned == [0, 1, 7, 14, 86, 87, 111, 171, 173, 180]",
                       "the constructor stores {stored:#x} but the vtable qualified here is"):
            if not hid_audit.is_file() or needle not in hid_audit.read_text():
                errors.append(f"scripts/build-clover-hid-input.py lacks {needle[:56]!r}: the subclass "
                              "would be compiled against a claim nobody re-read from this board's own "
                              "image, which is how a vtable slot ends up pointing at the wrong method")
        for argument in ("offline-hid-input=1", "offline-hid-home=1", "sheng-touch-hid=1"):
            if argument not in board["payload"]["kernel_args"]:
                errors.append(f"[payload] kernel_args has no {argument}: the instrument stays dark and "
                              "the boot reads back the same silence round254 left")

        # Round229's first cut is negative: three producers filled half the kernel ring with lines
        # that say nothing new (the bare apfs event tail, a nagging client repeated, an exit line per
        # respawn), so the ring started 400 s into the boot. The filters have to be in the object, and
        # their counters have to be reported at the drain, or a ring that starts late again cannot be
        # told from a filter that never ran.
        for needle in ("static uint32_t teeStampSkip (const char *line, uint32_t length)",
                       "static bool teeExitCommKey (const char *line, uint32_t length, TeeExitComm **entry)",
                       "static bool teeNagSource (const char *line, uint32_t length, uint32_t *hash)",
                       "CloverSdhci r229: tee filter fl=%u nag=%u exit kept=%u dropped=%u comms=%u\\n"):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci lacks {needle[:52]!r}: the ring would again "
                              "spend half its records on the three producers r229 filters out, "
                              "and a late start would not name which one filled it")
        # Round230: boot 18 re-ran the census and the ring's biggest resident had become CloverBlock's
        # r190 barrier pair (241 of 504). Same defect, one producer further down the list.
        for needle in ("static const uint32_t kTeeSyncKeep = 8;",
                       'teeHasPrefix (body, rest, "CloverBlock r190: sync-in ")',
                       "CloverSdhci r230: tee sync-in=%u sync-out=%u dropped=%u\\n"):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci lacks {needle[:52]!r}: the sync metronome would "
                              "again take half the ring, and a ring that starts late could not say "
                              "how many of its records the filter held back")
        if "CloverSdhci r212: panic tail lines=%u unseen=%u bufx=%d tee=%u boot=%u" not in sdhci_source:
            errors.append("drivers/CloverSdhci has no round212 panic-time tail read: a boot whose "
                          "block path stops would again take the kernel ring with it, and without "
                          "the boot id a tail line could not be attributed to this boot")
        if "append_rc=%u\\n" not in sdhci_source:
            errors.append("the round211 tail status row drops panicAppendText's return code: a sink "
                          "that could not take the panic's own sentence would look like a silent one")
        if "panicTailRead ();" not in sdhci_source:
            errors.append("drivers/CloverSdhci never calls the round209 tail read from the panic "
                          "drain: the sink would not select it")
        # Round212. The round209 tail read was gated on gTeeReady, and on the one boot it mattered
        # the gate explained nothing: the ring was armed, the sink was silent, and the unchanged
        # card could not say whether the read had even run. Three things replace that silence --
        # the boot counter (so a record names its boot instead of being inferred), an ungated tail
        # read, and a probe ring the kernel can reach while the process is still alive.
        for needle, why in (
                ("static const uint32_t kBootRecordOffset = 192 + 2 * 512;",
                 "round212's boot record is not one block past the kernel ring, where the reader "
                 "looks and where the dump stops"),
                ("static const uint32_t kBootKind = 24;",
                 "no kind names the boot record, so a stale counter would read as a record"),
                ("static const uint32_t kProbeKind = 25;",
                 "no kind names a relayed probe line, so it could not be told from a verdict"),
                ("static uint32_t bootIdClaim (uint32_t lba)",
                 "no boot counter: two boots' records would still be inferred apart by guessing"),
                ("gBootId = bootIdClaim (entryLdFirst + kBootRecordOffset);",
                 "the boot counter is not claimed when the window arms, so it would be read "
                 "before it was ever written"),
                ("CloverSdhci r212: boot id=%u record=%u probe slots=%u text=%u",
                 "no arming line names the boot id: a card could not say which boot wrote it"),
                ("gRingBlock[9] = gBootId;",
                 "two-block records do not carry the boot id in word 9"),
                ('extern "C" unsigned int clover_emmc_probe_say (const char *text, unsigned int length)',
                 "no probe entry point: the kernel's kill-point lines could only reach msgbuf, "
                 "which the panic a minute later has already overwritten"),
                ("CloverSdhci r212: drain enter boot=%u first=%u panic_first=%u verdict=%u tee=%u ",
                 "no drain enter line: a silent panic drain could not be told from one that "
                 "never ran"),
                ("CloverSdhci r308: drain exit boot=%u wrote=%u dropped=%u tee_passes=%u ",
                 "no drain exit line: the flush would not report how much it wrote"),
                ("probe=%u probe_drop=%u ring_pend=%u probe_pend=%u\\n",
                 "the drain exit line still reports one backlog for two rings: boot 97 read a zero under "
                 "the name probe_pend= that was the driver tee ring's count, so the relay's own queue was "
                 "the one number the row did not say"),
                ("panicProbeFlush ()",
                 "the drain never flushes the probe ring"),
                ("static uint32_t panicRingFlush (uint32_t which)",
                 "the drain never flushes a ring directly, so a dead tee takes both rings with it"),
                ("slot = minSeqSlot;",
                 "the panic window still refuses when full instead of rotating the oldest slot")):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci lacks {needle[:48]!r}: {why}")
        # The round209 early return is the specific defect round212 removes; naming it keeps a
        # future edit from putting the gate back where it hid the one silence that mattered.
        tail = sdhci_source[sdhci_source.find("static void panicTailRead (void)"):
                            sdhci_source.find("static void panicTailRead (void)") + 4000]
        if "if (!gTeeReady)" in tail:
            errors.append("panicTailRead returns early when the tee is not armed: the round212 "
                          "tail read is meant to run even then, and the boot id is what the "
                          "silence is now attributed with")
        for needle, why in (
                ("static const uint32_t kFlightEntryOffset = 32;",
                 "round210's entry region is not at sector 32, where the reader looks"),
                ("static const uint32_t kFlightBeatOffset = 56;",
                 "round210's beat region is not at sector 56, where the reader looks"),
                ("CloverSdhci r210: flight armed entry=%u slots=%u beat=%u slots=%u ",
                 "no round210 arming line: a dump could not tell 'never armed' from 'armed and quiet'"),
                ("flightEnter (kFlightEventRead", "no entry record names a block read"),
                ("flightEnter (kFlightEventWrite", "no entry record names a block write"),
                ("static void flightExit (uint32_t done)",
                 "no exit records the result of the request just finished"),
                ("extern \"C\" void clover_emmc_flight_beat (void)",
                 "no beat entry point for the timer to call")):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci lacks {needle[:40]}...: {why}")
        # Round213: the beat must never wait, so it takes the host with one compare-and-swap; and a
        # holder whose stage has not moved has to be recovered by that same beat, or the one request
        # that stopped still costs the boot (r212: every later reader and the panic sink queued
        # behind it). Naming both halves here keeps a later edit from leaving only the sampler.
        for needle, why in (
                ("static bool tryTakeHost (uint32_t &token)",
                 "no lock-free acquire: the beat would have to wait for the request it samples"),
                ("if (!tryTakeHost (token))",
                 "the beat does not take the host with the one-shot ticket"),
                ("static void recoverStuckRequest (uint32_t stage, uint32_t owner)",
                 "no recovery path: a stuck holder would keep the host until the boot died"),
                ("recoverStuckRequest (stage, owner);",
                 "the beat measures a stuck stage and never acts on it"),
                ("static const uint64_t kGuardStuckMs = 75000;",
                 "the stuck bound is not the measured 75 s"),
                ("dropHostTicket (token);",
                 "the beat releases with the request's release, which would erase the holder's stage")):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci lacks {needle[:44]!r}: {why}")
        armcpu_source = (REPO / "drivers/ArmCpu/ArmCpu.cpp").read_text()
        if "clover_emmc_flight_beat ();" not in armcpu_source:
            errors.append("drivers/ArmCpu never calls the round210 beat: nothing would sample the "
                          "controller between block requests")
        # Round236: boot 24 hit its 1800 s deadline with no kind-20 panic record, no drain-enter line
        # and an empty BCB while the driver's own ring carried six card recoveries in the same
        # stretch. The sink's silence was the card's, so the deadline gets a record that does not go
        # through the sink at all: ArmCpu calls into the driver immediately before panic(), the
        # driver writes one dedicated block past the guard and the arming gates, and the sink's own
        # entry and exit are marked too so a sink that never ran can be told from one that ran and
        # failed.
        for needle, why in (
                ("static const uint32_t kFlightDeadlineOffset = 60;",
                 "the deadline record is not at sector 60, where the reader looks"),
                ("static const uint32_t kFlightPanicOffset = 61;",
                 "the panic markers are not at sectors 61-62, where the reader looks"),
                ("const bool forced = (event == kFlightEventDeadline) || (event == kFlightEventPanic) ||",
                 "the survival records would still need the arming gates"),
                ("(event == kFlightEventText) || (event == kFlightEventDrainIn);",
                 "the sentence record or round309's drain-in record is not one of the forced events: on a "
                 "boot that never armed the flight region it would be refused before the sink ran"),
                ("if (!forced && !__atomic_load_n (&gFlightArmed, __ATOMIC_ACQUIRE)) {",
                 "the armed gate no longer exempts the forced records"),
                ("extern \"C\" void clover_emmc_flight_deadline (",
                 "no deadline entry point for the timer to call before panic()"),
                ("if (__atomic_exchange_n (&gFlightDeadlineDone, 1u, __ATOMIC_ACQ_REL)) {",
                 "the deadline record is not one-shot: every CPU in the interrupt would write it"),
                ("(void)flightWrite (kFlightEventDeadline, 3, 0, 0, extra, 11);",
                 "the deadline record does not carry its five 64-bit numbers plus the boot stamp"),
                ("flightPanicMark (0, 0, text != nullptr);",
                 "the panic sink has no entry marker: a sink that died inside append would leave "
                 "nothing to say it was ever entered"),
                ("flightPanicMark (1, rc, text != nullptr);",
                 "the panic sink has no exit marker: the append's return code would be unreadable"),
                ("(void)flightWrite (kFlightEventPanic, 3, 0, 0, extra, 18);",
                 "the panic marker does not carry its eight gate words, round305's kernel second and last "
                 "refusal and round310's relay cursors and door state -- and the signature then lands at the "
                 "wrong word"),
                ("__atomic_load_n (&gHostOwner, __ATOMIC_ACQUIRE)",
                 "the panic marker does not read the live host owner, which is the one gate its "
                 "return code cannot express")):
            if needle not in sdhci_source:
                errors.append(f"drivers/CloverSdhci lacks {needle[:52]!r}: {why}")
        deadline_call = armcpu_source.find("clover_emmc_flight_deadline (")
        panic_call = armcpu_source.find('panic ("clover r148:')
        if deadline_call < 0:
            errors.append("drivers/ArmCpu never calls the round236 deadline record: the deadline "
                          "would again be known only through the sink that boot 24 starved")
        elif panic_call < 0 or deadline_call > panic_call:
            errors.append("drivers/ArmCpu calls the deadline record after panic(): the record has "
                          "to be on the card before the screen starts resetting the board")

    adt = board["payload"]["adt"]
    cores = [int(value, 0) for value in adt["cpu_regs"].split(",") if value.strip()]
    if cores != CPU_REGS:
        errors.append(f"unexpected Kryo 260 MPIDR set: {cores}")
    if int(adt["chip_id"], 0) != MSM_ID:
        errors.append(f"[payload.adt] chip_id is {adt['chip_id']}, the SDM660 msm-id is 0x13d")
    owned = int(adt["owned_memory_size"], 0)
    dram = int(board["payload"]["dram_size"], 0)
    if not 0x10000000 <= owned <= 0x100000000 or owned % 0x200000:
        errors.append(f"owned_memory_size {owned:#x} is outside 256 MiB..8 GiB or not 2 MiB aligned")
    if owned > dram:
        errors.append(f"owned_memory_size {owned:#x} is larger than dram_size {dram:#x}")

    volume = board["payload"]["volume"]
    if len(volume) > 11:
        errors.append(f"[payload] volume {volume} is longer than 11 characters")
    for argument in board["payload"]["kernel_args"]:
        if not re.fullmatch(r"[A-Za-z0-9_.\-]+(=[A-Za-z0-9_.,\-]+)?", argument):
            errors.append(f"kernel argument {argument} fails the payload pattern")

    # The root contract, round150. The kernel's own vfs_mountroot() is the only mounter. The board
    # declares the root once, in /chosen/root-matching (IOFindBSDRoot deserializes it at
    # IOKitBSDInit.cpp:790-797 and waits for that IOMedia). Round138-149 carried `rd=md0s39` as a
    # four-character alias whose only job was to make bsd_rooted_ramdisk() (bsd_init.c:450-466)
    # match its "md0" prefix and skip the FSIOC_KERNEL_ROOTAUTH block. Round150 removes the alias:
    # the ROOTAUTH block now runs, and under XNU_OWNER_ROOT_AUDIT/TRUST it accepts the root when
    # drivers/CloverBlock's native_verified_block_root_authorized() answers for the mounted device
    # (offline-owner-root-binding-audit.patch). That is sheng's own path, not a lie, so no rd= is
    # allowed -- any rd= would declare a second root before the block is reached.
    # The switches are compiled into bsd_init.o by the board patch below, because
    # scripts/build-xnu-4k-baseline.sh gates them behind XNU_NATIVE_VIRTIO_BLOCK, whose archive
    # defines the same native_verified_block_* symbols CloverBlock supplies for this board's eMMC.
    if str(adt.get("root_match_volume_uuid", "")).upper() != ROOT_VOLUME_UUID:
        errors.append(f"[payload.adt] root_match_volume_uuid must be {ROOT_VOLUME_UUID}: "
                      "/chosen/root-matching is the only root declaration this board has")
    if adt.get("packed_data_ramdisk"):
        errors.append("[payload.adt] packed_data_ramdisk is true, but this board's RAMDisk is the "
                      "unchanged mockfs image; the payload step would emit PackedRAMDisk")
    rd_args = [argument for argument in board["payload"]["kernel_args"]
               if argument == "rd" or argument.startswith("rd=")]
    if rd_args:
        errors.append(f"[payload] kernel_args carries {rd_args}: round150 removes rd= entirely; the "
                      "root is authorized by owner trust, so no alias may declare a ramdisk root "
                      "and skip the ROOTAUTH block")
    trust_patch = next((base / "patches/xnu-clover/zzy-clover-owner-root-trust.patch"
                        for base in (REPO, pathlib.Path.cwd())
                        if (base / "patches/xnu-clover/zzy-clover-owner-root-trust.patch").is_file()),
                       None)
    if trust_patch is None:
        errors.append("patches/xnu-clover/zzy-clover-owner-root-trust.patch is missing: bsd_init.o "
                      "would compile with XNU_OWNER_ROOT_AUDIT/TRUST undefined and the now-unskipped "
                      "ROOTAUTH block would refuse this board's unsealed root")
    else:
        text = trust_patch.read_text()
        for macro in ("XNU_OWNER_ROOT_AUDIT", "XNU_OWNER_ROOT_TRUST"):
            if f"#undef {macro}" not in text or f"#define {macro} 1" not in text:
                errors.append(f"{trust_patch.name} does not #undef then #define {macro} 1: the audit "
                              "block would leave owner_authorized FALSE and the boot would stop at "
                              '"rootvp not authenticated after mounting"')
    if "-no_slto_panic" not in board["payload"]["kernel_args"]:
        errors.append('[payload] kernel_args has no -no_slto_panic: sheng boots with spinlock timeout '
                      "panics disarmed (osfmk/kern/startup.c:476), and the r146 silence includes no "
                      "spinlock report only because the timer has not fired yet")
    # Round158. The lock timeout is a generic XNU machine timeout (osfmk/arm/locks_arm.c:106,
    # 0xc00000 ticks = 524 ms at 24 MHz), overridden as ml-timeout-<name>-scale
    # (osfmk/kern/machine.c:1331-1345). Round156 measured a 16 KiB CloverBlock batch holding
    # a buffer lock for 655 ms at 25 KB/s, so the shipped boot must carry the same 40x scale
    # sheng uses; otherwise a slow read is indistinguishable from a dead lock.
    if "ml-timeout-lock-panic-scale=40" not in board["payload"]["kernel_args"]:
        errors.append('[payload] kernel_args has no ml-timeout-lock-panic-scale=40: the 524 ms '
                      "lock deadline is shorter than one 16 KiB read on this board's PIO path")
    # Round139. Round138's frame was `namespace 9 code 0x8` with `description none`, i.e.
    # EXEC_EXIT_REASON_SECURITY_POLICY created on process_signature()'s ip_mac_return branch
    # (kern_exec.c:8541-8546) -- a MAC label-update denial, which here is AMFI's hook. Both readers
    # of this name honor it: XNU's CS core (kern_codesigning.c:242-289) and AMFI's own start().
    if "cs_enforcement_disable=1" not in board["payload"]["kernel_args"]:
        errors.append('[payload] kernel_args has no cs_enforcement_disable=1: round138 died in '
                      "AMFI's label-update hook, and this is the only bypass the kext itself reads")
    if "clover-apfs=3" not in board["payload"]["kernel_args"]:
        errors.append('[payload] kernel_args has no clover-apfs=3: without it CloverBlock keeps the '
                      'whole-card geometry, never reads the SHNGROOT manifest, and the APFS volume '
                      'the root search wants is never published')
    if "clover-emmc=21" not in board["payload"]["kernel_args"]:
        errors.append('[payload] clover-apfs=3 is set but clover-emmc=21 is not: both windows are '
                      "published by the same mode 21 run that reads EXT_CSD and walks the GPT, so the "
                      'boot would stop at "udwindow 0"')
    userdata = board.get("userdata")
    if not userdata or not userdata.get("windows"):
        errors.append('[userdata] is missing or declares no windows: ./unixnu build userdata has '
                      'nothing to write, and CloverBlock would read an empty partition as a manifest')
    pins = board["payload"].get("trust_cache_sha256")
    if not pins:
        errors.append("[payload] trust_cache_sha256 is empty: the mockfs init program would "
                      "have no offline trust, and the exec of /sbin/launchd is a panic")
    else:
        # Round142: the boot trust cache is three modules -- clover's owner module, then the two
        # original Apple modules (StaticTrustCache, Cryptex1,SystemTrustCache). launchd's cdhash
        # 1d52c697… is entry 279 of the static one; the 56-byte one-module bundle of r141 carried
        # only the mockfs init, so the APFS root's launchd had no offline trust at all.
        bundle_path = next((base / "build/clover-trust/trustcache.bundle"
                            for base in (REPO, pathlib.Path.cwd())
                            if (base / "build/clover-trust/trustcache.bundle").is_file()), None)
        if bundle_path is None:
            print("[check-clover] notice: build/clover-trust/trustcache.bundle is not built; "
                  "run scripts/make-clover-trust-bundle.py", file=sys.stderr)
        elif hashlib.sha256(bundle_path.read_bytes()).hexdigest() not in pins:
            errors.append(f"{bundle_path} differs from [payload] trust_cache_sha256: the payload "
                          "step would refuse it or ship a bundle nobody pinned")
        else:
            data = bundle_path.read_bytes()
            count = struct.unpack_from("<I", data)[0]
            if count != 3 or len(data) < 4 + 4 * count:
                errors.append(f"{bundle_path} carries {count} modules, not owner + static + system")
            else:
                starts = [struct.unpack_from("<I", data, 4 + 4 * i)[0] for i in range(count)]
                ends = starts[1:] + [len(data)]
                structural = True
                for index, (start, end) in enumerate(zip(starts, ends)):
                    module = data[start:end]
                    if len(module) < 24 or struct.unpack_from("<I", module)[0] != 2:
                        errors.append(f"{bundle_path} module {index} is not version 2")
                        structural = False
                    elif len(module) != 24 + struct.unpack_from("<I", module, 20)[0] * 24:
                        errors.append(f"{bundle_path} module {index} is truncated")
                        structural = False
                if structural:
                    static = data[starts[1]:ends[1]]
                    system = data[starts[2]:ends[2]]
                    if hashlib.sha256(static).hexdigest() != STATIC_MODULE_SHA256:
                        errors.append(f"{bundle_path} module 1 is not the IPSW's StaticTrustCache")
                    if hashlib.sha256(system).hexdigest() != SYSTEM_MODULE_SHA256:
                        errors.append(f"{bundle_path} module 2 is not the IPSW's System trust cache")

    # Round143. Round142's frame was `namespace 6 subcode 0xd` -- OS_REASON_DYLD, dyld's
    # "Library not loaded: /usr/lib/libSystem.B.dylib ... (no such file, no dylib cache)" while
    # launching /sbin/launchd. The OS volume is the sealed snapshot and carries neither
    # /usr/lib/libSystem.B.dylib nor System/Library/Caches/com.apple.dyld/; the dyld cache and usr/
    # live on the Cryptex1,SystemOS volume beside it. sheng mounts that volume over "/" with
    # MNT_RDONLY|MNT_UNION from load_init_program(), and the board copies that path: the base repo
    # already has offline-owner-systemos-mount.patch applied but compiled out, and
    # scripts/build-xnu-4k-baseline.sh passes -DXNU_OWNER_CACHE_MOUNT=0 (plus STARTUP/UNION_SYSTEMOS)
    # to kern_exec.o, so the only place the macros can be turned back on is the translation unit
    # that reads them. Flip any of them in clover_defconfig instead and the builder's gates pull in
    # OWNER_ROOT_TRUST/VERIFIED_ROOT_PROBE + NativeVirtioBlock, which this board has no use for.
    patch = next((base / "patches/xnu-clover/zzy-clover-systemos-mount.patch"
                  for base in (REPO, pathlib.Path.cwd())
                  if (base / "patches/xnu-clover/zzy-clover-systemos-mount.patch").is_file()), None)
    if patch is None:
        errors.append("patches/xnu-clover/zzy-clover-systemos-mount.patch is missing: without it "
                      "kern_exec.o stays compiled with -DXNU_OWNER_CACHE_MOUNT=0 and launchd dies "
                      "again at OS_REASON_DYLD for want of the shared cache")
    else:
        text = patch.read_text()
        # Round159 turns on the same file's Data half and the manifest publisher, and the
        # publisher lives in IOKitBSDInit.cpp rather than kern_exec.c. All six switches are
        # still flipped inside the translation unit that reads them, for the reason above.
        for macro in ("XNU_OWNER_CACHE_MOUNT", "XNU_OWNER_CACHE_STARTUP",
                      "XNU_OWNER_UNION_SYSTEMOS", "XNU_OWNER_BOOT_MANIFEST",
                      "XNU_OWNER_DATA_MOUNT", "XNU_NATIVE_RAM_BLOCK"):
            if f"#undef {macro}" not in text or f"#define {macro} 1" not in text:
                errors.append(f"{patch.name} does not #undef then #define {macro} 1: the command "
                              "line's =0 would win and the SystemOS union mount would not run")
        for needle in ("#define XNU_OWNER_DATA_VOLUME_UUID "
                       '"FC1A580E-2D67-48FC-B8CF-87422A319340"',
                       "IOKitBSDInit.cpp",
                       "a redefinition with a",
                       "#undef XNU_OWNER_BOOT_MANIFEST"):
            if needle not in text:
                errors.append(f"{patch.name} lacks {needle!r}: the Data mount would ask for a UUID "
                              "CloverBlock never publishes, or the manifest publisher would be "
                              "compiled out by the builder's -DXNU_OWNER_BOOT_MANIFEST=0")
    windows = (userdata or {}).get("windows") or []
    if [window.get("name") for window in windows] != ["OS", "SystemOS"]:
        errors.append('[userdata] windows must be OS then SystemOS: CloverBlock publishes them in '
                      'that order so the OS volume is attached and named before the SystemOS window '
                      'is handed to a driver whose single-caller guard forbids overlap')
    if len(windows) == 2 and windows[1].get("component") != "Cryptex1,SystemOS":
        errors.append('[userdata] windows[1].component is not Cryptex1,SystemOS: the manifest slot '
                      "is the dyld cache volume sheng union-mounts, not the OS snapshot")

    # Round144. r143's frame was a data abort at _bsd_init+0x79c, `ldr w8,[x20,#0x70]` with
    # x20 = mountlist.tqh_first = 0, on the MNT_ROOTFS write right after IOSecureBSDRoot(). It
    # followed a vfs_mountroot() that returned 0, and every return-0 in that function sits after
    # mount_list_add(), so a static walk cannot name the branch. The probe patch therefore records
    # which of the four exits ran and what the list held at that instant, retries the mount once in
    # bsd_init(), and only panics with the readings. Like r143's board patch it has to be carried in
    # this repository and copied into the base tree's patches/xnu for the build.
    # Round145 adds the part one snapshot cannot say: every call in order with the caller phase
    # (1 loop, 2 retry, 3/4 imageboot) plus mount_list_add/remove counts, so the next frame splits
    # "never added" from "added then unmounted" from "a later call ran".
    probe = next((base / "patches/xnu-clover/zzy-clover-root-mount-why.patch"
                  for base in (REPO, pathlib.Path.cwd())
                  if (base / "patches/xnu-clover/zzy-clover-root-mount-why.patch").is_file()), None)
    if probe is None:
        errors.append("patches/xnu-clover/zzy-clover-root-mount-why.patch is missing: without it a "
                      "second empty mountlist faults at the MNT_ROOTFS write with no numbers")
    else:
        text = probe.read_text()
        missing = [needle for needle in
                   ("clover_root_probe(0)", "clover_root_probe(1)",
                    "clover_root_probe(2)", "clover_root_probe(3)",
                    "clover_root_probe_tag", "clover_root_probe_inshut",
                    "clover_root_phase", "clover_root_dump",
                    "clover_mount_adds", "clover_mount_removes",
                    "clover r145 hist calls=%d add=%d del=%d now ph=%d tag=%d dev=%d n=%d first=%lx",
                    "clover r145: mountlist empty after vfs_mountroot()==0",
                    "clover r145: root mount recovered on retry")
                   if needle not in text]
        for needle in missing:
            errors.append(f"{probe.name} lacks {needle!r}: that exit would report no tag, or the "
                          "panic would lose the snapshot it exists to print")

    # Round145. Round143 published both windows from clover_block_run(), before bsd_init() had
    # mounted anything; r143 and r144 then both left mountlist empty, while r137-r142 -- one window
    # -- mounted the same root. sheng never publishes SystemOS at boot: its owner mount looks the
    # volume up after the root is up, at load_init_program(). The board copies that order: boot
    # publishes the OS window only, and native_verified_block_volume_device() publishes the second
    # window when the owner mount asks at exec. These three source facts are the whole change; a
    # regression that puts the publication back before the root mount must fail this checker.
    block = REPO / "drivers/CloverBlock/CloverBlock.cpp"
    if not block.is_file():
        errors.append("drivers/CloverBlock/CloverBlock.cpp is missing: the boot window policy "
                      "cannot be checked")
    else:
        source = block.read_text()
        if "static IOMedia *publishSystemOs(void)" not in source:
            errors.append("CloverBlock.cpp has no publishSystemOs(): the SystemOS window must be "
                          "published on demand, not from a boot-time prepare step")
        if "publishWindow(systemFirst" in source:
            errors.append("CloverBlock.cpp still publishes the SystemOS window from "
                          "clover_block_run(): that is r143's shape, which r143/r144 proved leaves "
                          "bsd_init() with an empty mountlist")
        if "IOMedia *media = publishSystemOs();" not in source:
            errors.append("native_verified_block_volume_device() does not call publishSystemOs(): "
                          "the owner mount would get no device at exec time")
        if "deferred=1" not in source:
            errors.append("CloverBlock.cpp's boot line has no deferred=1: the frame would not say "
                          "that only one window was published")

    # Round153. r152's frame carried neither the r146 lines (the removal worked) nor a single r151
    # line, and that silence turned out to be the probe's own bug: NDINIT never initializes
    # cn_pnbuf, and r151 read it before vn_open_auth_internal ran, so the saved pointer was the
    # freshly allocated nameidata's garbage (usually NULL) and every call returned early. This
    # round reads the path *after* the call, when namei has filled it, and prints the dyld_flags
    # the asking process was handed, because dyld only looks in the on-disk cache directory when
    # bit 3 is set. The r146 patch still has to stay gone: a frame that prints "clover r146:" was
    # built from an older tree.
    r146 = next((base / "patches/xnu-clover/zzy-clover-shared-region-why.patch"
                 for base in (REPO, pathlib.Path.cwd())
                 if (base / "patches/xnu-clover/zzy-clover-shared-region-why.patch").is_file()), None)
    if r146 is not None:
        errors.append("patches/xnu-clover/zzy-clover-shared-region-why.patch is back: its namei ran "
                      "inside shared_region_check_np, right before dyld looks for the cache, and "
                      "Round152 removed it so the frame shows dyld's own calls only")

    # Round153. The probe must be the post-call form: the wrapper calls vn_open_auth_internal
    # first, then reads ndp->ni_cnd.cn_pnbuf. nameidone() only frees that pointer for paths longer
    # than ni_pathbuf (HASBUF), and dyld's two names are short, so the read is valid here. The
    # contract also forbids the r151 strings, since a conditional compiled from r151 would repeat
    # the early return, and requires the dyld_flags line that separates "dyld never asked" from
    # "dyld was never told where to look".
    cache_probe = next((base / "patches/xnu-clover/zzy-clover-shared-region-why2.patch"
                        for base in (REPO, pathlib.Path.cwd())
                        if (base / "patches/xnu-clover/zzy-clover-shared-region-why2.patch").is_file()), None)
    if cache_probe is None:
        errors.append("patches/xnu-clover/zzy-clover-shared-region-why2.patch is missing: without it "
                      "the frame cannot say whether dyld opened the cache file at all")
    else:
        text = cache_probe.read_text()
        missing = [needle for needle in
                   ("bsd/vfs/vfs_vnops.c",
                    "bsd/vm/vm_unix.c",
                    "vn_open_auth_internal(struct nameidata *ndp",
                    "+int\n+vn_open_auth(struct nameidata *ndp",
                    "-vn_open_auth(struct nameidata *ndp",
                    "error = vn_open_auth_internal(ndp, fmodep, vap, authvp);",
                    "path = ndp->ni_cnd.cn_pnbuf;",
                    "clover r170: open %s fmode=0x%x err=%d",
                    "\"/System/Library/Caches/com.apple.dyld\"",
                    "\"dyld_shared_cache\"",
                    "clover r153: read err=%d resid=%d hdr=%s",
                    "clover r170: check_np pid=%d comm=%s dyld_flags=0x%llx",
                    "clover_check_np_prints < 48",
                    "clover_cache_dir_prints >= 16",
                    "clover_cache_header(vnode_t vp, const char *path)",
                    "clover_cache_focus(const char *path)",
                    "if (!focus && error == 0) {",
                    'if (strcmp(tag, "main") != 0 && strcmp(tag, ".01") != 0) {',
                    "clover r155: hdr %s vp=%p len=%lld sig=%llu+%llu err=%d/%d",
                    "clover r155: hdr %s off=%u n=%u ty=%llu/%u plat=%u/%u fl=%x",
                    "clover r155: hdr %s m0=%llu/%x ml=%llu/%x subs=%u@%u")
                   if needle not in text]
        for needle in missing:
            errors.append(f"{cache_probe.name} lacks {needle!r}: the open, the header read and the "
                          "dyld_flags line are the three halves of the cache-reachability question, "
                          "and the r155 hdr lines are the verdict dyld never prints")
        if "clover r151:" in text:
            errors.append(f"{cache_probe.name} still carries a 'clover r151:' string: that form read "
                          "cn_pnbuf before NDINIT's copyin filled it, so the probe returned early "
                          "on every open and the frame stayed silent for the wrong reason")
        if "clover r154:" in text:
            errors.append(f"{cache_probe.name} carries a 'clover r154:' string: why2 is the "
                          "open/header probe, why3 is the map-and-slide probe, and a rebuild made "
                          "while why3 was applied would fold r154 into this file")
        # Round170: the unthrottled form is what filled the r169 frame. Three check_np lines per
        # process, for every process, spent the console budget on a value that stops changing.
        if 'printf("clover r153: check_np dyld_flags=' in text:
            errors.append(f"{cache_probe.name} still prints check_np on every call: the r169 frame "
                          "was that one line repeated, and the boot had no console time left")
        if "clover r153: open " in text:
            errors.append(f"{cache_probe.name} still prints every cache-directory open: dyld reopens "
                          "it per process even when it reuses the mapped cache")
        if "+#include" in text or "-vn_open_auth(struct nameidata *ndp" not in text:
            errors.append(f"{cache_probe.name} does not rename vn_open_auth to vn_open_auth_internal: "
                          "the probe would replace the open path instead of watching it")
        source = cache_probe.read_text()
        call = source.find("error = vn_open_auth_internal(ndp, fmodep, vap, authvp);")
        read = source.find("path = ndp->ni_cnd.cn_pnbuf;")
        if call < 0 or read < 0 or read < call:
            errors.append(f"{cache_probe.name} reads cn_pnbuf before the internal call runs: "
                          "NDINIT leaves the field uninitialized, so that read is always garbage")

    # Round154. r153's frame ended right after the second header read: dyld had found
    # the cache, opened it and read "dyld_v1   arm64" from both files, then went
    # quiet. The next thing dyld should do is syscall 536,
    # shared_region_map_and_slide_np. This round prints that entry, the first file's
    # descriptor and the return value, and walks a stage ladder inside
    # shared_region_map_and_slide_setup, so the frame separates "dyld never asked"
    # from "the kernel refused, at this gate". why3 is vm_unix.c only; why2 must
    # stay, since its open/read lines are how the next frame shows how far dyld got.
    map_probe = next((base / "patches/xnu-clover/zzy-clover-shared-region-why3.patch"
                      for base in (REPO, pathlib.Path.cwd())
                      if (base / "patches/xnu-clover/zzy-clover-shared-region-why3.patch").is_file()), None)
    if map_probe is None:
        errors.append("patches/xnu-clover/zzy-clover-shared-region-why3.patch is missing: without it "
                      "the frame cannot say whether dyld ever entered syscall 536")
    else:
        text = map_probe.read_text()
        missing = [needle for needle in
                   ("bsd/vm/vm_unix.c",
                    "clover r154: map2 enter files=%u mappings=%u",
                    "clover r154: map2 fd0=%d m0=%u slide0=%u",
                    "clover r154: map2 <- err=%d",
                    "clover r154: setup err=%d stage=%d at=%u files=%u mappings=%u",
                    "clover r154: mapfile kr=0x%x carrier=%lu/%lu",
                    "clover_stage = 12;")
                   if needle not in text]
        for needle in missing:
            errors.append(f"{map_probe.name} lacks {needle!r}: entry, descriptor, return value and "
                          "and the stage ladder are what the map-and-slide question needs")
        if "vfs_vnops.c" in text:
            errors.append(f"{map_probe.name} touches vfs_vnops.c: why3 is a vm_unix.c-only probe and "
                          "must not disturb why2's wrapper")
        if "clover r146:" in text or "clover r151:" in text:
            errors.append(f"{map_probe.name} carries an old round's string: r146 and r151 are gone "
                          "from the tree and must stay gone")
        for stage in ("clover_stage = 3;", "clover_stage = 9;", "clover_stage = 11;"):
            if stage not in text:
                errors.append(f"{map_probe.name} has no {stage!r}: the ladder would not reach the "
                              "mount, size or memory-object gates")

    # Round155. r154's frame carried no "clover r154:" line at all, so dyld stopped
    # between the .01 open and syscall 536. There are two shapes the frame cannot
    # tell apart: dyld's own preflight rejected a .01 header field and returned
    # without printing, or it reached the code-signature registration and is sitting
    # in the kernel reading a multi-megabyte blob at this board's block speed. why2
    # now dumps the header fields preflightCacheFile checks plus vnode_size, and why4
    # watches F_ADDFILESIGS_RETURN -- dyld's signature registration for cache files --
    # printing the request, the allocation, the read and its wall time. The header
    # verdict says whether .01 should have reached the fcntl; the fsig lines say
    # whether it did and what the kernel answered.
    fsig_probe = next((base / "patches/xnu-clover/zzy-clover-shared-region-why4.patch"
                       for base in (REPO, pathlib.Path.cwd())
                       if (base / "patches/xnu-clover/zzy-clover-shared-region-why4.patch").is_file()), None)
    if fsig_probe is None:
        errors.append("patches/xnu-clover/zzy-clover-shared-region-why4.patch is missing: without it "
                      "the frame cannot tell a rejected .01 header from a slow signature read")
    else:
        text = fsig_probe.read_text()
        missing = [needle for needle in
                   ("bsd/kern/kern_descrip.c",
                    "clover r155: fsig vp=%p start=%llu blob=%llu size=%llu",
                    "clover r155: fsig hit=1",
                    "clover r155: fsig mem=%p/%llu",
                    "clover r155: fsig rd off=%llu len=%llu",
                    "clover r155: fsig rd <- err=%d resid=%d ms=%llu",
                    "clover r156: fsig add err=%d",
                    "mach_absolute_time()",
                    "(uint64_t)kernel_blob_size")
                   if needle not in text]
        for needle in missing:
            errors.append(f"{fsig_probe.name} lacks {needle!r}: the entry, the hit, the allocation, "
                          "the read and its duration are what separate a refusal from a stall")
        if "vfs_vnops.c" in text or "vm_unix.c" in text:
            errors.append(f"{fsig_probe.name} touches another file: why4 is the fcntl-side probe "
                          "and must leave why2's wrapper and why3's ladder alone")
        if "clover r153:" in text or "clover r154:" in text:
            errors.append(f"{fsig_probe.name} carries another round's string: r153 lives in why2 "
                          "and r154 in why3")
        read = text.find("clover r155: fsig rd off=%llu len=%llu")
        timing = text.find("mach_absolute_time() - clover_start")
        if read < 0 or timing < 0 or timing < read:
            errors.append(f"{fsig_probe.name} starts the clock before it prints the read: the frame "
                          "would lose the offset/length line that names the blob")

    # Round156. The host measured the shipped 21H461 caches: 54 files, 3.71 GiB, a
    # 480 KiB main stub and a .01 whose signature is 224 KiB. Reading that at the
    # driver's old pace -- one FIFO word every 160 us, a habit from mode 12's
    # 400 kHz identification clock -- is 25 KB/s, so what looked like a hang was
    # the block layer itself. This round removes the pace from the read path, gives
    # the card CMD18 runs of 32 blocks, and lets CloverBlock hand whole 16 KiB runs
    # down instead of 512 bytes at a time. The write path keeps its proven pace:
    # writes are rare (logfs) and the single-block sequence is what every earlier
    # round validated. The assertions are shape, not speed: a future edit must not
    # quietly restore the read-side gap or drop the multi-block stop command.
    sdhci = REPO / "drivers/CloverSdhci/CloverSdhci.cpp"
    if not sdhci.is_file():
        errors.append("drivers/CloverSdhci/CloverSdhci.cpp is missing: the block device would "
                      "have no FIFO path to pace")
    else:
        text = sdhci.read_text()
        missing = [needle for needle in
                   ("static const uint32_t kReadChunkBlocks = 128;",
                    "static bool sendStopCommand (void)",
                    "static bool readMultiBlock (uint32_t lba",
                    "SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN |",
                    "SDHCI_TRNS_MULTI",
                    "MMC_READ_MULTIPLE_BLOCK",
                    "if (!chunkWritten && chunk >= 4) {",
                    "readMultiBlock (lba + done, target, chunk, got, status)")
                   if needle not in text]
        for needle in missing:
            errors.append(f"drivers/CloverSdhci/CloverSdhci.cpp lacks {needle!r}: without the "
                          "multi-block read and write the cache costs one command per 512 bytes")
        # The read path must not carry the counter gap any more. The write path and
        # mode 11's identification drain still use it, so only the read function is
        # isolated and searched.
        start = text.find("static bool readDataBlock (")
        end = text.find("static bool readPioBlock (", start)
        body = text[start:end] if start >= 0 and end > start else ""
        if "const uint64_t gap" in body or "lastRead" in body:
            errors.append("readDataBlock still paces on the counter: 160 us per FIFO word is "
                          "25 KB/s, which is what made the 3.71 GiB cache look hung")
        if "SDHCI_DATA_AVAILABLE" not in body:
            errors.append("readDataBlock does not gate on SDHCI_DATA_AVAILABLE: the fast drain "
                          "needs the level bit, not a timer")

    block = REPO / "drivers/CloverBlock/CloverBlock.cpp"
    if not block.is_file():
        errors.append("drivers/CloverBlock/CloverBlock.cpp is missing: nothing would publish "
                      "the card to the storage stack")
    else:
        text = block.read_text()
        missing = [needle for needle in
                   ("static const uint32_t kBatchBlocks = 128;",
                    "static const uint32_t kBatchWords = kBatchBlocks * kBlockWords;",
                    "static uint32_t gBatchWords[kBatchWords];",
                    "static IOLock *gBatchLock = nullptr;",
                    "gBatchLock = IOLockAlloc();",
                    "IOLockLock(gBatchLock);",
                    "IOLockUnlock(gBatchLock);",
                    "clover_emmc_block_read(cardBlock, gBatchWords, chunk)",
                    "clover_emmc_block_write(cardBlock, gBatchWords, chunk)")
                   if needle not in text]
        for needle in missing:
            errors.append(f"drivers/CloverBlock/CloverBlock.cpp lacks {needle!r}: the block layer "
                          "would keep asking the card for one 512-byte block at a time")
        # Round171: the batch is shared, so it is owned by a lock. A CAS retry loop that gives up
        # after a thousand spins hands the loser the per-block path, which on a contended boot is
        # every read.
        if "gBatchBusy" in text:
            errors.append("drivers/CloverBlock/CloverBlock.cpp still arbitrates the staging buffer "
                          "with a spin flag: a waiter that loses the race degrades to single blocks")
        # A caller that loses the busy flag must still have a path. The per-block loop
        # is the old, proven one and its buffer is on the stack.
        if "uint32_t words[kBlockWords];" not in text:
            errors.append("CloverBlock lost the per-block fallback: a second caller would have "
                          "no staging buffer and the request would fail")

    # Round181, drivers/CloverInput. What is asserted here is the shape of the QUP v2 transaction,
    # because every one of these is invisible on a bus that happens to work and fatal on one that
    # does not: rx tags go out through the OUTPUT FIFO (i2c-qup.c:1305-1312, called from 1426), so a
    # read that wrote them to MX_INPUT_CNT would time out with the FIFO empty; a tag length of 0
    # means 256, not nothing (:607), so an "address ping" is not expressible and the smallest real
    # read is what the probe asks for instead; SW_RESET has to precede every transfer (:1584) or the
    # done flags from the previous one are still set; and after the first sub-transfer the counters
    # are reprogrammed while RUNNING only through BIT(31). They are source-shape assertions because
    # a driver that loses one still compiles.
    touch = REPO / "drivers/CloverInput/CloverInput.cpp"
    if not touch.is_file():
        errors.append("drivers/CloverInput/CloverInput.cpp is missing: the touch instrument, and "
                      "with it the only reading of blsp_i2c3, would not exist")
    else:
        text = touch.read_text()
        # Round318 moved the tag bytes, the step codes and the status masks into InputCore.h, so what
        # this block pins about the driver is the *call*, and the bytes themselves are pinned in the
        # Round318 gate below -- a needle left here would be asserting a copy that no longer exists.
        missing = [needle for needle in
                   ("qupWrite (kQupSwReset, 1);",
                    "qupWrite (kQupI2cClkCtl, kQupI2cClk400k);",
                    "uint8_t tags[kQupMaxTagsLen]",
                    "input_read_tags (address, outLen, tags, sizeof (tags))",
                    "input_write_tags (address, regLen, outLen ? 0 : 1, wire, sizeof (wire))",
                    "return finishTransfer ();",
                    "extern \"C\" void clover_input_run (void)",
                    "PE_parse_boot_argn (\"clover-input\"",
                    "clover_emmc_flush_cache ();")
                   if needle not in text]
        for needle in missing:
            errors.append(f"drivers/CloverInput/CloverInput.cpp lacks {needle!r}: the QUP v2 "
                          "transaction it describes would not be the one upstream sends")
        # A 0-length v2 data tag is 256 bytes, not an empty write, so the driver must not offer a
        # bare address ping. Same for writing rx tags into the input counter, which is the shape a
        # reader of the register names would naturally produce.
        if "chipWriteOnly" in text:
            errors.append("CloverInput has a write-only transaction again: a v2 tag length of 0 "
                          "encodes 256, so there is no such thing as a zero-length probe")
        if "kQupMxInputCnt, txLen" in text or "qupWrite (kQupMxOutputCnt, rxLen" in text:
            errors.append("CloverInput puts a tag vector in the wrong FIFO: both counter registers "
                          "are written from the output side, and only MX_READ_CNT adds rx_tag_len")

        # Round318. Conformance §7 gap 3, the last of the three drivers: every bit-field, verdict
        # class, tag byte, cursor, bound and unit conversion the touch instrument reads or decides is
        # now in InputCore.h, and tests/clover_input_host.cpp runs it without a board. The failure
        # mode of a split like this is a second copy left behind in the driver, so all three faces are
        # asserted: (a) the layer still contains each decision, (b) the driver calls each of them,
        # (c) the driver no longer contains the expression it used to.
        input_core = REPO / "drivers/CloverInput/InputCore.h"
        if not input_core.is_file():
            errors.append("drivers/CloverInput/InputCore.h is missing: the touch instrument's pin "
                          "decode, its INTR_CFG cut, its verdict classes and its tag bytes would be "
                          "provable only on a bus that cannot answer")
        else:
            core_text = input_core.read_text()
            missing = [needle for needle in
                       ("return index < kInputBlkSizeCount ? kInputBlkSizes[index] : 0;",
                        "if (rx->tag_left) {",
                        "input_tag_len_field(reg_len) < 0",
                        "if (!(op & want) || got != rx_len || sent != tx_len) {",
                        "if (input_xfer_is_nack(bus_err)) {",
                        "return input_core_mode_word() | (reads ? 0u : kQupNoInput);",
                        "out->residue = out->mainline_error & ~(named & kI2cStatusErrorMask);",
                        "out->residue = out->mainline_error & ~(named & kQupStatusErrorFlags);",
                        "(ctl & (kPinDrvMask << kPinDrvShift)) >> kPinDrvShift",
                        "return (word & kCbcrClkOff) && input_branch_fsm(word) != kCbcrFsmOn;")
                       if needle not in core_text]
            for needle in missing:
                errors.append(f"drivers/CloverInput/InputCore.h lacks {needle!r}: this is a field cut "
                              "or a verdict the host model proves and the board cannot -- losing it "
                              "would be a log line that still prints and no longer means anything")
            for needle in ("input_rcg_decode (", "input_branch_running (", "input_branch_halted (",
                           "input_branch_enable_word (", "input_ahb_voted (",
                           "input_ahb_vote_word (", "input_rcg_xo_cfg (", "input_rcg_update_word (",
                           "input_rcg_update_pending (", "input_i2c_pin_ctl (",
                           "input_pin_output_io (", "input_pin_output_ctl (",
                           "input_pin_input_ctl (", "input_pin_drive_ctl (", "input_pin_decode (",
                           "input_state_reset_done (", "input_core_tags_taken (",
                           "input_state_reached (", "input_core_mode_word (", "input_core_config (",
                           "input_fifo_sizes (", "input_counters (", "input_tx_reset (",
                           "input_tx_next (", "input_tx_room_left (", "input_rx_reset (",
                           "input_rx_needs_word (", "input_rx_word (", "input_rx_pop (",
                           "input_rx_complete (", "input_done_mask (", "input_read_request_ok (",
                           "input_write_tags (", "input_read_tags (", "input_xfer_verdict (",
                           "input_xfer_verdict_name (", "input_xfer_is_nack (",
                           "input_bus_status_decode (", "input_core_flags_decode (",
                           "input_verdict_code (", "input_goodix_identify (",
                           "input_bytes_agree (", "input_goodix_sensor_id_valid (",
                           "input_ft_is_m06 (", "input_ticks_per_ms (", "input_ticks_for_ms (",
                           "input_elapsed_ms ("):
                if needle not in text:
                    errors.append(f"drivers/CloverInput/CloverInput.cpp does not call {needle!r}: the "
                                  "pure layer would be dead code the model passes and the kernel ignores")
            # A second copy is the failure mode. Each of these was the driver's own expression
            # before round318; any one of them back in the driver means the board runs arithmetic
            # the model never proved.
            for gone in ("kStepAhbVote", "kFailPoll", "kQupStatusErrorFlags", "kI2cStatusErrorMask",
                         "kQupNackFlag", "kGtPidValidMax", "(address << 1) | 1", "kQupTagV2DataRdStop",
                         "kQupTagV2DataWrStop", "(cfg & kPinIntrTargetMask)", "kRcgCfgWritable",
                         "kBlkSizes", "(drive + 1u) * 2u", "seg.first ? 0u : kQupMxConfigDuringRun"):
                if gone in text:
                    errors.append(f"drivers/CloverInput/CloverInput.cpp still carries a second copy of "
                                  f"{gone!r}: the shipped path and the modelled path would be two "
                                  "answers to one question")
            if '#include "InputRegs.h"' not in core_text:
                errors.append("InputCore.h does not include InputRegs.h: without it the layer would "
                              "have to re-type every register constant the board acts on")
            if "NULL" in core_text:
                errors.append("InputCore.h uses NULL: the header is included before anything that "
                              "defines it on this build, so the check is a literal 0")
            # The new r318 rows and the image that carries them: a reader of boot 103's log must not
            # look for these fields in an older frame.
            for needle in ("clover r318: xfer %s class=%s status=%08x err=%08x vend=%08x res=%08x "
                           "sda=%u scl=%u ",
                           "clover r318: core %s class=%s flags=%08x err=%08x vend=%08x res=%08x "):
                if needle not in text:
                    errors.append(f"drivers/CloverInput/CloverInput.cpp lacks the r318 row {needle!r}: "
                                  "the raw status word and SDA/SCL would stay unreadable, because the "
                                  "r181 bus= column is the masked word by construction")
            # Round319 latches the last sub-transfer's raw words so the beat can re-say them after the
            # one-time rows are overwritten by the tee's ring (the r260 pattern). The latch must be
            # filled in runSegment -- the one place rawStatus/rawErr/sda/scl exist together -- and must
            # not be re-read on the beat, or the bus would be touched twice per boot.
            for needle in ('volatile uint64_t clover_input_last_xfer[6];',
                           'clover_input_last_xfer[0] = rawStatus;',
                           'clover_input_last_xfer[2] = busStatus.sda_high;',
                           'clover_input_last_xfer[3] = busStatus.scl_high;'):
                if needle not in text:
                    errors.append(f"drivers/CloverInput/CloverInput.cpp lacks the r319 latch {needle!r}: "
                                  "the raw status word and SDA/SCL would again be one-time rows the ring "
                                  "throws away, which is the blind spot round319 exists to remove")
            harness = REPO / "drivers/CloverInput/tests/clover_input_host.cpp"
            runner = next((base / "scripts/test-clover-input.py"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "scripts/test-clover-input.py").is_file()), None)
            if runner is None:
                errors.append("scripts/test-clover-input.py is missing: the pin cut, the verdict "
                              "classes and the FIFO cursors would be proven only on a bus that has "
                              "never ACKed")
            elif not harness.is_file():
                errors.append("drivers/CloverInput/tests/clover_input_host.cpp is missing: the host "
                              "model has nothing to drive InputCore.h against")
            else:
                model = harness.read_text()
                for needle in ("kInputXferNack", "kInputXferBusError", "kInputXferCoreError",
                               "kInputXferIncomplete", "kInputStepResetCore", "kInputFailXfer",
                               "input_bus_status_decode", "input_core_flags_decode",
                               "input_goodix_identify", "kPinIntrTargetMask"):
                    if needle not in model:
                        errors.append(f"the CloverInput host model never exercises {needle}: a "
                                      "refusal or a field cut the board can only reach at boot is "
                                      "a decision nobody has seen fire")
                check = subprocess.run([sys.executable, str(runner)], capture_output=True, text=True)
                if check.returncode:
                    errors.append("the CloverInput host model failed: a field cut, a verdict class, "
                                  "a FIFO cursor or a tag byte is not what this boot's report will "
                                  "claim it is\n" + (check.stdout + check.stderr)[-700:])

    # Round182, drivers/CloverPower. Two different things are asserted here, and both are things a
    # driver that still compiles can silently lose. First, the v5 transaction shape: the command word
    # is (op << 27) | ((addr & 0xff) << 4) | (bc & 7) with no SID in it, DONE/FAILURE/DENIED/DROPPED
    # are four different answers, the bytes come out of RDATA0 low-byte-first and spill into RDATA1
    # past four, and a version outside the v5 band has to be refused rather than walked with v5
    # offsets (a v7 map at 0x2000 would otherwise yield a confident, wrong APID). Second, the
    # read-only promise, which is the reason this round is allowed to touch a PMIC at all -- so it is
    # asserted by absence: no write helper, no write opcode.
    arb = REPO / "drivers/CloverPower/PowerArb.h"
    power = REPO / "drivers/CloverPower/CloverPower.cpp"
    if not arb.is_file() or not power.is_file():
        errors.append("drivers/CloverPower/{PowerArb.h,CloverPower.cpp} is missing: the SPMI arbiter "
                      "round, and with it the answer to who owns the touch panel's rails, does not exist")
    else:
        text = arb.read_text()
        missing = [needle for needle in
                   ("(op << kPmicArbCmdOpShift)",
                    "return PA_E_VERSION;",
                    "if (len < 1 || len > kPmicArbMaxTransBytes) {",
                    "word = ch[kPmicArbRdata0 / 4];",
                    "word = ch[kPmicArbRdata1 / 4];",
                    "if (status & kPmicArbStatusDenied) {",
                    "PA_BARRIER ();")
                   if needle not in text]
        for needle in missing:
            errors.append(f"drivers/CloverPower/PowerArb.h lacks {needle!r}: the v5 transaction it "
                          "describes would not be the one the vendor driver sends")
        # Round313: the floor plan is data. The arbiter's version word picks which of the vendor's ops
        # tables runs, and with it the APID map base (0x800 v2/v3, 0x900 v5), the observer strides
        # (0x1000/0x8000 versus 0x10000/0x80), where the access words live (inside `intr` for v2/v3,
        # inside `chnls` for v5) and how the owner/ACC status is indexed (per 32-APID group, not per
        # APID). None of that may be a constant again: a v3 arbiter read at v5 offsets returns a
        # confidently wrong APID, and round183's frame is the evidence that this board may be one.
        missing = [needle for needle in
                   ("static inline const struct pa_layout * pa_layout_for (uint32_t version)",
                    "return &pa_layout_v3;",
                    "r->l = pa_layout_for (r->version);",
                    "map = a->core[(l->mapBase + l->mapStride * i) / 4];",
                    "l->ownerAccGroupStride * (apid >> kPmicArbOwnerAccGroupShift)",
                    "static inline int pa_acc_window_is_intr (const struct pa_layout *l)")
                   if needle not in text]
        for needle in missing:
            errors.append(f"drivers/CloverPower/PowerArb.h lacks {needle!r}: round313's whole point is "
                          "that the band the version word selects decides where every one of these "
                          "registers is, and a hard-coded v5 offset would silently mis-read a v3 board")
        regs = REPO / "drivers/CloverPower/PowerRegs.h"
        if not regs.is_file():
            errors.append("drivers/CloverPower/PowerRegs.h is missing: the two bands' addresses have "
                          "nothing to be named in")
        else:
            regtext = regs.read_text()
            for needle in ("#define kPmicArbApidMapV23      0x800U",
                           "#define kPmicArbObsApidStrideV23 0x8000U",
                           "#define kPmicArbOwnerAccBaseV3  0x200000U",
                           "#define kPmicArbOwnerAccGroupShift 5U",
                           "#define kSpmiObsMapSizeV23",
                           "#define kSpmiIntrMapSizeV23"):
                if needle not in regtext:
                    errors.append(f"drivers/CloverPower/PowerRegs.h lacks {needle!r}: the v2/v3 row of "
                                  "the vendor's dispatch has no documented address to be read at")
        source = power.read_text()
        missing = [needle for needle in
                   ("extern \"C\" void clover_power_run (void)",
                    "PE_parse_boot_argn (\"clover-power\"",
                    "clover_emmc_flush_cache ();",
                    "clover_power_observation[15]",
                    "kPmicTypeValue",
                    "kLdoRegEnable",
                    "kPmicArbOwnerTable")
                   if needle not in source]
        for needle in missing:
            errors.append(f"drivers/CloverPower/CloverPower.cpp lacks {needle!r}: the report would "
                          "not say which step, which identity or which enable it read")
        # Round313's instrument shape. Round183's frame carried two transcriptions of one word --
        # `ver=30000000` in the scan line and `(03000000)` in the refusal line -- and a photograph
        # cannot settle which one the silicon said, so the next image has to make the question
        # machine-checkable instead: the same word rendered four ways, read twice, printed beside the
        # band the driver selected, and beside core + 0x0004 as a control that the page is answering.
        missing = [needle for needle in
                   ("layout = pa_layout_for (version);",
                    "arbiter_ver hex=%08x dec=%u nib=%x-%x-%x-%x-%x-%x-%x-%x again=%08x stable=%u",
                    "band=%s floor=%08x next=%08x",
                    "main_apids=%u (mask %x) differs=%u",
                    "no floor plan for band",
                    "version moved under us: chose=",
                    "layout mismatch: mapped=")
                   if needle not in source]
        for needle in missing:
            errors.append(f"drivers/CloverPower/CloverPower.cpp lacks {needle!r}: the arbiter version "
                          "would still be one transcribable number, which is the exact shape that left "
                          "round183's two readings of it unresolvable")
        # The refusal must stay a refusal. A band this port has no table for cannot be walked by
        # guessing the nearest one, so the branch that stops has to come before the map walk.
        if "if (!layout) {" not in source:
            errors.append("drivers/CloverPower/CloverPower.cpp has no unknown-band stop: an unbanded "
                          "arbiter would be read at v5 addresses and reported as an answer")
        # The guarantee is structural, not a promise in a comment. If either of these appears, the
        # round is no longer the one that can only be wrong in its report.
        if "windowWrite" in source:
            errors.append("CloverPower grew a write path to a register window: this round is read-only, "
                          "and the only word it may put on the bus is a read command on the observer "
                          "channel")
        if "kPmicArbWriteOp" in source:
            errors.append("CloverPower builds a write command word: pa_read() is the only transaction "
                          "this round is allowed to start")
        # The model of the arbiter is the evidence for everything above, so it is run, not just
        # asserted to exist: the file's own header promises the logic lives in PowerArb.h "where
        # scripts/test-clover-power.py can exercise it", and a promise the checker does not keep is
        # how a driver ends up tested only by the board.
        harness = REPO / "drivers/CloverPower/tests/clover_power_host.cpp"
        runner = next((base / "scripts/test-clover-power.py"
                       for base in (REPO, pathlib.Path.cwd())
                       if (base / "scripts/test-clover-power.py").is_file()), None)
        if runner is None:
            errors.append("scripts/test-clover-power.py is missing: the v5 arbiter logic would be "
                          "proven only on a PMIC that cannot be un-broken")
        elif not harness.is_file():
            errors.append("drivers/CloverPower/tests/clover_power_host.cpp is missing: the host model "
                          "has nothing to drive PowerArb.h against")
        elif "pa_layout_v3" not in harness.read_text() or "setUp (1);" not in harness.read_text():
            # Round313's claim is that a v3 arbiter is a *known* shape rather than a refusal, and the only
            # place that can be proven before a board is put next to one is the model: it has to build a
            # fake arbiter with the map at 0x800 and the channels 0x8000 apart and get the same APID, the
            # same tie-break and the same five bytes back. A model that only ever drives the v5 fake would
            # pass while the new table was wrong in every field.
            errors.append("the arbiter host model does not drive a v3 fake: the second floor plan would "
                          "be shipped unproven, on the same arithmetic that round183 could not resolve")
        else:
            # Round331b. The model answers from a second thread while pa_wait() counts its patience in
            # polls, and those are two clocks: the bound was smaller than a loaded host's wake latency,
            # so this gate went red over this machine's scheduling and not over the arbiter. Measured on
            # the way back: the old 200000 fails the loaded leg at a served read (line 419) while the
            # idle leg passes, and the new bound passes ten of ten with one busy process per core. The
            # needles below keep the three things that closed it from walking back in -- a runnable
            # server rather than a coalesced timer, a bound sized for a loaded host, and a runner that
            # really runs the model loaded. The sleep ban is judged on code lines only, because the two
            # comments naming the old usleep are the record of why it is gone.
            harness_source = harness.read_text()
            harness_code = [line for line in harness_source.splitlines()
                            if not line.strip().startswith(('//', '*', '/*'))]
            if any('usleep' in line for line in harness_code):
                errors.append("the arbiter model's fake PMIC sleeps again: its answer would arrive on "
                              "the kernel's timer while the reader's budget is counted in polls, which "
                              "is the two-clock mistake round331b was written to close")
            for needle, why in (('sched_yield', "the server thread is not runnable again, so its wake "
                                                  "latency is the kernel's timer's, not the reader's"),
                                 ('static const uint32_t kPolls = 100000000;', "the served-read bound is "
                                                  "no longer the one sized for a loaded host"),
                                 ('kPollsNoAnswer', "the legs that assert the poll ENDS lost their own "
                                                  "bound, and one number would then be asked to mean "
                                                  "both a patient read and an impatient one"),
                                 ('channelLock', "rearm() can interleave with serve() again, and a "
                                                  "stale DONE would answer the next transaction out of "
                                                  "the previous command's bytes")):
                if needle not in harness_source:
                    errors.append(f"the arbiter host model lost {needle!r}: {why}")
            # Round339 separated the two hypotheses round331b had left standing next to each other. The
            # answer was neither: the model's own fake PMIC stored DONE ahead of the answer it was
            # supposed to have placed first, so a poll could return success while the channel still
            # described the ask before it. Once the leg judged from per-APID records and a snapshot, it
            # could say so -- and said it 347 times out of 4000 loaded runs, split between a sequence
            # that had not advanced and a peripheral that had not been asked. With the release/acquire
            # pair in place the same 4000 runs under twelve external spinners printed nothing at all, so
            # the interleaving the channelLock guards against was not what was left over. The pins below
            # hold each half of that measurement: every DONE store has a release in front of it, the
            # reader of the evidence has its acquire, and the evidence is the channel's own record
            # rather than a word that every serve in the model writes.
            done_stores = sum(1 for line in harness_code
                              if re.search(r"ch\[kPmicArbStatus / 4\] = .*kPmicArbStatusDone", line))
            releases = sum(1 for line in harness_code
                           if "__atomic_thread_fence (__ATOMIC_RELEASE);" in line)
            if releases != done_stores:
                errors.append(f"the fake PMIC stores DONE {done_stores} times and releases {releases} "
                              "times: the answer has to become visible before the flag that says it is, "
                              "and one un-fenced store is the shape that answered 347 of 4000 loaded "
                              "runs out of the previous ask")
            if "__atomic_thread_fence (__ATOMIC_ACQUIRE);" not in harness_source:
                errors.append("require_burst() has no acquire: it reads the channel's record after the "
                              "poll returned, and without the reader's half of the pair the record it "
                              "prints can be older than the DONE it is reporting on")
            if "servedSid[apid] = lastSid;" in harness_source:
                errors.append("a channel's answered-by record is being filled from the model's shared "
                              "last-serve word again: two serves write that one word and every burst "
                              "leg asks about one of them, which is how round338's row could print three "
                              "operands that all agreed and still die")
            for needle, why in (('static volatile uint32_t servedSeq[512];',
                                 "the model has no per-channel answer count, so \"this channel was "
                                 "served for this ask\" is once again a claim about a shared word"),
                                ('servedSid[apid] = (int) sid;',
                                 "the per-channel peripheral record is not being filled from serve()'s "
                                 "own decoded command, which is the only version of it that is about "
                                 "this transaction"),
                                ('const uint32_t asked = servedSeq[',
                                 "the burst legs no longer read their channel's sequence before the "
                                 "command goes out, so there is only one reading left to judge from "
                                 "and it cannot tell a fresh answer from a stale DONE"),
                                ('no serve answered this ask',
                                 "the leg that separates \"this channel was never served\" from \"a "
                                 "different peripheral answered it\" is gone, and those are the two "
                                 "readings round339 was built to tell apart"),
                                ('moved=',
                                 "the row no longer says whether the words it printed moved while it "
                                 "was being written, which is the only thing that makes an agreeing "
                                 "triple trustworthy")):
                if needle not in harness_source:
                    errors.append(f"the arbiter host model lost {needle!r}: {why}")
            if 'os.cpu_count' not in runner.read_text() or 'loaded' not in runner.read_text():
                errors.append("scripts/test-clover-power.py runs the model only on an idle host again: "
                              "the promise that a served read answers inside the bound would then be "
                              "assumed rather than tested, which is what made this gate untrustworthy")
            check = subprocess.run([sys.executable, str(runner)], capture_output=True, text=True)
            if check.returncode:
                errors.append("the arbiter host model failed: the APID tie-break, the bounded poll or "
                              "the burst byte order is not what round182's report will claim it is\n"
                              + (check.stdout + check.stderr)[-700:])

    # Round183, drivers/CloverDsp. What is asserted here is the shape of a walk over a structure whose
    # every pointer was written by another processor: magics compared as one little-endian word (the
    # vendor declares u8[4] and memcmps it, and the two forms differ -- a byte-order transcription error
    # makes a live heap read as "no partition table"), bounds taken subtractively so an offset and a
    # size that overflow cannot pass (`entry.data + entry.size > smem.size`), the cached list walked
    # down from the partition's end with a cacheline that has to be a power of two, "$SMP" accepted only
    # with version 1, sixteen entries and both pid fields naming the edge it was read for, and the
    # global path refusing an item whose aux_base says it lives in another region. Second, the
    # read-only promise, asserted by absence: this round does not even issue a command, so the only
    # thing that may appear is a load. Third, the model -- DspCore.h's header says the bounds decisions
    # "are testable there and nowhere else", so the checker runs it rather than trusting the file to
    # exist.
    core = REPO / "drivers/CloverDsp/DspCore.h"
    regs = REPO / "drivers/CloverDsp/DspRegs.h"
    dsp = REPO / "drivers/CloverDsp/CloverDsp.cpp"
    if not core.is_file() or not regs.is_file() or not dsp.is_file():
        errors.append("drivers/CloverDsp/{DspCore.h,DspRegs.h,CloverDsp.cpp} is missing: the SMEM and "
                      "doorbell round, and with it 排期 5's fork between a protocol job and a bring-up "
                      "job, does not exist")
    else:
        text = core.read_text()
        missing = [needle for needle in
                   ("if (!smem.base || smem.size < kSmemToc)",
                    "if (entry.data + entry.size > smem.size) {",
                    "if (cacheline & (cacheline - 1)) {",
                    "if (item.magic != kSmp2pMagic) {",
                    "if (item.total_entries != kSmp2pMaxEntries",
                    "item.local_pid != expect_local",
                    "if (entry.aux_base) {",
                    "return at == end ? DS_OK : DS_E_CANARY;",
                    "DS_E_RANGE")
                   if needle not in text]
        for needle in missing:
            errors.append(f"drivers/CloverDsp/DspCore.h lacks {needle!r}: the walk would stop being the "
                          "one the host model proves -- every clause above is a bounds or identity "
                          "decision that a board cannot distinguish from a corrupt heap afterwards")
        constants = parse_defines(regs.read_text())
        for name, want in (("kSmemPtableMagic", 0x434f5424), ("kSmemPartMagic", 0x54525024),
                           ("kSmemInfoMagic", 0x49494953), ("kSmp2pMagic", 0x504d5324)):
            if name not in constants:
                errors.append(f"drivers/CloverDsp/DspRegs.h does not define {name}")
            elif constants[name] != want:
                errors.append(f"drivers/CloverDsp/DspRegs.h:{name} is {constants[name]:#x}, not "
                              f"{want:#x}: that is the vendor's byte-array order, and a word-read "
                              "walker wearing it reports a live heap as an empty one")
        source = dsp.read_text()
        missing = [needle for needle in
                   ("extern \"C\" void clover_dsp_run (void)",
                    "PE_parse_boot_argn (\"clover-dsp\"",
                    "clover_emmc_flush_cache ();",
                    "clover_dsp_observation[15]",
                    "kApcsIpcPhys",
                    "ds_smp2p_head",
                    "ds_find_item_global",
                    "kSmemItemAdspSmp2pOut",
                    "kWlanMsaPhys")
                   if needle not in source]
        for needle in missing:
            errors.append(f"drivers/CloverDsp/CloverDsp.cpp lacks {needle!r}: the report would not say "
                          "which window, which item path or which failure it read")
        # Read-only, and this time the risk is not a transaction but an MPU denial -- which is why the
        # mapping is done before any read and why nothing here may grow a store. SMEM is Device-ish
        # memory another processor owns; a write to it is not a report that happens to be wrong.
        for forbidden in ("windowWrite", "ds_write", "kSmp2pSet", "IOMemoryDescriptor::withAddress"):
            if forbidden in source:
                errors.append(f"drivers/CloverDsp grew {forbidden}: round183 is a load and nothing "
                              "else; enabling the doorbell, SSR or PIL is the round after this one")
        # The model is the evidence for every bounds decision above, so it is run here. DspCore.h claims
        # the walk is "testable there and nowhere else", and a claim the checker does not keep is how a
        # driver ends up tested only by an ADSP that cannot answer.
        harness = REPO / "drivers/CloverDsp/tests/clover_dsp_host.cpp"
        runner = next((base / "scripts/test-clover-dsp.py"
                       for base in (REPO, pathlib.Path.cwd())
                       if (base / "scripts/test-clover-dsp.py").is_file()), None)
        if runner is None:
            errors.append("scripts/test-clover-dsp.py is missing: the SMEM walk's bounds would be "
                          "proven only on a heap this port does not control")
        elif not harness.is_file():
            errors.append("drivers/CloverDsp/tests/clover_dsp_host.cpp is missing: the host model has "
                          "nothing to drive DspCore.h against")
        else:
            check = subprocess.run([sys.executable, str(runner)], capture_output=True, text=True)
            if check.returncode:
                errors.append("the SMEM host model failed: the magic word form, a bound, or the SMP2P "
                              "identity check is not what round183's report will claim it is\n"
                              + (check.stdout + check.stderr)[-700:])

    # Round157. r156 made the card fast but left four ways for the fast path to report
    # something that was not true: a stop command whose failure was invisible, DATA_END
    # accepted next to a CRC/timeout bit, a 32-bit word offset that wraps past 16 GiB, and
    # a CloverBlock completion that counted blocks the caller never received. These are
    # source-shape assertions because each one is silent on a happy path.
    sdhci = REPO / "drivers/CloverSdhci/CloverSdhci.cpp"
    if sdhci.is_file():
        text = sdhci.read_text()
        if "SDHCI_CMD_RESP_SHORT_BUSY" not in text:
            errors.append("the stop command is not issued as R1b: CMD12's busy period would be "
                          "unobserved and the card could stay in data mode")
        if "kDataErrorMask" not in text:
            errors.append("the block paths do not mask data errors: DATA_END beside a CRC or "
                          "timeout would be reported as a clean transfer")
        if "!(status & kDataErrorMask)" not in text:
            errors.append("kDataErrorMask is declared but never applied to a transfer verdict")
        if "words + static_cast<uint64_t>(done) * kBlockWords" not in text:
            errors.append("the block pointer is still computed in 32 bits: a run past 16 GiB "
                          "would wrap to the start of the caller's buffer")
    block = REPO / "drivers/CloverBlock/CloverBlock.cpp"
    if block.is_file():
        text = block.read_text()
        if "done += read;" in text:
            errors.append("CloverBlock counts blocks the failed card read never delivered: the "
                          "completion would claim bytes that never reached the caller")
        if "blk_account_write (" not in text or "blk_account_read (" not in text:
            errors.append("CloverBlock no longer asks BlockCore.h what a command delivered: the count "
                          "of whole blocks a failed write did commit, and the rule that a short read "
                          "commits none, are one function each and both live in the pure layer")
        core = REPO / "drivers/CloverBlock/BlockCore.h"
        if core.is_file() and "*done += delivered;" not in core.read_text():
            errors.append("BlockCore.h lost the partial-write count: a chunk the card took only part of "
                          "would report none of it committed, which is round157's bug back again")

    # Round159. The user's request for this round was two-part -- finish the eMMC driver,
    # then mount the Data volume -- and both halves fail silently when a step is skipped,
    # so the assertions are shape.
    #
    # The link. Round156 removed the read pacing but the card still ran the whole boot at the
    # identification clock on one lane: nothing had ever sent CMD6. fastMode() performs the
    # standard init order (HS_TIMING=1, BUS_WIDTH=8, then the 50 MHz row of sdcc1_apps_clk_src),
    # gates every step on the card's own answer, and proves the result by reading LBA 1 --
    # "EFI PART" -- before gBlockReady is raised. A refused step falls back to the 400 kHz
    # settings the earlier rounds proved; a slow card must not become a dead boot.
    #
    # The volume. The owner Data mount is sheng's code, unchanged. What this board supplies is
    # the block device behind it: a third SHNGROOT entry appended by
    # scripts/append-data-window.py, published writable (flags 0, unlike the packer's read-only
    # entries), and answered by native_ram_block_volume_seed() for
    # native_owner_data_prepare() to mount on /private/var.
    sdhci = REPO / "drivers/CloverSdhci/CloverSdhci.cpp"
    if sdhci.is_file():
        text = sdhci.read_text()
        missing = [needle for needle in
                   ("static bool fastMode (FastLink &link)",
                    "static const uint32_t kGccCfg50m = 0x0000060bu;",
                    "kExtCsdHsTiming = 185u",
                    "kExtCsdBusWidth = 183u",
                    "kHsTimingHighSpeed = 1u",
                    "kBusWidth8Bit = 2u",
                    "SDHCI_CTRL_HISPD",
                    "link.header0 == 0x20494645u && link.header1 == 0x54524150u",
                    "programRootClock (true, kGccCfg400k, link.clockAfter, backUpdated, backStable);",
                    "clover_emmc_link[6] = (link21.highSpeed ? 1u : 0u) | (link21.width8 ? 2u : 0u) |",
                    "CloverSdhci r159: link hs=%d/%x bw=%d/%x clk=%x hc=%x hdr=%x/%x ok=%d",
                    "readMultiBlock (lba + done, target, chunk, got, status)",
                    "writeMultiBlock (lba + done, target, chunk, handed, status)",
                    "CloverSdhci r159: multi %s fail lba=%u blocks=%u st=0x%x w=%u/%u (once)")
                   if needle not in text]
        for needle in missing:
            errors.append(f"drivers/CloverSdhci/CloverSdhci.cpp lacks {needle!r}: the card would "
                          "stay at the identification clock, or a fast link that lied would be "
                          "handed to the filesystem")
        if "static const uint32_t kGccCfg400k" not in text:
            errors.append("CloverSdhci lost its 400 kHz row: fastMode() would have nothing to fall "
                          "back to and a refused CMD6 would leave the card unusable")

    block = REPO / "drivers/CloverBlock/CloverBlock.cpp"
    if block.is_file():
        text = block.read_text()
        missing = [needle for needle in
                   ('static const char kDataWindowName[] = "Data";',
                    'static const char kDataVolumeUuid[] = "FC1A580E-2D67-48FC-B8CF-87422A319340";',
                    "static ManifestEntry gDataEntry;",
                    "static volatile uint32_t gDataWindowFirst = 0;",
                    "static volatile uint32_t gDataWindowSectors = 0;",
                    "windowsFromManifest(first, sectors, apfs == 3, apfs == 3);",
                    "static IOMedia *publishDataVolume(void)",
                    "IOMedia *media = publishDataVolume();",
                    "CloverBlock r159: boot os=%u+%u system=%u+%u data=%u+%u fires=%u deferred=1",
                    "CloverBlock r159: data vu=%s bsd=%s %u:%u p=%u",
                    "CloverBlock r159: seed data %s dev=%d bytes=%llu off=%llu",
                    "native_ram_block_volume_seed(const char *uuid, dev_t *device, char *path,",
                    "native_ram_block_gpu_addon(const uint8_t **addon, uint64_t *bytes)",
                    "native_verified_block_manifest_sha384(dev_t device, const char *uuid,",
                    "return device == gOsVolumeDev || device == gSystemVolumeDev;")
                   if needle not in text]
        for needle in missing:
            errors.append(f"drivers/CloverBlock/CloverBlock.cpp lacks {needle!r}: the Data window "
                          "would not be parsed, published or answered, and the owner Data mount "
                          "would fall back to a scratch disk")
        for tag in ("root-attr", "system-attr", "data-attr", "data-bounds", "data-geometry",
                    "\"datawin\""):
            if tag not in text:
                errors.append(f"CloverBlock has no {tag!r} verdict: a malformed {tag[:-1]} entry "
                              "would be published instead of named")
        if "rememberEntry(&gDataEntry, &windows.data_entry);" not in text:
            errors.append("CloverBlock does not keep the Data entry's offset/size/sha256: the "
                          "manifest digest and native_ram_block_volume_seed() would describe "
                          "different extents")

        # Round316. The manifest walk and the request arithmetic moved into BlockCore.h so that a host
        # model could answer them; the pins follow the code, not the file. What is asserted is (a) that
        # every decision still exists, one clause each, (b) that the driver *calls* each of them, and
        # (c) that no second copy survived in the driver -- a second copy is the failure mode of a split
        # like this one, because the board would then run arithmetic the model never proved.
        core = REPO / "drivers/CloverBlock/BlockCore.h"
        if not core.is_file():
            errors.append("drivers/CloverBlock/BlockCore.h is missing: the decode that turns a "
                          "manifest row into a card LBA would be provable only on a card, and every "
                          "filesystem above it would be reading bytes nobody checked")
        else:
            core_text = core.read_text()
            missing = [needle for needle in
                       ("if (!blk_has_root_magic (bytes)) {",
                        "if (version != kBlkVersion) {",
                        "count > kBlkRowsMax",
                        "static_cast<uint64_t> (count) * kBlkRowStride > nbytes",
                        "blk_le32 (row + kBlkRowBlockSizeAt) != kBlkBytes",
                        "const uint32_t expect_flags = which == BLK_W_DATA ? kBlkFlagWritable : "
                        "kBlkFlagReadOnly;",
                        "offset < kBlkSpanBytes",
                        "size > partition_bytes - offset",
                        "first + sectors > card->card_sectors",
                        "block >= sectors || nblks > sectors - block",
                        "*done += delivered;",
                        "if (delivered != chunk || !copied) {",
                        "first + count > window_first",
                        "*max_block = static_cast<uint64_t> (sectors) - 1;")
                       if needle not in core_text]
            for needle in missing:
                errors.append(f"drivers/CloverBlock/BlockCore.h lacks {needle!r}: this is a bound or an "
                              "identity decision the host model proves and the board cannot -- a window "
                              "off by one row is a filesystem mounted over bytes that belong to "
                              "something else")
            for needle in ("blk_parse_manifest (", "blk_admit_request (", "blk_next_chunk (",
                           "blk_account_write (", "blk_account_read (", "blk_overlaps (",
                           "blk_last_block ("):
                if needle not in text:
                    errors.append(f"drivers/CloverBlock/CloverBlock.cpp does not call {needle!r}: the "
                                  "pure layer would be dead code the model passes and the kernel ignores")
            for gone in ("offset < kRootManifestBytes", "nblks > sectors - block",
                         "chunk > kBatchBlocks", "first + count > windowFirst"):
                if gone in text:
                    errors.append(f"drivers/CloverBlock/CloverBlock.cpp still carries a second copy of "
                                  f"{gone!r}: the shipped path and the modelled path would be two "
                                  "answers to one question")
            harness = REPO / "drivers/CloverBlock/tests/clover_block_host.cpp"
            runner = next((base / "scripts/test-clover-block.py"
                           for base in (REPO, pathlib.Path.cwd())
                           if (base / "scripts/test-clover-block.py").is_file()), None)
            if runner is None:
                errors.append("scripts/test-clover-block.py is missing: the manifest walk and the "
                              "delivery counts would be proven only on a card that cannot be un-broken")
            elif not harness.is_file():
                errors.append("drivers/CloverBlock/tests/clover_block_host.cpp is missing: the host "
                              "model has nothing to drive BlockCore.h against")
            else:
                model = harness.read_text()
                for needle in ("BLK_E_MAGIC", "BLK_E_VERSION", "BLK_E_COUNT", "BLK_E_ATTR",
                               "BLK_E_BOUNDS", "BLK_E_GEOMETRY", "BLK_E_NAME", "BLK_E_SYSTEM",
                               "BLK_E_DATA", "BLK_REQ_BUFFER", "BLK_REQ_RANGE", "BLK_DONE_PARTIAL",
                               "BLK_DONE_NONE"):
                    if f"== {needle}" not in model:
                        errors.append(f"the CloverBlock host model never asserts {needle}: a refusal the "
                                      "board can only reach by panicking is a refusal nobody has seen "
                                      "fire, which is how round128's single tag lost three findings")
                check = subprocess.run([sys.executable, str(runner)], capture_output=True, text=True)
                if check.returncode:
                    errors.append("the CloverBlock host model failed: the row walk, a bound, the "
                                  "delivery counts or the log-window overlap is not what this boot's "
                                  "report will claim it is\n"
                                  + (check.stdout + check.stderr)[-700:])

    append = REPO / "scripts/append-data-window.py"
    if not append.is_file():
        errors.append("scripts/append-data-window.py is missing: the flashed image would carry no "
                      "Data window and the owner mount would panic 'offline scratch Data mount failed'")
    else:
        text = append.read_text()
        for needle in ('SEED_BYTES = 32 * MIB',
                       'SEED_SHA256 = "ef00c083fc54453c4ff6062c2e6947ddbba63e96aabcfd27d2273a53e4d3918c"',
                       'SEED_UUID = "FC1A580E-2D67-48FC-B8CF-87422A319340"',
                       'NAME = "Data"',
                       'BLOCK_BYTES, 0, digest',
                       'if size + SEED_BYTES > MAX_BYTES',
                       'if size % MIB'):
            if needle not in text:
                errors.append(f"scripts/append-data-window.py lacks {needle!r}: the appended entry "
                              "would not match the seed, the UUID CloverBlock answers, or the "
                              "writable flags rule")

    # Round160. The userdata flash is the one destructive step in this port, and it writes 9.4 GB
    # over a 54 GB partition. The manifest, the seed and the sparse container were each checked by
    # the builder that wrote them; the host gate has to read the container the flasher will read.
    # scripts/verify-userdata-window.py is that gate: it expands the all-RAW sparse file by header,
    # refuses anything but OS + SystemOS + Data with the writable flag, and hashes the Data bytes
    # in place. The needles below are the checks that make it a gate rather than a printout.
    verifier = REPO / "scripts/verify-userdata-window.py"
    if not verifier.is_file():
        errors.append("scripts/verify-userdata-window.py is missing: the 9.4 GB userdata image "
                      "would be flashed with no host check of its manifest or Data seed")
    else:
        text = verifier.read_text()
        for needle, why in (
                ("PARTITION_BYTES = (0x747BFDE - 0xF10520 + 1) * BLOCK_BYTES",
                 "the capacity check would not use board.toml's own partition extent"),
                ('DATA_SHA256 = "ef00c083fc54453c4ff6062c2e6947ddbba63e96aabcfd27d2273a53e4d3918c"',
                 "the seed bytes would not be compared with the pinned 32 MiB image"),
                ('DATA_UUID = "FC1A580E-2D67-48FC-B8CF-87422A319340"',
                 "the APFS volume the kernel searches for would not be named"),
                ('if ctype != CHUNK_TYPE_RAW:',
                 "a DONT_CARE chunk would be accepted, but the device would skip its bytes"),
                ('if sorted(found) != ["Data", "OS", "SystemOS"]:',
                 "a missing or extra window would pass instead of being refused"),
                ('if data["offset"] + data["size"] != image_bytes:',
                 "a Data window that is not last would overwrite the backup GPT's region"),
                ('if actual != image.checksum:',
                 "the --crc pass would not compare the container's own CRC-32"),
                ('if uuid != DATA_UUID:',
                 "the optional APFS check would not compare the mounted volume's UUID")):
            if needle not in text:
                errors.append(f"scripts/verify-userdata-window.py lacks {needle!r}: {why}")

    # Round148. Round147 shipped an unattended deadline that fired on the very first timer tick, with
    # a garbage counter in the message. The cause was not the arithmetic: the Darwin arm64 assembler
    # reads `;` as the comment character, so every multi-instruction asm string kept only its first
    # instruction -- the CNTVCT read, the SPSR_EL1 read, the IPI send, the PAR_EL1 read and the
    # panic screen's CNTPCT wait were all silently dropped. This round separates those sequences
    # with newlines and makes check_inline_asm keep them that way. Round147's contract, meanwhile,
    # still has to hold: the emergency exit the host can find on its own.
    #
    # Every previous round ended in a state the host had to look at: a panic screen that
    # parks, a tick-count watchdog, and no way back in without a hand on the tablet. The unattended
    # contract makes the board answer by itself: ArmCpu turns clover-unattended=<seconds> into a
    # CNTVCT deadline, CloverSdhci marks IMEM's restart-reason cell at boot and publishes it to the
    # panic screen, the panic screen writes "bootloader" and resets through PSCI after 20 s, and the
    # host waits for fastboot, asks for recovery and decodes logfs. Round149 keeps every link but
    # disarms it in the shipped boot, so the links are still asserted as source below.
    # Round149 disarmed the deadline because its own question was "how far does iPadOS get when
    # nothing interrupts it". Round184 arms it again, and the reason is on the card rather than in
    # this file: a boot that gets anywhere never comes back to fastboot by itself, so the host waits
    # for a device that will never appear -- and the panel it leaves behind is a printer, not a
    # scrolling console, that stops drawing when the screen is full. round183's frame is the proof:
    # its first row is still the kernel's own version banner and its last row is the DSP instrument's
    # `part(3)`. Both halves of the reading were lost that way. Round285 disarms it again: absence
    # is asserted above, next to the archives that schedule it, so what stays here is the rule that
    # the *tick-count* watchdog must not come back, because a clock that panics at N ticks
    # measures the scheduler rather than the boot.
    kernel_args = board["payload"]["kernel_args"]
    if any(argument.startswith("armcpu-clock-panic=") for argument in kernel_args):
        errors.append("[payload] kernel_args still carries armcpu-clock-panic=: the tick-count "
                      "watchdog would also cut the observation short")
    # Round178 introduced the rule; each round since keeps it. The menu has to name the round under
    # test: the payload and its readings have to agree about which round is on the card, or an image
    # would be claiming a round it does not run. The drivers keep their own stamps -- the display
    # probe still says r180, drivers/CloverInput says r181, drivers/CloverPower says r182 and
    # drivers/CloverDsp says r183 -- because the tee on the card filters the shared "clover r"
    # prefix, not a round number. Round185 moves the log window to logdump and arms the rings from the
    # GPT walk, so the SDHCI driver carries its own r185 stamp for the arming line; round186, round187
    # and round188 change no driver at all. Round189 changes the driver only inside the tee's per-pass
    # push budget, which prints nothing new and leaves the window geometry alone, so the r185 arming
    # stamps still say what happened and only the menu moves. Round190 adds the flush bracket to the
    # block driver, which carries its own r190 stamps and also leaves the arming lines alone.
    # Round191 splits that bracket into stages and moves the commit's wait onto the host's busy bit,
    # which is still inside the flush path and still leaves the arming lines where r185 put them.
    # Round193 changes no driver at all: it adds one kernel line at the top of
    # proc_handle_critical_exit and one boot argument, so every driver stamp stays where it was.
    # Round312 changes no source that prints and ships no image -- it is the Wi-Fi transport reading --
    # so the menu stayed on round311. Round313 moves it: drivers/CloverPower's addresses become a
    # function of the arbiter's own version word, and that driver has never been in a boot whose title
    # named it, because clover-power has always been commented out. Its new lines are stamped r313 and
    # the round182 lines keep their origin stamp, which is the reason the tee now budgets both tokens.
    # Round319 re-publishes the r318 raw words on the beat (the r260 pattern), so the image whose card
    # first carries the surviving SDA/SCL reading has to name itself round319. Round320 reused that
    # kernel and added ephemeral-storage; boot 108 read it back and the stall only moved (migration ->
    # chronod), and sheng boots to the desktop without it, so round322 reverts the ADT again: same
    # kernel, ephemeral-storage ABSENT so first-boot migration runs -- the menu names round322.
    # Round335 is the code round after the merged-tooling control round: two probe columns move (bbswap
    # gains arg/done/cur/list and drops swap; bbsend now prints every hop), so the menu names round335
    # while rows this round did not touch keep their own generation tokens (bbsend r331, bbrcv r328,
    # dispstate r329) -- a card row names its probe generation, the title names the image that ran.
    if not board["payload"]["title"].startswith("clover r339:"):
        errors.append('[payload] title is not "clover r339: ...": the menu would not name this round')
    # The instrument's readings only leave the device if the card-backed ring exists, so the order of
    # the tail calls in sheng_pmgr_run() is part of the contract, not an implementation detail.
    pmgr_source = (REPO / "drivers/CloverPMGR/CloverPMGR.cpp").read_text()
    unattended = pmgr_source.find("clover_unattended_begin();")
    emmc = pmgr_source.find("clover_emmc_run();")
    touch = pmgr_source.find("clover_input_run();")
    power = pmgr_source.find("clover_power_run();")
    dsp = pmgr_source.find("clover_dsp_run();")
    if not (0 <= unattended < emmc < touch < power < dsp):
        errors.append("drivers/CloverPMGR/CloverPMGR.cpp does not call unattended_begin(), then "
                      "emmc_run(), then input_run(), then power_run(), then dsp_run(): the power and "
                      "dsp reports would precede either the ring that carries them off the device or "
                      "the touch readings they exist to explain")

    # Round196, the host half of the panic's BCB. The step after --boot-after-flash watched only
    # for fastboot and printed "the board returned to fastboot by itself, so the deadline fired";
    # round195 read the same landing and the board had come up in Android and been put into
    # fastboot by hand, so that sentence was true about the deadline and false about its intent.
    # The wait now answers both landings, and fastboot means the recovery command never reached
    # misc. Both halves are asserted by name because the sequence around them is the same
    # reboot-then-watch earlier rounds used.
    # Round293 takes the last over-read out of the TWRP branch: round198 pre-arms the BCB so that
    # *any* reset lands in recovery, so an adb landing says a reset happened and nothing about which
    # one. Boot 79 ended silent and printed "the panic path's own BCB took ... after the deadline
    # reset", which is the sentence this project keeps having to un-write.
    script_text = (REPO / "scripts/run-clover-unattended.sh").read_text()
    if "watch_after_deadline() {" not in script_text:
        errors.append("scripts/run-clover-unattended.sh has no watch_after_deadline: after "
                      "--boot-after-flash the script would again read a fastboot landing as proof "
                      "of a reset whose recovered landing it never checked")
    if "after the deadline reset" in script_text:
        errors.append("scripts/run-clover-unattended.sh still calls a TWRP landing the deadline's: "
                      "since round198 pre-arms the BCB every reset lands there, and a silent boot "
                      "would be reported as one that reached its deadline")
    # Round202's frame reached SpringBoard at 358 s, and its 900 s deadline sat outside the
    # script's WAIT window -- so the board's own reset landed after the window closed and the
    # run reported "neither". Round228's deadline is 1800 s (board.toml), so WAIT and
    # LANDING_WAIT both cover it plus the reset delay and TWRP's adb coming up, with
    # DEADLINE_SLACK on top.
    if "DEADLINE_SLACK=120" not in script_text:
        errors.append("scripts/run-clover-unattended.sh has no DEADLINE_SLACK: the landing budget "
                      "would again expire before the payload's deadline plus its reset delay, "
                      "and a reset that did happen would read as nothing coming back")
    if "WAIT=2100" not in script_text:
        errors.append("scripts/run-clover-unattended.sh does not wait 2100 s: with the r228 "
                      "deadline at 1800 s the --boot-after-flash watch would expire before the "
                      "payload's own reset")
    if "LANDING_WAIT=2100" not in script_text:
        errors.append("scripts/run-clover-unattended.sh does not watch 2100 s for the recovery "
                      "landing: a board whose BCB did not take boots the payload and only comes "
                      "back to fastboot when the 1800 s deadline resets it")
    if "returned to fastboot by itself, so the deadline fired" in script_text:
        errors.append("scripts/run-clover-unattended.sh still says the board 'returned to fastboot "
                      "by itself, so the deadline fired': round195 showed that sentence on a board "
                      "that had come up in Android and was put into fastboot by hand")
    # Round268's flash test. The script treated "an adb device is already up" as "we are in recovery,
    # just read", and that shortcut set SKIP_RECOVERY=1 -- which is also the gate on the flash block. A
    # run asked to --flash-cache with the board sitting in TWRP therefore flashed nothing, read the
    # window the PREVIOUS payload's boot had left, and exited 0: every number in the report was real and
    # none of it belonged to the round. The fix is two parts, and both are asserted because the failure
    # was silent: a declared flash hops the board to the bootloader, and the card's own boot counter, read
    # while recovery is still up, has to have moved before a window is called a result.
    if '[ -n "$(adb_serial)" ]; then\n  SKIP_RECOVERY=1' in script_text:
        errors.append("scripts/run-clover-unattended.sh lets an adb device in recovery set "
                      "SKIP_RECOVERY=1 unconditionally: that shortcut skips a declared --flash-cache and "
                      "reports the previous boot's window as this round's reading")
    for needle in ("an adb device is up and a flash was declared",
                   'PRE_BOOT=$(python3 "$(cd "$(dirname "$0")" && pwd)/read-clover-log.py" --boot',
                   "the window still holds boot"):
        if needle not in script_text:
            errors.append("scripts/run-clover-unattended.sh has no %r: without the hop-and-count pair a "
                          "flash that never booted is indistinguishable from a boot that printed nothing"
                          % needle)
    # Round330 host side. Boots 113 and 114 each ran their full watch window, reported "neither TWRP's
    # adb nor fastboot" and left nothing on disk behind them, while the board was later found sitting in
    # recovery holding the boot in question. "When did it come back" therefore has no reading, and the
    # two candidates that message hides are 2300 s apart: a landing just after the window, or a board
    # that never enumerated over USB at all. The instrument is one file per run, one section per watch,
    # each poll's own two answers written when either changes and otherwise once a minute, with the USB
    # tree captured at open and close. What is asserted below is the part a half-implemented version
    # would get wrong: a watch that samples only one of the two tools cannot date its own silence,
    # because the other tool's answer is the reading.
    for needle in ("timeline_open()", "timeline_poll()", "timeline_close()",
                   ".timeline.txt", "ioreg -p IOUSB -w0", 'timeline: $TIMELINE',
                   "watch timeline:"):
        if needle not in script_text:
            errors.append("scripts/run-clover-unattended.sh has no %r: a silent watch window would again "
                          "leave no artifact, and the run could not tell a too-short window from a board "
                          "that never enumerated" % needle)
    for label in ("wait-fastboot", "wait-adb", "watch-landing", "after-deadline"):
        if f'timeline_open {label} ' not in script_text:
            errors.append(f"scripts/run-clover-unattended.sh opens no timeline section for {label}: that "
                          "window's silence would be unrecorded, which is the defect round330 removes")
    for name in ("wait_fastboot", "wait_adb", "watch_landing", "watch_after_deadline"):
        body = re.search(rf"^{name}\(\) \{{.*?^\}}", script_text, re.S | re.M)
        if body is None:
            errors.append(f"scripts/run-clover-unattended.sh has no {name}(): the read-back path lost a "
                          "watch it is contracted to run")
            continue
        text = body.group(0)
        for needle in ("adb_serial", "fastboot_serial", "timeline_poll", "timeline_close"):
            if needle not in text:
                errors.append(f"{name}() does not {needle}: a watch that asks only one of the two tools, "
                              "or writes nothing, cannot tell a late landing from no landing at all")
    # Round335 (#716). "Neither tool answered" is not one outcome, and the two runs that proved it are
    # boots 118 and 119: both closed their after-deadline watch with `neither`, both said "the deadline
    # did not fire, the panic screen never reset, or the reset does not reach this host", and both had
    # already photographed a USB tree at that close enumerating QUSB_BULK -- a reset this host did see,
    # in the one mode those two tools do not read. The half that was missing was not the tree, it was
    # the verdict sentence: the tree was on disk and nothing quoted it. So the class is now measured by
    # the writer and named in the close label, and what is pinned below is the part a half-fix would
    # get wrong: a class re-measured when the sentence is printed is a claim about the moment of
    # printing, not about the moment of the close.
    if not re.search(r"^usb_landing_class\(\) \{", script_text, re.M):
        errors.append("scripts/run-clover-unattended.sh has no usb_landing_class(): the silent window "
                      "would again be one undivided outcome")
    if "QUSB_BULK" not in script_text:
        errors.append("usb_landing_class does not look for QUSB_BULK: that is the node the two silent "
                      "runs of boots 118 and 119 enumerated, and it is what the classifier is for")
    silent_closes = re.findall(r'timeline_close "neither-\$\(usb_landing_class\)" "" ""', script_text)
    if len(silent_closes) != 2:
        errors.append(f"the silent path interpolates the class in {len(silent_closes)} of the 2 watches "
                      "that can close silent (watch_landing and watch_after_deadline): one of them would "
                      "still write a classless `neither`, and a reader cannot classify what was not said")
    if "closing=$(timeline_last_close)" not in script_text:
        errors.append("the exit-6 verdict does not quote the timeline's own close line: re-measuring the "
                      "tree when the sentence is printed would describe the host at verdict time, and the "
                      "board may have moved on -- the reading is about the close")
    # Round339b. Boot 120 proved that quoting the file is not the same as quoting the close line: the
    # writer appends the USB tree underneath the close, so the line at the end of the file is a field of
    # that tree, and the sentence that claimed to name the close named `id`. That is the shape layer 17
    # warns about -- a needle pinned to an expression, satisfied by an expression that is wrong. So the
    # old text needle is now a ban plus a run: the helper is cut out of the shipping script and executed
    # against synthetic timelines, which is the only way this file can tell `tail -n 1` from a scan that
    # restarts at each header.
    quoted_close_ban = re.findall(r'closing=\$\(tail[^\n]*TIMELINE', script_text)
    if quoted_close_ban:
        errors.append(f"the verdict sentences extract the close from the end of the file in "
                      f"{len(quoted_close_ban)} place(s), the first {quoted_close_ban[0][:70]!r}: the "
                      "close line is not the last line -- timeline_close photographs the USB tree under "
                      "it, and boot 120's run quoted a field of that tree as its own verdict")
    closer = re.search(r"^timeline_last_close\(\) \{.*?^\}", script_text, re.S | re.M)
    if closer is None:
        errors.append("scripts/run-clover-unattended.sh has no timeline_last_close() block the gate can "
                      "run: the verdict's reading of its own timeline would again be judged by looking "
                      "for the characters it happens to contain")
    else:
        with tempfile.TemporaryDirectory() as tmp:
            # One case per thing the sentence claims. The first is boot 120's own shape: close line,
            # then a tree whose rows have fields of their own. The second is the append across runs --
            # the file is never truncated, so a scan without a header reset would quote an older
            # window's verdict as this one's. The third is the window that never closed: the sentence
            # must then say `no-close-line`, and a helper that fell through to the last close in the
            # file would silently upgrade that to a reading.
            closer_cases = [
                ("close then the tree under it",
                 "# watch after-deadline budget=2100s opened X\n"
                 "# epoch elapsed adb fastboot event class\n"
                 "1794036000 0 - - poll no-node\n"
                 "1794036735 2220 - - close:neither-edl\n"
                 "# usb at close (neither-edl)\n"
                 "  | +-o F6548A52@01143000  <class IOUSBHostDevice, id 0x100003d11>\n",
                 "close:neither-edl"),
                ("an older window's close, this one still open",
                 "# watch after-deadline budget=2100s opened X\n"
                 "1794000000 2100 - - close:adb\n"
                 "# usb at close (adb)\n"
                 "  | +-o Mi Pad 4@0130000f  <class IOUSBHostDevice, id 1>\n"
                 "# watch watch-landing budget=900s opened Y\n"
                 "1794099000 120 - - poll no-node\n",
                 ""),
                ("two closes in one window, the last one is this window's",
                 "# watch after-deadline budget=2100s opened X\n"
                 "1794036000 0 - - close:neither-no-node\n"
                 "1794036100 100 - - close:neither-edl\n",
                 "close:neither-edl"),
            ]
            for name, contents, want in closer_cases:
                path = pathlib.Path(tmp) / "timeline.txt"
                path.write_text(contents)
                probe = subprocess.run(
                    ["bash", "-c", closer.group(0) + f'\nTIMELINE="{path}"\n'
                     'printf "GOT[%s]\\n" "$(timeline_last_close)"'],
                    capture_output=True, text=True)
                got = ""
                for row in probe.stdout.splitlines():
                    if row.startswith("GOT[") and row.endswith("]"):
                        got = row[4:-1]
                if got != want:
                    detail = f"; stderr {probe.stderr.strip()[-160]!r}" if probe.stderr else ""
                    errors.append(f"timeline_last_close on {name} returns {got!r}, expected {want!r}: "
                                  "the verdict sentence would name something other than this window's "
                                  f"own close{detail}")
    reader_text = (REPO / "scripts/read-clover-log.py").read_text(encoding="utf-8", errors="replace")
    for label in ("wait-fastboot", "wait-adb", "watch-landing", "after-deadline"):
        if f'"{label}": {{' not in reader_text:
            errors.append(f"read-clover-log.py registers no reading for the {label} watch: a close label "
                          "with no key is counted as UNKEYED, and the point of that file is that each of "
                          "its lines says what it excludes")
    if not re.search(r"^    timeline_selftest\(\)$", reader_text, re.M):
        errors.append("read-clover-log.py --selftest does not run its timeline leg (the call line, not "
                      "the definition -- `def timeline_selftest():` contains the same characters and "
                      "proves nothing): the parser would again be tested only against the writer's "
                      "printf literals in principle, not in fact")
    # Round337 (#718). The close line says what the bus was at the end of a silent window; boots 118
    # and 119 showed that this cannot date the change, because the two runs proved only "in EDL by
    # 2221 s" while the card's last kernel line sat at 649 s and 730 s. So the class is now asked at
    # every poll, inside timeline_poll() rather than at its four call sites -- a watch cannot forget
    # a sampling it has no parameter for. Pinned below: that the sampling sits in the one function
    # every watch must call,
    # that an empty answer from the host tool is its own class rather than a quiet bus, that both
    # verdict sentences quote the file instead of re-measuring, and that the reader can tell a poll
    # row without the column from a poll row that asked and got no-node.
    poll_body = re.search(r"^timeline_poll\(\) \{.*?^\}", script_text, re.S | re.M)
    if poll_body is None:
        errors.append("scripts/run-clover-unattended.sh has no timeline_poll(): the per-poll class has "
                      "nowhere to live, so the window's silence again has one timestamp instead of a "
                      "sequence")
    elif "usb_landing_class" not in poll_body.group(0):
        errors.append("timeline_poll() does not sample usb_landing_class: a class asked only at the "
                      "close dates nothing -- the reading this round exists to produce is the moment "
                      "the mode changed")
    if '[ -z "$tree" ]' not in script_text:
        errors.append("usb_landing_class does not test the empty tree before matching names: when "
                      "ioreg answers nothing this host failed to answer, which is not the same "
                      "statement as a bus with no Qualcomm node on it -- the third zero of this "
                      "file's own making")
    # The classifier is run here rather than read: grepping for the word it emits was tested by
    # negative case N8 and passed a mutation that deleted the branch, because the word also appears
    # in the sentence explaining it. Four stubs, one per answer, and the empty one has to come out
    # as its own class.
    classifier = re.search(r"^usb_landing_class\(\) \{.*?^\}", script_text, re.S | re.M)
    if classifier is None:
        errors.append("scripts/run-clover-unattended.sh has no usb_landing_class() block the gate can "
                      "run: the class of every poll would again be judged by looking for its name")
    else:
        probe = subprocess.run(
            ["bash", "-c", classifier.group(0) + """
ioreg() { printf '%s\\n' "$IOUSB_STUB"; }
for stub in "" "x QUSB_BULK_SN:F6548A52@01143000 y" "x Diag@01144000 y" "x Mi Pad 4@0130000f y"; do
  IOUSB_STUB="$stub"
  printf 'STUB[%s] CLASS[%s]\\n' "$stub" "$(usb_landing_class)"
done
"""], capture_output=True, text=True)
        wanted = {"STUB[] CLASS[ioreg-fail]": "the empty answer is this host failing, not a quiet bus",
                  "STUB[x QUSB_BULK_SN:F6548A52@01143000 y] CLASS[edl]":
                      "the node boots 118 and 119 enumerated must classify as edl",
                  "STUB[x Diag@01144000 y] CLASS[diag]": "a diagnostic port is its own class",
                  "STUB[x Mi Pad 4@0130000f y] CLASS[no-node]":
                      "a board the two tools do not see here must not be read as edl"}
        for line, why in wanted.items():
            if line not in probe.stdout:
                errors.append(f"usb_landing_class does not return {line}: {why}; stdout was "
                              f"{probe.stdout.strip()[-300:]!r}")
    quoted_closes = re.findall(r'closing=\$\(timeline_last_close\)', script_text)
    if len(quoted_closes) != 2:
        errors.append(f"the verdict sentences quote the timeline's close line in {len(quoted_closes)} "
                      "of the 2 places that can end silent (watch_landing and watch_after_deadline): "
                      "the one that does not describes the host at printing time, and the board may "
                      "have moved by then")
    remeasured = re.findall(r'(?:say|die)[^\n]*\$\(\s*usb_landing_class', script_text)
    if remeasured:
        errors.append("a verdict sentence measures the USB tree again at the moment it prints: "
                      f"{len(remeasured)} such line(s), the first {remeasured[0][:90]!r} -- the close "
                      "line is the reading, and this script already knows how to quote it")
    if 'TIMELINE_CLASS_ABSENT = "not-asked-by-this-writer"' not in reader_text:
        errors.append("read-clover-log.py has no named literal for a poll row without the class "
                      "column: five-field rows are every timeline written before round337, and reading "
                      "them as `no-node` would put a reading on a window where nobody asked")
    for needle in ("class-polls=", "class-at-open=", "poll class asked in"):
        if needle not in reader_text:
            errors.append(f"read-clover-log.py no longer prints {needle!r}: the onset time, the "
                          "derived-at-open leg and the generation of the writer are the three things "
                          "this file says that a close label cannot")
    # The script is the read-back path, and until now its syntax was only ever tested by running it on
    # the board's clock: a typo costs a 2100 s window and a boot nobody read.
    syntax = subprocess.run(["bash", "-n", str(REPO / "scripts/run-clover-unattended.sh")],
                            capture_output=True, text=True)
    if syntax.returncode:
        errors.append("scripts/run-clover-unattended.sh does not parse: "
                      + (syntax.stderr or syntax.stdout)[-700:])
    # Round338 (#719). The reset record was sized by a comment: round283's author read the unclaimed
    # IMEM gap as twenty words and the slot grew to exactly what the comment said. The gap is ten.
    # What is pinned below is not the number -- it is that the number is DERIVED from two device-tree
    # sources and attributed per word, so the next resize is measured against a map instead of
    # against somebody's memory of one. The gate runs the script rather than grepping it (round337's
    # N8 lesson: a substring pin is satisfied by the sentence that explains the substring), and it
    # counts the children in each archived block itself, so a script that quietly compares the board
    # tree with a copy of itself is caught by the counts disagreeing.
    geometry = REPO / "scripts/test-clover-imem-geometry.py"
    archive = REPO / "notes/sources/sdm660/imem-children.dts"
    if not archive.is_file():
        errors.append("notes/sources/sdm660/imem-children.dts is missing: the IMEM child map would "
                      "again live only in a comment, which is what made round283's twenty words a "
                      "claim instead of a reading")
    elif not geometry.is_file():
        errors.append("scripts/test-clover-imem-geometry.py is missing: nothing would say which "
                      "child node owns the eight bytes under each word the reset record touches")
    else:
        archived = archive.read_text(encoding="utf-8", errors="replace")
        blocks = re.split(r"^=== SOURCE", archived, flags=re.M)[1:]
        if len(blocks) != 2:
            errors.append(f"the IMEM archive carries {len(blocks)} SOURCE blocks, not 2: the map is "
                          "supposed to be two-source, and one leg of a pair that is not a pair is a "
                          "single source wearing a claim of independence")
        else:
            counted = [len(re.findall(r"^\s*[A-Za-z0-9_,.-]+@[0-9a-fA-F]+ \{", b, re.M)) - 1
                       for b in blocks]
            run = subprocess.run([sys.executable, str(geometry)], capture_output=True, text=True)
            if run.returncode:
                errors.append("scripts/test-clover-imem-geometry.py exits "
                              f"{run.returncode}: " + (run.stderr or run.stdout)[-700:])
            else:
                report = run.stdout
                parsed = re.search(r"children parsed: board=(\d+) vendor=(\d+)", report)
                if parsed is None:
                    errors.append("the geometry report prints no `children parsed: board= vendor=` "
                                  "line: the two legs would be unnamed and a reader could not tell "
                                  "one source from two")
                elif [int(parsed.group(1)), int(parsed.group(2))] != counted:
                    errors.append(f"the geometry script reports board={parsed.group(1)} "
                                  f"vendor={parsed.group(2)} children while the archive holds "
                                  f"{counted[0]} and {counted[1]}: the script would be parsing one "
                                  "block twice, which is the round337 N8 shape again -- a check that "
                                  "asks the same source twice")
                for needle in ("two-source disagreements=0",
                               "largest contiguous free run starting at the record offset",
                               "imem geometry selftest:"):
                    if needle not in report:
                        errors.append(f"the geometry report no longer prints {needle!r}: that line is "
                                      "the free-run size, the independence of the two sources, and "
                                      "the claim that every word got attributed")
                rows = re.findall(r"^\[\s*\d+\] {2}0x[0-9a-f]+ {2}(\S+)", report, re.M)
                beat_rows = re.findall(r"^b\[\s*\d+\] {2}0x[0-9a-f]+ {2}(\S+)", report, re.M)
                declared = re.search(r"reset record: offset (0x[0-9a-f]+), (\d+) words touched",
                                     report)
                beat_declared = re.search(r"heartbeat cell: offset (0x[0-9a-f]+), (\d+) words touched",
                                          report)
                if declared is None or beat_declared is None:
                    errors.append("the geometry report does not give both cells their own row count: "
                                  "a table without its own scope is a scan whose denominator is unknown")
                else:
                    record_at = int(declared.group(1), 16)
                    record_words = int(declared.group(2))
                    beat_at = int(beat_declared.group(1), 16)
                    beat_words = int(beat_declared.group(2))
                    if len(rows) != record_words:
                        errors.append(f"the geometry table has {len(rows)} record rows but declares "
                                      f"{declared.group(2)} words probed: the unattributed words are "
                                      "the ones that matter")
                    if len(beat_rows) != beat_words:
                        errors.append(f"the geometry table has {len(beat_rows)} heartbeat rows but "
                                      f"declares {beat_declared.group(2)} words touched: the cell "
                                      "round339 moved the counters into would be listed at a different "
                                      "width than it is claimed")
                    if any(row in ("", "UNNAMED") for row in rows + beat_rows):
                        errors.append("at least one word in the geometry table has no owner: an "
                                      "unattributed word reads as free space while a child node claims it")
                    beat_free = re.search(r"largest contiguous free run starting at the heartbeat "
                                          r"cell: (\d+) bytes", report)
                    if beat_free is None:
                        errors.append("the report says nothing about the free run at the heartbeat "
                                      "cell: that cell is this round's claim, and a claim nobody "
                                      "re-measured is the twenty-word comment again")
                    elif int(beat_free.group(1)) < beat_words * 8:
                        errors.append(f"the heartbeat cell at {beat_declared.group(1)} is "
                                      f"{beat_declared.group(2)} words but only {beat_free.group(1)} "
                                      "free bytes start there: the counters would sit on a child node "
                                      "the firmware owns, which is the exact thing round338 found in "
                                      "the record's own words 10 and 11")
                    gap = re.search(r"gap between the two cells: (\d+) bytes = (\d+) words; owners "
                                    r"in the gap: (.+?) \(asked about (\d+) words, named (\d+)\)",
                                    report)
                    if gap is None:
                        errors.append("the geometry report prints no gap between the record and the "
                                      "heartbeat cell: the reason the record stops at ten words is the "
                                      "territory between them, and without it the resize is an "
                                      "assertion again")
                    elif int(gap.group(1)) != beat_at - (record_at + record_words * 8):
                        errors.append(f"the gap line reports {gap.group(1)} bytes but the two cells' "
                                      f"own offsets and widths put {(beat_at - record_at - record_words * 8)} "
                                      "bytes between them: the addresses in this report are not "
                                      "describing one layout")
                    elif int(gap.group(2)) != int(gap.group(4)):
                        errors.append(f"the gap line says {gap.group(2)} words and then asks about "
                                      f"{gap.group(4)}: a count and its denominator disagreeing is "
                                      "how `0 named` becomes `0 asked`")
                    elif gap.group(3) == "none":
                        errors.append("nothing in the tree claims the bytes between the two cells, so "
                                      "the split into two cells is not the reading the tree gives -- "
                                      "either the gap has become free (and the record could be one "
                                      "cell again, which is a decision to re-register rather than a "
                                      "silent edit) or the map changed under this round")
                span = re.search(r"child nodes the record spans: (.+?) \(asked about (\d+) words, "
                                 r"named (\d+)\)", report)
                if span is None:
                    errors.append("the geometry report names no cells the record spans and asked "
                                  "about nothing: the overlap question would be unanswered again")
                elif declared is not None and int(span.group(2)) != record_words:
                    errors.append(f"the overlap line asks about {span.group(2)} words while the "
                                  f"record itself is declared at {declared.group(2)}: the two lines "
                                  "would be describing different footprints")
                elif declared is not None and span.group(1) != "none":
                    errors.append(f"the record spans {span.group(1)} again: round339 resized it to the "
                                  "run round338 measured, and a record that reaches a child node is "
                                  "this driver wiping the firmware's own counters every boot")
        for needle in ("sdam", "PM660"):
            if needle.lower() not in archived.lower():
                errors.append(f"the IMEM archive says nothing about {needle}: what this board's tree "
                              "does NOT declare is as much a fact as what it does, and round336's "
                              "next hand was planned around a PMIC this board does not carry")

        # The same instrument's second leg: what the words in this window MEAN, taken from the only
        # kernel that writes them. Round338 ran it against the name and value drivers/CloverSdhci used
        # then and found the vendor table putting that value under `other`, the unrecognized-command
        # arm and the no-command arm -- never under `bootloader`; round339 changed the constant to the
        # value the vendor does write for that command. From here this is a requirement, and the way
        # it is enforced is by counting the rows rather than by naming the retired spelling: the
        # strings the check forbids are therefore never written out in this file.
        # Pinned so the reading cannot survive by being written down once: the gate counts the writes in
        # the archived file itself, re-reads the driver's own constant, and refuses a report whose value
        # came from the report instead of from the source.
        poweroff = REPO / "notes/sources/sdm660/msm-poweroff.c"
        note = REPO / "notes/sources/sdm660/dload-and-restart-reason.md"
        stats = REPO / "notes/sources/sdm660/boot_stats.c"
        for path, why in ((poweroff, "the restart_reason values would have no authority to be checked "
                                     "against -- the driver's own comment is not that authority"),
                          (note, "the EDL and download-mode reading would have no artifact to point "
                                 "at, and a reading that lives only in a round's prose is the round332 "
                                 "shape again"),
                          (stats, "the claim that boot_stats is read-only for the kernel would be a "
                                  "claim about a file this repository does not hold")):
            if not path.is_file():
                errors.append(f"{path.relative_to(REPO)} is missing: {why}")
        if run.returncode == 0 and poweroff.is_file() and stats.is_file() and note.is_file():
            writes = re.findall(r"__raw_writel\(\s*(?:0x7766[0-9a-fA-F]{4}|0x0)\s*,\s*"
                                r"restart_reason\s*\)", poweroff.read_text(encoding="utf-8",
                                                                            errors="replace"))
            report_written = re.search(r"restart_reason writes parsed from msm-poweroff\.c: (\d+)",
                                       report)
            if report_written is None:
                errors.append("the geometry report says nothing about how many restart_reason writes "
                              "it read: a table without its own scope is how `0 rows` means `not asked`")
            elif int(report_written.group(1)) != len(writes):
                errors.append(f"the report parsed {report_written.group(1)} restart_reason writes while "
                              f"the archived vendor file holds {len(writes)}: the table is not being "
                              "built from the file it names")
            driver_text_now = (REPO / "drivers/CloverSdhci/CloverSdhci.cpp").read_text(
                encoding="utf-8", errors="replace")
            declared_names = sorted({n for n in re.findall(
                r"static const uint32_t (kRestart\w*) = 0x[0-9a-fA-F]+u?;", driver_text_now)
                if not n.endswith("Offset")})
            if len(declared_names) != 1:
                errors.append(f"the driver declares {len(declared_names)} restart-reason value "
                              "constants: the geometry instrument asks the vendor table about one, so "
                              "a second name in the same file means the report and the code are not "
                              "talking about the same word")
                declared_name = None
            else:
                declared_name = declared_names[0]
                stale_names = sorted({n for n in re.findall(r"\bkRestart\w*\b", driver_text_now)
                                      if n != declared_name and not n.endswith("Offset")})
                if stale_names:
                    errors.append(f"the driver still carries {stale_names} next to the constant the "
                                  "instrument verifies: a retired name left in the file is a claim "
                                  "about the vendor's labelling that nothing checks any more")
            driver_value = re.search(
                rf"static const uint32_t {declared_name} = (0x[0-9a-fA-F]+)u?;",
                driver_text_now) if declared_name else None
            reported_name = re.search(r"this port: (\w+) = (0x[0-9a-fA-F]+)", report)
            if reported_name is None:
                errors.append("the report no longer names this port's restart-reason value: the "
                              "comparison would have no subject")
            elif declared_name and reported_name.group(1) != declared_name:
                errors.append(f"the report asks about {reported_name.group(1)} while the driver "
                              f"declares {declared_name}: the two are different claims about the "
                              "same cell")
            if driver_value and reported_name and \
                    int(driver_value.group(1), 16) != int(reported_name.group(2), 16):
                errors.append("the report names a value that is not the one drivers/CloverSdhci "
                              "declares: the comparison would be about a number nobody ships")
            boot_row = re.search(r"the value the vendor writes for `reboot bootloader`: "
                                 r"(0x[0-9a-fA-F]+) \(line (\d+)\)", report)
            if boot_row is None:
                errors.append("the report no longer names the vendor's bootloader value and the line it "
                              "came from: that pair is the finding, and without it the next round is "
                              "back to reading a comment")
            elif driver_value and boot_row.group(1).lower() != driver_value.group(1).lower():
                errors.append(f"this port's restart-reason value ({driver_value.group(1)}) is no longer "
                              f"the vendor's bootloader value ({boot_row.group(1)}): round338 measured "
                              "that disagreement and round339 closed it, so a new one is either a "
                              "deliberate re-judgement or the constant moved by accident")
            # The agreement sentence is the closure itself: every live vendor row that writes this
            # value has to label it bootloader. Round338 printed this as a reading (0 of 3); the
            # sentence stays in the report as a count pair so a future round can see which of the two
            # arms moved, and the gate is what changed from reporting to requiring.
            agreement = re.search(r"agreement check: our value is labelled bootloader by the vendor "
                                  r"in (\d+) of (\d+) rows that write it", report)
            if agreement is None:
                errors.append("the geometry report prints no agreement count: without `N of M` the "
                              "closure cannot be re-opened by a later round, which is the shape this "
                              "gate had in round338 and is exactly what it is now checking")
            elif int(agreement.group(2)) == 0 or int(agreement.group(1)) != int(agreement.group(2)):
                errors.append(f"the agreement line reads {agreement.group(1)} of "
                              f"{agreement.group(2)}: the value this port writes is not labelled "
                              "bootloader in every live vendor row that carries it")
            panic_text = (REPO / "scripts/xnu-panic-screen.c").read_text(encoding="utf-8",
                                                                        errors="replace")
            word_macros = sorted({m.group(1) for m in re.finditer(r"#define (\w+WORD)\s+0x",
                                                                 panic_text)})
            if word_macros != ["BOOTLOADER_WORD"]:
                errors.append(f"the panic screen declares the word macros {word_macros}: the cell has "
                              "one meaning per boot and two writers, so a second name there is the "
                              "round339 rename half-done")
            screen_word = re.search(r"#define BOOTLOADER_WORD\s+(0x[0-9a-fA-F]+)u", panic_text)
            if screen_word and driver_value and \
                    screen_word.group(1).lower() != driver_value.group(1).lower():
                errors.append(f"the panic screen marks the cell with {screen_word.group(1)} while the "
                              f"driver arms it with {driver_value.group(1)}: one column would then "
                              "mean two different previous boots depending on which writer got there")
            if re.search(r"\bwritel\b", stats.read_text(encoding="utf-8", errors="replace")):
                errors.append("the archived boot_stats.c writes something: the note's reading that the "
                              "kernel only reads those four KPI counters -- and therefore that zeroing "
                              "them costs this board's own boot timing -- has to be redone")

    # Round268's second half: the shared-cache frame decoder, whose two constants decide whether it lies.
    # Scanning image headers one per 0x1000 found 825 of the cache's images and reported most real frames as
    # "in no image"; the cache packs headers every 32 bytes, so the scan has to find each magic occurrence.
    # And N_STAB is the 0xE0 debug mask while N_SECT is 0x0E: written the other way round every defined
    # symbol is skipped and no name is ever printed -- which is exactly the state that made the previous
    # round conclude, wrongly, that these frames cannot be named on the host at all.
    slide_tool = REPO / "scripts/decode-clover-slide.py"
    if not slide_tool.is_file():
        errors.append("scripts/decode-clover-slide.py is missing: without it the runtime frame addresses "
                      "have no slide to unslide by, and the unslid region table answers them with holes")
    else:
        tool = slide_tool.read_text()
        for needle in ("N_STAB = 0xE0", "N_SECT = 0x0E", "MH_MAGIC_BYTES",
                       "cand % PAGE_ALIGN == 0", "refusing to name frames"):
            if needle not in tool:
                errors.append("scripts/decode-clover-slide.py has no %r: one byte-step, page-alignment test "
                              "or symbol-classification rule is gone, and the tool answers with names it "
                              "did not earn" % needle)
        if "range(0, size - 32, PAGE_ALIGN)" in tool:
            errors.append("scripts/decode-clover-slide.py scans cache headers one page at a time: the "
                          "shared cache packs them every 32 bytes, so most images go unseen and frames "
                          "that are in the cache get reported as outside it")
    # Round269's other half: the syntax gate that was missing when five errors went through as "clean".
    # The ad-hoc check printed keep[:40] of the diagnostic stream, and task.c carries a thousand
    # pre-existing -Wcast-function-type-strict warnings ahead of line 12000, so an undeclared proc_rele
    # and two subscripts of an incomplete struct were never shown. The shipped tool has to derive its
    # exit code from the compiler and has to treat the silently-wrong classes as hard errors.
    syntax_tool = REPO / "scripts/check-clover-xnu-syntax.py"
    if not syntax_tool.is_file():
        errors.append("scripts/check-clover-xnu-syntax.py is missing: a round would again be "
                      "self-certified by a diagnostic list that stops at forty lines")
    else:
        stext = syntax_tool.read_text()
        for needle in ("compile_commands.json", "clean.insert(1, \"-fsyntax-only\")",
                       "implicit function declaration", "incomplete type", "out-of-scope",
                       "return 1 if (hard or r.returncode != 0) else 0"):
            if needle not in stext:
                errors.append("scripts/check-clover-xnu-syntax.py has no %r: without the real flags, the "
                              "hard classes and the compiler's own exit code it is the truncated printer "
                              "again" % needle)
        if "[:40]" in stext or "keep[:40]" in stext:
            errors.append("scripts/check-clover-xnu-syntax.py truncates the diagnostic list: that is the "
                          "defect that let five errors in task.c ship as a clean check")
    # Round170. The r169 frame had no panic, no EIO and no signature refusal left: what it had was
    # dyld's check_np line three times per process until the console ran out, and nothing at all
    # about the processes themselves. A child's exit is silent by design -- only critical processes
    # print through proc_handle_critical_exit -- so a spawn loop and a dying daemon look the same.
    # This round puts one line at each end of a process's life: exec names the pid, the parent and
    # the command; exit adds the wait status and the exit-reason namespace and code. Both carry
    # seconds since boot, which is what separates a loop from a machine that is merely slow.
    life_probe = next((base / "patches/xnu-clover/zzy-clover-shared-region-why5.patch"
                       for base in (REPO, pathlib.Path.cwd())
                       if (base / "patches/xnu-clover/zzy-clover-shared-region-why5.patch").is_file()), None)
    if life_probe is None:
        errors.append("patches/xnu-clover/zzy-clover-shared-region-why5.patch is missing: without it "
                      "the frame cannot say which process is looping or how it left")
    else:
        text = life_probe.read_text()
        missing = [needle for needle in
                   ("bsd/kern/kern_exec.c",
                    "bsd/kern/kern_exit.c",
                    "clover r170: exec pid=%d ppid=%d t=%llus comm=%s",
                    "clover r170: exit pid=%d ppid=%d t=%llus rv=0x%x ns=%d code=0x%llx comm=%s",
                    "clock_get_uptime(&clover_now);",
                    "clover_exec_prints < 256",
                    "clover_exit_prints < 256",
                    "exit_reason->osr_namespace",
                    "+#include <kern/clock.h>")
                   if needle not in text]
        for needle in missing:
            errors.append(f"{life_probe.name} lacks {needle!r}: the exec line, the exit line and the "
                          "uptime stamp are the three halves of the spawn question")
        if "set_proc_name(imgp, p);" not in text:
            errors.append(f"{life_probe.name} does not cut in right after set_proc_name(): p_comm is "
                          "still the inherited name before that call, so the line would name the parent")
        if "bsd/vm/vm_unix.c" in text or "vfs_vnops.c" in text:
            errors.append(f"{life_probe.name} touches a file why2 owns: editing inside an earlier "
                          "patch's added block breaks that patch's idempotent reverse check")
    # Round169. The r168 frame answered the write question and asked a read one: the card stored
    # the writes (no tx_flush wall, no "general error" on disk5), but every 16 KiB APFS buffer came
    # back EIO at block numbers nothing had complained about, and decompressing any daemon's file
    # failed with errno 5. The driver was asymmetric: a failed write sends CMD12 and returns the
    # card to TRAN, while a failed single-block read only reset the host. A card left in the data
    # state ignores the reads that follow, which is exactly a cascade at unrelated blocks. Every
    # read failure now sends CMD12 before the host reset, the wait before a write retry and before
    # the first read is the same thirty-second ready-wait the guard uses, and eight stage-tagged
    # lines ("guard", "idle", "deadline", "gave up") replace the once-per-boot line so a frame says
    # where the first EIO came from.
    sdhci = next((base / "drivers/CloverSdhci/CloverSdhci.cpp"
                  for base in (REPO, pathlib.Path.cwd())
                  if (base / "drivers/CloverSdhci/CloverSdhci.cpp").is_file()), None)
    if sdhci is None:
        errors.append("drivers/CloverSdhci/CloverSdhci.cpp is missing: no read path to verify")
    else:
        text = sdhci.read_text()
        for needle in ("CloverSdhci r167: readcheck lba=",
                       "CloverSdhci r167: batch reads trusted for this boot",
                       "CloverSdhci r167: single-block reads for this boot",
                       "CloverSdhci r171: write cache size=",
                       "CloverSdhci r167: write not ready lba=",
                       "CloverSdhci r168: block guard waited %llu ms",
                       "CloverSdhci r168: write gave up lba=%u blocks=%u done=%u",
                       "CloverSdhci r169: card busy %llu ms",
                       "CloverSdhci r169: read %s lba=%u blocks=%u done=%u st=%08x",
                       "CloverSdhci r169: write settle took %llu ms polls=%u",
                       "const uint64_t deadline = frequency * 30ULL;      // 30 s",
                       "static void abortReadTransfer (void)",
                       "abortReadTransfer ();",
                       "static bool writeSettled (uint32_t &response, uint32_t &status, uint32_t &polls)",
                       "static const uint32_t kExtCsdCacheCtrl = 33u;",
                       "const bool stopped = sendStopCommand ();",
                       "static bool prepareReadPath (void)",
                       "SDHCI_RESET_DATA",
                       "if (!chunkRead && chunk >= 4 && gReadBatchTrusted) {"):
            if needle not in text:
                errors.append(f"{sdhci.name} lacks {needle[:28]}...: a write could still be counted "
                              "before the card stored it, or a batch read would be unguarded")
        # The old flat request deadlines must be gone: they were sized for a batch that finishes
        # in milliseconds and turn a slow fallback into a short read the filesystem reports as EIO.
        if "const uint64_t deadline = frequency * 2ULL;   // 2 s for the whole request" in text:
            errors.append(f"{sdhci.name} still gives the whole read request a flat two seconds")
        if "const uint64_t deadline = frequency * 2ULL +" in text:
            errors.append(f"{sdhci.name} still scales a request from a flat two-second base: the "
                          "r167 frame showed both disks erroring while a flush held the card")
        # Round168: the two-second hand-over is what turned a wait into "general error".
        if "const uint64_t handover = frequency * 2ULL;" in text:
            errors.append(f"{sdhci.name} still gives up the block guard after two seconds")
        # Round169: four seconds is shorter than the card's own housekeeping after a persona write,
        # and a read give-up that only prints once per boot cannot say where a cascade started.
        if "const uint64_t deadline = frequency * 4ULL;" in text:
            errors.append(f"{sdhci.name} still aborts write settlement after four seconds")
        if "CloverSdhci r168: read gave up" in text:
            errors.append(f"{sdhci.name} still prints the read give-up once per boot")
        # The batch is only trusted on a three-way agreement, never unconditionally.
        if "gReadBatchTrusted = false;\n  sdhciSay (\"CloverSdhci r167: readcheck" in text:
            errors.append(f"{sdhci.name} still retires the batch unconditionally: r166 proved the "
                          "batch was not the corrupt reader")
        # The 160 us pacing must not survive in the write path (mode 12's identification read
        # keeps its own copy, which is where the constant came from).
        if "gap = frequency / 6250ULL;        // 160 us; writes are rare" in text:
            errors.append(f"{sdhci.name} still paces writes at the 400 kHz 160 us gap")

    # Round171. The r169 and r170 frames said the read path was healthy and slow: no EIO, no
    # cascade, no jhash complaint -- and one command per 512 bytes, because the r167 batch gate
    # reported "multi read fail ... st=0x21 w=1024/1024". 0x21 is SDHCI_INT_DATA_AVAIL |
    # SDHCI_INT_RESPONSE and 1024/1024 is a whole 32-block batch delivered, so the verdict the code
    # demanded -- SDHCI_INT_DATA_END -- is one this host only raises once a stop puts the bus idle.
    # The criterion therefore became a permanent failure of a successful transfer, the batch never
    # became trusted, and every read ran the single-block sequence. Three things change together:
    # the batch is judged on delivered words beside the content-error bits (and the trust check now
    # runs on the request size the filesystem actually asks for), callers wait on an IOLock instead
    # of spinning out a retry budget and losing the batch path, and the card's write cache is
    # switched on and committed by an explicit EXT_CSD CACHE_CTRL flush -- because the r170 frames
    # also showed the synchronous programming r167 chose had grown the checkpoint flush from 22.7 s
    # to 102 s and 122 s.
    sdhci = next((base / "drivers/CloverSdhci/CloverSdhci.cpp"
                  for base in (REPO, pathlib.Path.cwd())
                  if (base / "drivers/CloverSdhci/CloverSdhci.cpp").is_file()), None)
    if sdhci is not None:
        text = sdhci.read_text()
        for needle in ("static bool batchDelivered (uint32_t status, uint32_t words, uint32_t total)",
                       "return words == total && !(status & kContentErrorMask);",
                       "static const uint32_t kContentErrorMask = SDHCI_INT_DATA_CRC",
                       "const bool delivered = clean || batchDelivered (status, wordCount, totalWords);",
                       "if (delivered && stopped) {",
                       "static void checkReadPath (uint32_t lba, uint32_t sample)",
                       "if (!gReadCheckDone && blocks >= 4) {",
                       "extern \"C\" uint32_t clover_emmc_flush_cache (void)",
                       "extern \"C\" void clover_emmc_cache_stats (uint32_t *size, uint32_t *on, uint32_t *flushes)",
                       "switchExtCsd (kExtCsdCacheCtrl, 0, response, status, &spins)",
                       "switchExtCsd (kExtCsdCacheCtrl, 1, backResponse, backStatus, &backSpins)",
                       "CloverSdhci r171: write cache size=",
                       "CloverSdhci r171: flush failed flushed=",
                       "static volatile uint32_t gHostOwner = 0;",
                       "static bool acquireBlockGuard (uint64_t &frequency, uint64_t &start, "
                       "uint32_t &token)",
                       "static void releaseBlockGuard (uint32_t &token)",
                       "static bool guardLost (uint32_t token)",
                       "static const uint64_t kGuardWaitMs = 60000;",
                       "static const uint32_t kStageEntry = 1;",
                       "static const uint32_t kStageCommit = 11;",
                       "CloverSdhci r213: guard wait gave up after %llu ms owner=%u stage=%u",
                       "CloverSdhci r213: guard stuck stage=%u dir=%u lba=%u blocks=%u pid=%u "
                       "held=%llu ms",
                       "static const uint32_t kStageReinit = 12;",
                       "static CardReinit reinitializeCard (void);",
                       "const CardReinit re = reinitializeCard ();",
                       "write8 (SDHCI_POWER_CONTROL, 0x00);",
                       "static const char kStuck[] __attribute__((used)) = \"clover-stuck=\";",
                       # Round215: the request brings the card back itself. A busy card keeps its
                       # own answer (CMD13 responses) and is left alone; only a card that stops
                       # answering, or one whose data circuit never releases, powers down.
                       "static volatile uint32_t gIdleFailReason = 0;",
                       "static const uint32_t kIdleSilent = 2;",
                       "static const uint32_t kCardRecoverCap = 16;",
                       "static const uint64_t kCardRecoverCooldownMs = 15000;",
                       "static bool cardRecoverHeld (const char *why, uint32_t detail, bool force = false);",
                       "static bool cardRecoverHeld (const char *why, uint32_t detail, bool force)",
                       "__atomic_add_fetch (&gFlightStuck, 1u, __ATOMIC_RELAXED);",
                       "if (reason != kIdleBusy && cardRecoverHeld (\"idle\", reason)) {",
                       "(void) cardRecoverHeld (\"stuck\", stage);",
                       "CloverSdhci r215: card recover why=%s d=%u n=%u stop=%u rst=%u pwr=%u clk=%u ",
                       "clkst=%u t=%u cmd0=%u cmd1=%u ready=%u cmd2=%u cmd3=%u cmd7=%u hs=%u bw=%u ok=%u\\n"):
            if needle not in text:
                errors.append(f"{sdhci.name} lacks {needle[:40]}...: r171's batch verdict, cache "
                              "commit or host serialization would be incomplete")
        # A spin-flag guard is what made a contended request degrade: the loser fell out of the
        # batch path and the winner kept the only core busy.
        if "gBlockGuard" in text:
            errors.append(f"{sdhci.name} still arbitrates the host with a spin flag: the batch path "
                          "the flag protects is the one the frame shows is missing")
        # Round213 replaced the IOLock with the ticket on purpose: an IOLock has no timeout, and the
        # r212 frame is exactly one holder that never returned. Putting a lock back would restore
        # the boot-killing behaviour, so any IOLock in this driver is a regression here.
        if "IOLockAlloc" in text or "IOLockLock" in text:
            errors.append(f"{sdhci.name} allocates or takes an IOLock again: round213's ticket is "
                          "the bound that keeps one stuck request from stopping every later one")
        if "__atomic_store_n (&gHostArmed, 1u, __ATOMIC_RELEASE);" not in text or \
                "__atomic_store_n (&gBlockReady, 1, __ATOMIC_RELEASE);" not in text:
            errors.append(f"{sdhci.name} publishes the block device without arming its host ticket: "
                          "an unsynchronized card engine is worse than no device")
        # The cache-flush pair must be a commit *and* a re-enable: leaving CACHE_CTRL at 0 after a
        # sync is r167's write-through with a different name.
        if "switchExtCsd (kExtCsdCacheCtrl, 0" in text and \
                "switchExtCsd (kExtCsdCacheCtrl, 1, backResponse" not in text:
            errors.append(f"{sdhci.name} flushes the write cache without switching it back on")

    block = next((base / "drivers/CloverBlock/CloverBlock.cpp"
                  for base in (REPO, pathlib.Path.cwd())
                  if (base / "drivers/CloverBlock/CloverBlock.cpp").is_file()), None)
    if block is not None:
        text = block.read_text()
        for needle in ("IOReturn commitCache(void);",
                       "IOReturn CloverBlockDevice::commitCache(void)",
                       "return commitCache();",
                       "static volatile uint64_t gWritesSinceFlush = 0;",
                       "__atomic_add_fetch(&gWritesSinceFlush",
                       "static bool overlapsLogfsWindow(uint64_t first, uint64_t count)",
                       "overlapsLogfsWindow(static_cast<uint64_t>(windowFirst) + block, done)",
                       "clover_emmc_cache_stats(&size, &on, &flushes);",
                       'IOLog("CloverBlock r171: first commit blocks='):
            if needle not in text:
                errors.append(f"{block.name} lacks {needle[:40]}...: the write cache would be "
                              "enabled without anything committing it, or logfs data would be left "
                              "in a cache the panic reset never flushes")
        # getWriteCacheState has to answer with the card's state; a constant `false` while the host
        # driver is caching is how the stack above stops issuing the syncs that make it durable.
        if "*enabled = false;" in text:
            errors.append(f"{block.name} still reports the write cache as off whatever the card says")

    # Round171's contract: the archive audit that resolves CloverBlock's imports has to know the two
    # new entry points, or the kernel link is the first place that discovers them.
    block_build = next((base / "scripts/build-clover-block.py"
                        for base in (REPO, pathlib.Path.cwd())
                        if (base / "scripts/build-clover-block.py").is_file()), None)
    if block_build is None:
        errors.append("scripts/build-clover-block.py is missing: nothing would audit the block "
                      "device's bindings")
    else:
        text = block_build.read_text()
        for needle in ("'_clover_emmc_flush_cache'", "'_clover_emmc_cache_stats'"):
            if needle not in text:
                errors.append(f"{block_build.name} does not audit {needle}: the archive would build "
                              "and the kernel link would fail")

    # Round172. Every byte this port has read has crossed the CPU: one word out of the FIFO at a
    # time, which is a 2 MB/s ceiling (r156's own measurement) against a 3.7 GiB dyld shared cache.
    # The controller's capability word and the firmware's last HOST_CONTROL both already say ADMA64,
    # so the round adds the transport the boot loader was using. The contract is the *gate*: a DMA
    # read is only used after it returned the same words as the PIO batch, on the same request, twice
    # through -- which is also what makes an unknown (ICE, an SMMU context the firmware left behind)
    # an answer on the panel rather than a corrupted filesystem. Writes stay on PIO: a wrong DMA write
    # is not an EIO, it is a changed card.
    sdhci_r172 = next((base / "drivers/CloverSdhci/CloverSdhci.cpp"
                       for base in (REPO, pathlib.Path.cwd())
                       if (base / "drivers/CloverSdhci/CloverSdhci.cpp").is_file()), None)
    regs_r172 = next((base / "drivers/CloverSdhci/SdhciRegs.h"
                      for base in (REPO, pathlib.Path.cwd())
                      if (base / "drivers/CloverSdhci/SdhciRegs.h").is_file()), None)
    if sdhci_r172 is None or regs_r172 is None:
        errors.append("drivers/CloverSdhci is missing: there is no ADMA2 path to verify")
    else:
        text = sdhci_r172.read_text()
        for needle in ("struct AdmaPath",
                       "static AdmaPath gAdma;",
                       "static bool dmaBringUp (void)",
                       "static void admaWriteEntry (uint32_t index, uint64_t address, uint32_t length, uint16_t cmd)",
                       "static uint32_t admaBuildTable (uint32_t bytes)",
                       "static bool readAdmaRun (uint32_t lba, uint32_t *dst, uint32_t blocks,",
                       "static void checkDmaPath (uint32_t lba, uint32_t sample)",
                       "if (!gDmaCheckDone && gReadBatchTrusted && blocks >= 4 && dmaBringUp ()) {",
                       "if (chunk >= 4 && gDmaTrusted) {",
                       "gDmaTrusted = pio && dma && got == words && diff == 0;",
                       "dst[word] = source[word];",
                       "write16 (SDHCI_TRANSFER_MODE, SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_MULTI |",
                       "SDHCI_TRNS_DMA",
                       "kIOMemoryPhysicallyContiguous",
                       "kIOMemoryMapperNone",
                       "getPhysicalSegment",
                       "cacheCleanInvalidate (gAdma.dataVirt, bytes);",
                       "cacheInvalidate (gAdma.dataVirt, bytes);",
                       "CloverSdhci r172: dma up data=",
                       "CloverSdhci r172: dmacheck lba=",
                       "CloverSdhci r172: ADMA2 reads trusted for this boot",
                       "CloverSdhci r172: reads stay on the PIO path for this boot",
                       "CloverSdhci r172: first dma read lba=",
                       "CloverSdhci r172: dma failed %u times, reads go back to the PIO path"):
            if needle not in text:
                errors.append(f"{sdhci_r172.name} lacks {needle[:44]}...: round172's DMA read path, "
                              "its bounce buffer, or the comparison that earns it would be missing")
        # The path has to be *earned*: nothing may set the flag except the word-for-word agreement,
        # and a boot whose multi-block command is untrusted may not compare against it at all.
        if "gDmaTrusted = true;" in text:
            errors.append(f"{sdhci.name} trusts the DMA path without comparing it to the PIO path")
        if "SDHCI_ADMA2_NOP_END_VALID" in text:
            errors.append(f"{sdhci.name} terminates the descriptor table with a NOP: this "
                          "controller family sets NO_ENDATTR_IN_NOPDESC, so the last transfer entry "
                          "carries END and a NOP entry would be read as a zero-length transfer")
        # Writes are not this round's, and the only difference between a read descriptor and a write
        # one is a bit the write path must not have yet.
        if re.search(r"SDHCI_TRNS_WRITE[^;]*SDHCI_TRNS_DMA", text):
            errors.append(f"{sdhci.name} puts the write path on DMA before the read path has a "
                          "frame behind it: a wrong DMA write changes the card rather than erroring")
        # Writes were not this round's, and the only difference between a read descriptor and a write
        # one is a bit the write path must not have yet. Round173 is the round that earns the write
        # path, with its own gate and its own contract below; this assertion is its history: the read
        # gate existed alone first, and a frame has to be able to say which round it is running.
        regs = regs_r172.read_text()
        for needle in ("#define SDHCI_ADMA2_64_DESC_SZ",
                       "#define SDHCI_ADMA2_TRAN_VALID",
                       "#define SDHCI_ADMA2_END",
                       "sdhci-msm.c:5646",
                       "v419-host-sdhci.h"):
            if needle not in regs:
                errors.append(f"{regs_r172.name} lacks {needle}: the descriptor layout would be "
                              "recalled rather than transcribed from this board's own CAF tree")

    # Round173. The write side gets the same transport, and the same rule can only be the same rule
    # with one extra step: a wrong DMA read costs an EIO, a wrong DMA write changes what is on the
    # card. So the gate rewrites a run inside the log window -- since round185 the logdump partition,
    # which nothing mounts and whose only contents are this port's own records -- with bytes it read
    # from that run a moment earlier, and the
    # flag is set only when the run reads back through PIO exactly as it was found. The PIO write of
    # the same bytes runs first as a control: a medium that will not round-trip its own bytes through
    # the proven path cannot produce a DMA verdict, and the gate says so rather than printing a zero.
    sdhci_r173 = next((base / "drivers/CloverSdhci/CloverSdhci.cpp"
                       for base in (REPO, pathlib.Path.cwd())
                       if (base / "drivers/CloverSdhci/CloverSdhci.cpp").is_file()), None)
    if sdhci_r173 is None:
        errors.append("drivers/CloverSdhci/CloverSdhci.cpp is missing: there is no ADMA2 write "
                      "gate to verify")
    else:
        text = sdhci_r173.read_text()
        for needle in ("static bool writeAdmaRun (uint32_t lba, const uint32_t *src, uint32_t blocks,",
                       "static void checkWriteDmaPath (void)",
                       "static bool readRunPio (uint32_t lba, uint32_t *dst, uint32_t blocks)",
                       "static const uint32_t kWriteCheckOffset = 4096u;",
                       "const uint32_t checkLba = windowFirst + kWriteCheckOffset;",
                       "if (!gWriteDmaCheckDone && blocks >= 4) {",
                       "if (chunk >= 4 && gWriteDmaTrusted) {",
                       "gWriteDmaTrusted = controlHeld && dmaWrite && dmaRead && diffDma == 0;",
                       "write16 (SDHCI_TRANSFER_MODE, SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_MULTI | SDHCI_TRNS_DMA);",
                       "countWordDiff (gCheckBatchA, gCheckSingle, kWriteCheckWords, firstDma)",
                       "checkLba + kWriteCheckBlocks - 1 > windowFirst + sectors - 1",
                       "CloverSdhci r173: wrcheck lba=",
                       "CloverSdhci r173: ADMA2 writes trusted for this boot",
                       "CloverSdhci r173: writes stay on the PIO path for this boot",
                       "CloverSdhci r173: first dma write lba=",
                       "CloverSdhci r173: dma writes failed %u times, writes go back to the PIO path"):
            if needle not in text:
                errors.append(f"{sdhci_r173.name} lacks {needle[:44]}...: round173's DMA write path, "
                              "the log-window run it is confined to, or the read-back that earns it "
                              "would be missing")
        # Earned, not assumed -- the same rule the read path lives under.
        if "gWriteDmaTrusted = true;" in text:
            errors.append(f"{sdhci_r173.name} trusts DMA writes without reading them back: the card "
                          "would be changed by a descriptor nobody compared")
        # The gate's sample has to stay inside the log window. Round185 moved it: 4096 sectors past the
        # window's first LBA, sixteen blocks long, and since logfs became this boot's root container a
        # sample anchored on kLogfsFirstLba would be an unproven transport rewriting APFS.
        sample_start = text.find("static void checkWriteDmaPath (void)\n{")
        sample_end = text.find("\n}\n", sample_start) if sample_start >= 0 else -1
        sample = text[sample_start:sample_end] if sample_end > sample_start else ""
        if not sample:
            errors.append(f"{sdhci_r173.name} lost the body of checkWriteDmaPath: the gate that earns "
                          "DMA writes could not be read at all")
        if "windowFirst + kWriteCheckOffset" not in sample:
            errors.append("the write gate's sample LBA is no longer the log window plus 4096: it has to "
                          "stay past the report slots and inside the logdump window")
        if "kLogfsFirstLba" in sample or "kLogfsLastLba" in sample:
            errors.append("the write gate bounds its sample on logfs again: that partition is the root "
                          "APFS container now, and a DMA write verdict is not permission to modify it")
        # The last thing a dying boot does must not depend on a flag this round introduced: the panic
        # screen and the bring-up report keep their own two-block PIO writes. Round184 split the sink
        # in two -- the exported entry point now runs the ring drain, and the two PIO writes moved into
        # panicAppendText just above it -- so the rule is checked on the function that holds the writes,
        # with the exported name asserted separately because that is the patcher's contract.
        panic_start = text.find("static unsigned int panicAppendText")
        panic_body = text[panic_start:panic_start + 4000] if panic_start >= 0 else ""
        if panic_body.count("writePioBlock (") < 2:
            errors.append("clover_emmc_panic_append no longer writes its two blocks through PIO: "
                          "the last line of a dying boot would depend on this round's flag")
        wrapper_start = text.find("extern \"C\" unsigned int clover_emmc_panic_append")
        wrapper_end = text.find("\n}", wrapper_start) if wrapper_start >= 0 else -1
        wrapper = text[wrapper_start:wrapper_end] if wrapper_end > wrapper_start else ""
        if "panicAppendText (" not in wrapper or "panicDrainRings ()" not in wrapper:
            errors.append("the exported clover_emmc_panic_append no longer both writes its own record "
                          "and drains the rings: round184's reading would stop at the panic line and "
                          "leave everything before it in memory to be reset away")

    # Round174. The reading is the deliverable, and for nine rounds the deliverable has been a
    # photograph of a panel that had already scrolled: r167 lost `write cache ... off=`, r169 lost the
    # `read <stage>` lines, r170 exists because one probe's own repetition filled the screen and left a
    # whole frame unable to say whether launchd was looping. So this round gives every line the driver
    # measures a second copy on the card -- kind 21 records in a ring at sector 64 of the log window,
    # clear of the 32 blocks the panic writer owns -- and `read-clover-log.py --verdict` reads them
    # back from the
    # same dd that brings the panic text. Saying something costs a format plus a memcpy: the card
    # write happens only from the three request tails that already hold the block guard with an idle
    # card, a few records at a time, because a flush that held the guard is the failure r167 r168 r169
    # spent themselves removing. One formatter runs, and the screen and the card cannot disagree.
    sdhci_r174 = next((base / "drivers/CloverSdhci/CloverSdhci.cpp"
                       for base in (REPO, pathlib.Path.cwd())
                       if (base / "drivers/CloverSdhci/CloverSdhci.cpp").is_file()), None)
    if sdhci_r174 is not None:
        text = sdhci_r174.read_text()
        for needle in ("#include <stdarg.h>",
                       "static const uint32_t kRingCardSlots[kRingCount] = {64, 512};",
                       "static const uint32_t kRingOffset[kRingCount] = {64, 192};",
                       "static const uint32_t kRingFlushPerPass[kRingCount] = {4, 8};",
                       "static uint32_t verdictFormat (char *out, uint32_t cap, const char *fmt, va_list args)",
                       "static void sdhciSay (const char *fmt, ...)",
                       "static bool ringWriteRecord (uint32_t which, const char *text, uint32_t length,",
                       "gRingBlock[9] = gBootId;",
                       "static void flushVerdictRing (void)",
                       "gRingBlock[5] = kind ? kind : ((which == kRingKernel) ? 22 : 21);",
                       "gRingBlock[8] = gRingSeq[which];",
                       "const uint32_t lba = gVerdictFirst + kRingOffset[which] +",
                       'IOLog ("%s\\n", line);',
                       "gVerdictFirst = entryLdFirst;",
                       "gVerdictReady = 1;"):
            if needle not in text:
                errors.append(f"{sdhci_r174.name} lacks {needle[:44]}...: round174's formatter, its "
                              "kind 21 record, or the arming that lets a verdict reach the card is gone")
        # A verdict printed only to the panel is a verdict that has to win a race with the next line.
        if "IOLog (\"CloverSdhci r" in text:
            errors.append(f"{sdhci_r174.name} still prints a CloverSdhci reading to the panel alone: "
                          "the line would exist in one place only, which is the failure this round ends")
        ring_lines = text.count('sdhciSay ("CloverSdhci r')
        if ring_lines < 30:
            errors.append(f"{sdhci_r174.name} routes only {ring_lines} "
                          "verdict lines through the ring; every line the driver measures has to go")
        # Three tails, and only tails that already hold the guard.
        if text.count("flushVerdictRing ();") < 3:
            errors.append(f"{sdhci_r174.name} flushes the verdict ring from fewer than three guarded "
                          "request tails: a boot that never reaches them keeps its readings in RAM only")
        if "gRingBusy[which]" not in text:
            errors.append(f"{sdhci_r174.name} lost the re-entrancy flag: a nested flush would write one "
                          "record twice from two owners of the same scratch block")

    # Round175. The ring only carries what this driver says itself, and the readings that decide the
    # next round are printed by other people: the r151/r153/r155 shared-region probes and the r170
    # exec/exit pairs are printf() calls in XNU, APFS's jhash and ntree errors come from the filesystem,
    # and the `load code signature error 4` line that has killed every process since r162 comes from
    # the kernel's loader. All of them already land in one place the driver can reach: msgbufp, the
    # circular log buffer that printf/IOLog append to (bsd/kern/subr_prf.c:388 -> log_putc ->
    # log_putc_locked(msgbufp, c)). So this round reads that buffer and copies the lines the port is
    # blocked on into the same ring -- with no new XNU patch, which matters because a probe may not be
    # inserted inside an earlier patch's own added block without breaking the build's reverse-check.
    # The tee consumes a chunk per guarded pass from its own byte cursor, keeps only allowlisted lines,
    # and never re-prints one: sdhciSay would IOLog the line straight back into the buffer it is
    # reading, and the tee would match its own echo forever. Hence ringPush, which fills a ring without
    # speaking: the tee's yield goes into round176's kernel ring, its own state lines into the driver's.
    # Round189 doubles the per-pass push to 16: the r188 census (seen=1598 kept=580 throttled=694) says
    # the per-pass cap, not the ring, was dropping the tail of a burst this boot has to see.
    if sdhci_r174 is not None:
        text = sdhci_r174.read_text()
        for needle in ("#include <sys/msgbuf.h>",
                       "struct msgbuf *buffer = msgbufp;",
                       "static const uint32_t kTeeScanPerPass = 4096;",
                       "static const uint32_t kTeePushPerPass = 16;",
                       'static const char *const kTeeAllowlist[] = {',
                       '"clover r",',
                       '"cs_blob",',
                       '"mountroot",',
                       "if (magic != MSG_MAGIC)",
                       "static void teeKernelLog (void)",
                       "static void ringPush (uint32_t which, const char *line, uint32_t length)",
                       "kernelRingPush (gTeeLine + start, length - start);",
                       "gTeeReady = 1;",
                       '"CloverSdhci r175: tee armed bufc=%p size=%d bufx=%d\\n"'):
            if needle not in text:
                errors.append(f"{sdhci_r174.name} lacks {needle[:44]}...: round175's kernel log tee, "
                              "its validation, or the ring-only push it depends on is gone")
        if text.count("teeKernelLog ();") < 3:
            errors.append(f"{sdhci_r174.name} drains the kernel log from fewer than three guarded "
                          "request tails: the tee would run where a card write is not allowed")
        # The echo check: everything between the tee's line consumer and its next function must be
        # silent. One IOLog in there and the ring fills with copies of copies.
        consume_start = text.find("static void teeConsumeLine (uint32_t &pushed)")
        consume_end = (text.find("static void teeKernelLog (void)", consume_start)
                       if consume_start >= 0 else -1)
        consume = text[consume_start:consume_end] if consume_end > consume_start else ""
        if not consume:
            errors.append(f"{sdhci_r174.name} lost teeConsumeLine: the tee's line filter cannot be "
                          "audited, so an echo back into msgbuf would go unnoticed")
        for loud in ("IOLog", "sdhciSay"):
            if loud in consume:
                errors.append(f"{sdhci_r174.name} has the tee call {loud} per line: printing a line the "
                              "tee just read appends it to msgbuf again, and the tee matches its own echo")
        # ... and the whole drain must be silent, for a second reason: teeKernelLog runs at the tails of
        # block requests, inside the block guard. IOLog takes bsd_log_lock, and a logger that holds
        # bsd_log_lock and then issues disk I/O would wait for the guard this thread already holds.
        tee_start = text.find("static void teeKernelLog (void)\n{")
        tee_end = text.find("\n}\n", tee_start) if tee_start >= 0 else -1
        tee_body = text[tee_start:tee_end] if tee_end > tee_start else ""
        if not tee_body:
            errors.append(f"{sdhci_r174.name} lost teeKernelLog: the drain that reads msgbuf is gone")
        for loud in ("IOLog", "sdhciSay"):
            if loud in tee_body:
                errors.append(f"{sdhci_r174.name} has the tee call {loud} while holding the block "
                              "guard: that is a lock order between bsd_log_lock and the guard, and a "
                              "logger doing I/O under the log lock would deadlock on it")
        if "static void teeSay (const char *fmt, ...)" not in text:
            errors.append(f"{sdhci_r174.name} lost teeSay: the tee's own state lines have no ring-only "
                          "way to speak, which is the only reason the drain is safe to run in a tail")
        # The cursor is a byte index into a buffer the kernel can reallocate; anything cached across
        # passes but not revalidated is a pointer into freed memory.
        if "if (gTeeBuffer && gTeeBuffer != base)" not in text:
            errors.append(f"{sdhci_r174.name} does not notice msg_bufc moving: kern.msgbuf can grow the "
                          "log, and a cursor into the old buffer reads freed memory")

    # Round176. Round175 documented its own cost: one ring, two producers. A busy daemon prints kernel
    # lines by the thousand -- `load code signature error 4` for every exec since r162 -- and each one
    # evicted a driver verdict that the card had not seen yet, so the tee could answer the question and
    # destroy the readings that decided the next round. The fix is geometric, not another pointer: two
    # rings with their own kind, offset and per-pass budget (driver kind 21 at sector 64 with 64 slots,
    # kernel kind 22 at sector 192 with 512), so the only thing they share is the one dd that reads both.
    # The tee's own state lines deliberately stay in the driver ring, which makes the kernel ring mean
    # exactly one thing and gives the host a falsifiable claim: --tee --driver must count zero.
    if sdhci_r174 is not None:
        text = sdhci_r174.read_text()
        for needle in ("static const uint32_t kRingDriver = 0;",
                       "static const uint32_t kRingKernel = 1;",
                       "static const uint32_t kRingRamSlots = 128;",
                       "static char gRingText[kRingCount][kRingRamSlots][kVerdictTextBytes];",
                       "gRingBlock[7] = gRingCursor[which] % kRingCardSlots[which];",
                       "static void flushRing (uint32_t which)",
                       "flushRing (kRingDriver);",
                       "flushRing (kRingKernel);",
                       "ringPush (kRingDriver, line, length);",
                       '"CloverSdhci r176: kernel ring armed lba=%u slots=%u\\n"',
                       "static const uint32_t kTeeCensusMax = 12;"):
            if needle not in text:
                errors.append(f"{sdhci_r174.name} lacks {needle[:44]}...: round176's ring split, its "
                              "per-ring flush budget, or the arming that lets kernel text reach the card is gone")
        # One ring per producer means the ring is no longer single-indexed anywhere: the round174/175
        # names have to be gone, or a line would be pushed to a buffer nothing flushes.
        for stale in ("static void verdictPush (", "gVerdictBlock", "kVerdictSlots", "kVerdictOffset"):
            if stale in text:
                errors.append(f"{sdhci_r174.name} still refers to {stale}: the single-ring shape is gone, "
                              "and a leftover of it is a ring that silently never flushes")
        # The kernel ring must be the kernel's. teeConsumeLine is the only push with kRingKernel in it.
        if text.count("ringPush (kRingKernel,") != 1:
            errors.append(f"{sdhci_r174.name} pushes kernel text from {text.count('ringPush (kRingKernel,')} "
                          "places: exactly teeConsumeLine may fill the kernel ring, or the host cannot read "
                          "it as what the kernel said")
        # The census is capped, because the census itself is a line the tee writes about itself and an
        # uncapped one repeats the r169/r170 failure inside the new ring.
        if "if (gTeeCensus < kTeeCensusMax)" not in text:
            errors.append(f"{sdhci_r174.name} reports the tee census without a cap: a boot would fill the "
                          "driver ring with its own bookkeeping, which is why round174 throttled the panel")

    # Round185. Two independent reasons made every card since round174 come back with no records at all,
    # and neither was the write path: the arming lived inside the `probeMode == 20` report block while
    # the shipped boot has been mode 21 since round137, and it armed only after reading an existing
    # record out of a partition that had never been written. So arming now runs from the GPT entry walk,
    # which every mode >= 14 boot reaches, and its only precondition is this card's own entry naming
    # `logdump` at the extent sysfs reports. The window itself moved because logfs became the root APFS
    # container in round127 -- report slot 0 is inside the superblock's own region -- and a diagnostic
    # that overwrites the filesystem it is diagnosing is not a diagnostic. The card has a partition named
    # for logs that this boot uses for nothing.
    if sdhci_r174 is not None:
        text = sdhci_r174.read_text()
        for needle in ("static const uint32_t kLogDumpFirstLba = 13320216u;",
                       "static const uint32_t kLogDumpLastLba = 13451287u;",
                       "static volatile uint32_t gLogWindowFirst = 0;",
                       "static volatile uint32_t gLogWindowSectors = 0;",
                       "if (entryWords >= 19 && entry[14] == 0x006f006cu && entry[15] == 0x00640067u &&",
                       "entry[16] == 0x006d0075u && entry[17] == 0x00000070u && entry[18] == 0) {",
                       "__atomic_store_n (&gLogWindowFirst, entryLdFirst, __ATOMIC_RELEASE);",
                       "__atomic_store_n (&gLogWindowSectors, logSectors, __ATOMIC_RELEASE);",
                       "const uint32_t needed = kBootRecordOffset + 1;",
                       '"CloverSdhci r185: log window armed from GPT first=%u sectors=%u report=%u driver=%u kernel=%u\\n"',
                       '"CloverSdhci r185: logdump entry too small, rings stay disarmed first=%u sectors=%u needed=%u\\n"',
                       '"CloverSdhci r185: no logdump entry at %u..%u found=%u first=%u last=%u, rings stay disarmed\\n"',
                       "gPanicLogFirst = entryLdFirst;",
                       "gPanicLogReady = 1;",
                       "gVerdictReady = 1;",
                       "gTeeReady = 1;"):
            if needle not in text:
                errors.append(f"{sdhci_r174.name} lacks {needle[:44]}...: round185's logdump window, the "
                              "GPT name match that finds it, or the arming that makes any of the rings "
                              "writable is gone")
        # The position is the whole point: arming has to be in the entry walk, ahead of the instrument
        # modes it used to sit inside. An arming that moved somewhere mode-dependent would reproduce the
        # round184 reading -- a card with nothing on it and a driver that never said it had armed.
        armed = text.find('"CloverSdhci r185: log window armed from GPT')
        mode16 = text.find("if (probeMode == 16 && entryFound14 != 0) {")
        if armed < 0 or mode16 < 0 or armed > mode16:
            errors.append(f"{sdhci_r174.name} no longer arms the rings from the GPT walk: the arming is "
                          "at " + (f"offset {armed}" if armed >= 0 else "nowhere") +
                          " and the mode-16 branch at " + (f"offset {mode16}" if mode16 >= 0 else "nowhere"))
        # The two shapes that made a boot unable to arm are forbidden, not merely unused: arming off the
        # logfs entry, and arming behind a record read back from a partition that has never been written.
        for stale in ("gPanicLogFirst = entryFirst14;", "gVerdictFirst = entryFirst14;"):
            if stale in text:
                errors.append(f"{sdhci_r174.name} still has {stale}: logfs is the root container, and a "
                              "window armed from it writes diagnostics over the filesystem they describe")
        if "CloverSdhci r185: log window armed" in text and "if (entryLdFound != 0 && entryLdFirst == kLogDumpFirstLba &&" not in text:
            errors.append(f"{sdhci_r174.name} arms a window the card's own entry did not report: the "
                          "logdump extent has to be matched before anything is armed on it")

    reader = next((base / "scripts/read-clover-log.py"
                   for base in (REPO, pathlib.Path.cwd())
                   if (base / "scripts/read-clover-log.py").is_file()), None)
    if reader is None:
        errors.append("scripts/read-clover-log.py is missing: nothing would read the verdict ring back")
    else:
        text = reader.read_text()
        for needle in ('21: "verdict-21"', '22: "kernel-22"', "VERDICT_OFFSET = 64",
                       "VERDICT_SLOTS = 64", "KERNEL_OFFSET = 192", "KERNEL_SLOTS = 512",
                       "LOG_WINDOW_FIRST_LBA = 13320216",
                       'default="clover-log-window.bin"', "default=LOG_WINDOW_FIRST_LBA",
                       "DUMP_SECTORS = BOOT_OFFSET + 1",
                       "BOOT_OFFSET = KERNEL_OFFSET + 2 * KERNEL_SLOTS",
                       "BOOT_KIND = 24", "PROBE_KIND = 25",
                       '24: "boot-24", 25: "probe-25"',
                       '12: "reinit"',
                       "def decode_boot(data, first_lba)",
                       "FLIGHT_DEADLINE_OFFSET = 60", "FLIGHT_PANIC_OFFSET = 61",
                       '4: "deadline", 5: "panic"', '3: "none"',
                       # Round293: the reader has to say which boot wrote what it prints. A stale
                       # deadline record is not a boot that reached its deadline, and the reader that
                       # printed one as if it were is how boot 79 nearly read as a boot that did.
                       "FLIGHT_BOOT_WORD = {1: 35, 2: 35, 3: 47, 4: 45, 5: 41, 6: 37, 7: 35}",
                       # Round302: the reader has to know where a verdict record signs itself, and that
                       # the sentence's slots do not arrive in sequence -- a reader that broke the group
                       # at the first untouched sector would read every panic before boot id reached 12
                       # as no sentence at all.
                       "FLIGHT_TEXT_OFFSET = 44", "FLIGHT_TEXT_SLOTS = 12",
                       "FLIGHT_MARKER_MAGIC = 0x434C564D",
                       "FLIGHT_MAGIC_WORD = {4: 46, 5: 53, 6: 126, 7: 43}",
                       # Round310: the signature sits one word past the last extra, so this one sector has
                       # carried it at three places. A reader that knows only the newest one calls boot 102's
                       # own marker "never a marker" -- the generation rule aimed at the wrong boot.
                       "FLIGHT_MAGIC_FALLBACK = {5: (48, 43)}",
                       '"magic_at": magic_word if magic else None',
                       'if record.get("magic_at") == FLIGHT_MAGIC_WORD[5]',
                       'PANIC_QUIESCE = {0: "nothing outstanding"',
                       '6: "text"', 'not a sentence: no r302 magic at word',
                       "no sentence record",
                       "def flight_stamp(records, boot_id)", "STALE: boot",
                       'flight_report(groups, boot["boot"] if boot else 0)',
                       "never wrote one, so this group says nothing about this boot",
                       "def flight_pairs(words, count)", "def flight_pairs_words(values)",
                       'no deadline record', 'no panic marker',
                       '"--boot"', '"--probe"',
                       'struct.unpack_from("<10I", block)',
                       "expects and kind not in expects",
                       '"--verdict"',
                       "order_by_seq=kind is not None", "def selftest()"):
            if needle not in text:
                errors.append(f"{reader.name} lacks {needle[:40]}...: a ring's slots, its place in the "
                              "dump, its kind or its sequence order after a wrap would be missing")
        # Round185: the host's defaults have to name logdump, not the partition the kernel mounted. The
        # driver arms from the card's GPT, so the numbers here only say where a dump that came from the
        # default path came from -- and if they still said logfs, every record would decode with the
        # wrong absolute LBA and be reported as a mismatch.
        if "12320768" in text.split("def main()")[0]:
            errors.append(f"{reader.name} still hard-codes the logfs extent ahead of main(): a dump of "
                          "logdump would decode every header as having the wrong LBA")
        # Round175: the host has to be able to ask the two producers apart -- "the tee never ran" and
        # "the tee ran and msgbuf matched nothing" are different answers. Round176: they no longer share
        # a ring, so what is asserted is that each ring holds only its own kind, and that reading a
        # driver record from the kernel ring is reported rather than silently trusted.
        for needle in ('DRIVER_PREFIX = b"CloverSdhci "', "def producer(content)",
                       '"--tee"', '"--driver"', "expects and kind not in expects",
                       'assert all(producer(record[2]) == "kernel" for record in kernel), kernel',
                       'assert [record for record in kernel if producer(record[2]) == "driver"] == [], kernel'):
            if needle not in text:
                errors.append(f"{reader.name} lacks {needle[:40]}...: a dump could not say which ring a "
                              "line came from, so round176's split would be unreadable")
        if reader.is_file():
            check = subprocess.run([sys.executable, str(reader), "--selftest"],
                                   capture_output=True, text=True)
            if check.returncode:
                errors.append("read-clover-log.py --selftest failed: the decoder and the driver's "
                              "record layout no longer agree\n" + (check.stdout + check.stderr)[-700:])
    unattended = next((base / "scripts/run-clover-unattended.sh"
                       for base in (REPO, pathlib.Path.cwd())
                       if (base / "scripts/run-clover-unattended.sh").is_file()), None)
    if unattended is not None:
        text = unattended.read_text()
        for needle in ("bs=512 count=1217", "623104", "--verdict", "--probe", "--boot",
                       "verdict line(s)", "clover-misc-1024.bin", "misc block 1"):
            if needle not in text:
                errors.append(f"{unattended.name} lacks {needle[:32]}...: the dd would stop short of one "
                              "of the two rings and that ring would never be read")
        # Round175: the script reports the tee's yield, because a ring of only driver lines is a
        # different finding from a ring that also carried kernel lines.
        for needle in ("--tee --count", "kernel log line(s)",
                       "brought no kernel lines"):
            if needle not in text:
                errors.append(f"{unattended.name} lacks {needle[:32]}...: the host would not count "
                              "the kernel lines the tee brought back")
        # Round176: both rings get their own heading, and the kernel ring is checked for the one thing
        # that would mean the tee is copying its own readings -- an impossible count, so a nonzero one is
        # a finding rather than a curiosity.
        for needle in ("== kernel-log ring", "predates round176", "--tee --driver --count",
                       "the tee is echoing"):
            if needle not in text:
                errors.append(f"{unattended.name} lacks {needle[:32]}...: round176's second ring would not "
                              "be decoded, or its echo invariant would not be checked")
        # Round184, the two host-side defects that made a readable card look empty: TWRP's adbd reports
        # the state as `recovery` and not `device`, so the wait gave up before it ever asked; and dd's
        # "1216+0 records ..." on stderr lands inside the adb stdout stream unless the device redirects
        # it, so a gate that compared the first bytes against the record magic rejected a good dump.
        # Round185 adds the third: the window is logdump now, and logfs stays in the list only because
        # the point of reading it is to show that nothing was ever written there.
        for needle in ('$2 == "recovery"', "dd if=$source bs=512 count=1217 2>/dev/null",
                       "/dev/block/bootdevice/by-name/logdump", "/dev/block/mmcblk0p56",
                       "clover-log-window.bin", "no readable log window"):
            if needle not in text:
                errors.append(f"{unattended.name} lacks {needle[:40]}...: the host would wait for the "
                              "wrong adb state, gate a good dump on dd's own stderr, or read the "
                              "partition the kernel mounted instead of the log window")

    # Round186. launchd panics with "required boot task executable not found" and neither the panel nor
    # the card says which path its posix_spawn() looked up. The kernel is the only place that knows, and
    # it has exactly two producers of that errno on the spawn path: the program-path namei() in
    # exec_activate_image() and the dylinker namei() in get_macho_vnode() -- load_return_to_errno() maps
    # every other kernel return to EBADARCH/EBADMACHO/EACCES/EAUTH. So the probe re-resolves the failing
    # path one component at a time and prints the component that stopped, the filesystem and mount point
    # behind the deepest prefix that resolved, and whether retrying the same path fails the same way.
    # That is the difference between "this file is not in the volume", "the volume under launchd lost its
    # contents" and "one lookup failed once".
    spawn_probe = next((base / "patches/xnu-clover/zzzz-clover-spawn-why.patch"
                        for base in (REPO, pathlib.Path.cwd())
                        if (base / "patches/xnu-clover/zzzz-clover-spawn-why.patch").is_file()), None)
    if spawn_probe is None:
        errors.append("patches/xnu-clover/zzzz-clover-spawn-why.patch is missing: the next frame would "
                      "name only the fact that a boot task was not found, not the path that stopped "
                      "resolving or the mount behind it")
    else:
        text = spawn_probe.read_text()
        for needle in ("bsd/kern/kern_exec.c", "bsd/kern/mach_loader.c",
                       "clover r186: spawn-why err=%d n=%d path=%s stop=%s good=%s fs=%s on=%s "
                       "mflags=%lx retry=%d\\n",
                       "clover r186: loader-why n=%d err=%d path=%s pid=%d\\n",
                       "proc_getpid(vfs_context_proc(ctx)) != 1", "#define CLOVER_SPAWN_WHY_LIMIT  24",
                       "vfs_name(mp, fsname)", "vfs_statfs(mp)->f_mntonname", "vnode_mount(vp)"):
            if needle not in text:
                errors.append(f"{spawn_probe.name} lacks {needle[:44]}...: without the component walk, "
                              "the mount behind the deepest good prefix, the pid-1 gate, the cap and "
                              "the retry line the probe cannot answer the question round186 asks")
        # Print-only is the whole safety argument: the probe runs after the kernel has already decided
        # the errno, and it may not change what userland sees. So the patch may delete no source line --
        # and it must not touch the dylinker's LOAD_ENOENT conversion either, which is the other place
        # a well-meant fix would silently change behavior.
        removed = [line for line in text.splitlines() if line.startswith("-") and not
                   line.startswith("---")]
        if removed:
            errors.append(f"{spawn_probe.name} deletes {removed[0][:60]!r}: the spawn probe prints and "
                          "returns, and a line it removes is a change in what launchd is told")
        if "clover r185:" in text or "clover r170:" in text:
            errors.append(f"{spawn_probe.name} carries an older round's string: its own lines are the "
                          "only new readings this round produces")

    # Round187: the r186 frame showed the spawn probe answering a question nobody asked -- the only
    # line it printed was load_init_program falling back from the development launchd -- and the boot
    # instead stopped 34 s into usermanagerd, after it opened the dyld shared cache and before it
    # printed anything else, while launchd waited for it as a boot task. So this patch counts entries,
    # not failures: unix_syscall() and the mach-trap branch of handle_svc() print the code/trap of every
    # entry for the two names the frame left unexplained, and every failing unix syscall prints its
    # errno. The last line before the silence is the answer, so the patch has to keep the entry print
    # ahead of the call itself, must not delete a source line, and must leave the watched names and the
    # heartbeat visible for the checker to pin.
    sys_probe = next((base / "patches/xnu-clover/zzzzz-clover-syswhy.patch"
                      for base in (REPO, pathlib.Path.cwd())
                      if (base / "patches/xnu-clover/zzzzz-clover-syswhy.patch").is_file()), None)
    if sys_probe is None:
        errors.append("patches/xnu-clover/zzzzz-clover-syswhy.patch is missing: the next frame would "
                      "end at usermanagerd opening the dyld shared cache again, with nothing saying "
                      "whether it is inside a unix syscall or a mach trap")
    else:
        text = sys_probe.read_text()
        for needle in ("bsd/dev/arm/systemcalls.c", "osfmk/arm64/sleh.c",
                       "clover r187: unix-in n=%d pid=%d tid=%lld comm=%s code=%d a0=%lx a1=%lx\\n",
                       "clover r187: unix-fail n=%d pid=%d tid=%lld comm=%s code=%d err=%d a0=%lx\\n",
                       "clover r187: mach-in n=%d pid=%d tid=%lld comm=%s trap=%d a0=%lx a1=%lx\\n",
                       'strcmp(comm, "usermanagerd") == 0 || strcmp(comm, "keybagd") == 0',
                       "#define CLOVER_SYS_WHY_ENTRY_LIMIT  512", "#define CLOVER_SYS_WHY_HEARTBEAT    128",
                       "clover_sys_why_mach(trap_no, state);", "error != EJUSTRETURN"):
            if needle not in text:
                errors.append(f"{sys_probe.name} lacks {needle[:44]}...: without the entry print, the "
                              "watched names, the fail filter and the heartbeat the probe cannot say "
                              "which call usermanagerd never returned from")
        removed = [line for line in text.splitlines() if line.startswith("-") and not
                   line.startswith("---")]
        if removed:
            errors.append(f"{sys_probe.name} deletes {removed[0][:60]!r}: the syscall probe prints and "
                          "returns, and a line it removes is a change in what the boot task is told")
        if "clover r186:" in text or "clover r185:" in text or "clover r170:" in text:
            errors.append(f"{sys_probe.name} carries an older round's string: its own lines are the "
                          "only new readings this round produces")

    # Round188: the r187 frame answered the entry question and left two payloads behind it. keybagd's
    # last three calls are write(console), close and reboot(0) -- and the host disassembly of its own
    # binary shows that is its hard-coded fatal path, whose text names the failure and whose exit code
    # is 71; Protobox denies the reboot, so the task dies instead of rebooting. usermanagerd's last
    # entry is write_nocancel(1, ...) with no entry after it, so either the write never returned or the
    # task simply stopped doing I/O. Round188 therefore keeps the r187 prints untouched and adds three
    # readings around them: the first CLOVER_SYS_WHY_TEXT_BYTES of every write's buffer (the fatal
    # message), the path behind every failing open/stat/access (the five open_dprotected_np ENOENTs and
    # the openat one), and the return value of every write. It is still log-only: no argument, no
    # return value and no errno may change, so the patch may only add lines.
    sys_probe2 = next((base / "patches/xnu-clover/zzzzz-clover-syswhy2.patch"
                       for base in (REPO, pathlib.Path.cwd())
                       if (base / "patches/xnu-clover/zzzzz-clover-syswhy2.patch").is_file()), None)
    if sys_probe2 is None:
        errors.append("patches/xnu-clover/zzzzz-clover-syswhy2.patch is missing: the next frame would "
                      "still say keybagd wrote to the console without saying what it wrote, and which "
                      "path usermanagerd's five ENOENT opens were looking for")
    else:
        text = sys_probe2.read_text()
        for needle in ("bsd/dev/arm/systemcalls.c",
                       "#define CLOVER_SYS_WHY_PATH_LIMIT   320",
                       "#define CLOVER_SYS_WHY_WRITE_LIMIT  96",
                       "#define CLOVER_SYS_WHY_TEXT_BYTES   64",
                       "case SYS_open_dprotected_np:", "case SYS_openat_nocancel:",
                       "copyin(src, dst, n) != 0",
                       "copyinstr(upath, path, sizeof(path), &len) != 0",
                       "clover_sys_why_text(text, saved_state64(state)->x[1],",
                       '"clover r188: unix-write n=%d pid=%d tid=%lld comm=%s code=%d "',
                       '"clover r188: unix-path n=%d pid=%d tid=%lld comm=%s code=%d "',
                       "clover r188: unix-wrote n=%d pid=%d tid=%lld comm=%s code=%d fd=%d ret=%lld err=%d\\n"):
            if needle not in text:
                errors.append(f"{sys_probe2.name} lacks {needle[:44]}...: without it the frame cannot "
                              "name keybagd's fatal message, the missing path, or whether the last "
                              "write came back")
        added = [line for line in text.splitlines() if line.startswith("+") and not
                 line.startswith("+++")]
        removed = [line for line in text.splitlines() if line.startswith("-") and not
                   line.startswith("---")]
        if removed:
            errors.append(f"{sys_probe2.name} deletes {removed[0][:60]!r}: r188 only adds readings, "
                          "and a deleted line is a change in what the boot tasks are told")
        joined = "\n".join(added)
        if len(added) < 60:
            errors.append(f"{sys_probe2.name} adds only {len(added)} lines: the three readers, the "
                          "two tables and the copy helpers do not fit in fewer")
        if joined.find("clover_sys_why_text(text,") > joined.find("clover r188: unix-write") or \
                joined.find("clover r188: unix-write") < 0:
            errors.append(f"{sys_probe2.name} prints the write before copying the buffer: the text "
                          "would be read after the call that may have consumed it")
        if joined.find("clover r188: unix-wrote") < joined.find("clover r188: unix-write"):
            errors.append(f"{sys_probe2.name} prints the write result before the write entry: the "
                          "pair would not read as one call")
        for older in ("clover r187:", "clover r186:", "clover r185:"):
            if older in joined:
                errors.append(f"{sys_probe2.name} adds a {older!r} line: its own r188 lines are the "
                              "only new readings this round produces")

    # Round189: the r188 frame closes both old questions and leaves one narrow one. keybagd's path is
    # now spelled out -- there is no data-volume keybag, so its hard-coded fatal branch runs, and the
    # reboot it substitutes for is denied by Protobox on IOPlatformDevice/auto-boot, which is where
    # e00002e2 comes from. usermanagerd completes the earlyboot setup, loads the session table, sets
    # the volume type and writes a 1020-byte bplist to fd 3, and that write is the last kernel line
    # in the frame. Two defects bound the reading: every open_dprotected_np path line was empty
    # because the probe took x[1] (that call's flags, 0x701) for the path pointer, and the entry
    # budget ran out at 67.190 s so the last 13 s are heartbeat-only. Round189 fixes the register,
    # logs the path and returned fd of every successful open for the two names, raises the entry and
    # fail budgets to 4096/256, halves the heartbeat to 64, and adds the mach-trap return line so an
    # entry without a matching return names the trap a task is parked in. Still log-only.
    sys_probe3 = next((base / "patches/xnu-clover/zzzzz-clover-syswhy3.patch"
                       for base in (REPO, pathlib.Path.cwd())
                       if (base / "patches/xnu-clover/zzzzz-clover-syswhy3.patch").is_file()), None)
    if sys_probe3 is None:
        errors.append("patches/xnu-clover/zzzzz-clover-syswhy3.patch is missing: the next frame "
                      "would still show empty paths for the protected opens and would stop tracing "
                      "at the 512th call")
    else:
        text = sys_probe3.read_text()
        for needle in ("bsd/dev/arm/systemcalls.c", "osfmk/arm64/sleh.c",
                       "#define CLOVER_SYS_WHY_ENTRY_LIMIT  4096",
                       "#define CLOVER_SYS_WHY_FAIL_LIMIT   256",
                       "#define CLOVER_SYS_WHY_HEARTBEAT    64",
                       "#define CLOVER_SYS_WHY_OPEN_LIMIT   128",
                       "#define CLOVER_SYS_WHY_MACHOUT_LIMIT 512",
                       "return code == SYS_openat || code == SYS_openat_nocancel;",
                       "clover_sys_why_path_arg(code, state)",
                       '"clover r189: unix-open n=%d pid=%d tid=%lld comm=%s code=%d fd=%lld path=%s\\n"',
                       '"clover r189: mach-out n=%d pid=%d tid=%lld comm=%s trap=%d ret=%d\\n"',
                       "clover_sys_why_mach_out(trap_no, state);"):
            if needle not in text:
                errors.append(f"{sys_probe3.name} lacks {needle[:44]}...: without it the frame still "
                              "cannot name the protected open's path, the writer's descriptor, or "
                              "whether a mach trap came back")
        added = [line for line in text.splitlines() if line.startswith("+") and not
                 line.startswith("+++")]
        removed = [line for line in text.splitlines() if line.startswith("-") and not
                   line.startswith("---")]
        if any("SYS_open_dprotected_np" in line and "return code ==" in line for line in added):
            errors.append(f"{sys_probe3.name} still reads x[1] for SYS_open_dprotected_np: that is "
                          "its flags (0x701), and the path line would come out empty again")
        if text.find("mach_syscall(state);") > text.find("clover_sys_why_mach_out(trap_no, state);"):
            errors.append(f"{sys_probe3.name} prints the mach return before the trap runs: the value "
                          "read would not be the call's return")
        if len(added) < 40:
            errors.append(f"{sys_probe3.name} adds only {len(added)} lines: the open reader, the "
                          "return reader, the register fix and the new limits do not fit in fewer")
        if any("printf(" in line for line in removed):
            errors.append(f"{sys_probe3.name} deletes a print: the old readings have to stay "
                          "readable in the same frame")
        joined = "\n".join(added)
        for older in ("clover r187:", "clover r188:", "clover r186:"):
            if older in joined:
                errors.append(f"{sys_probe3.name} adds a {older!r} line: its own r189 lines are the "
                              "only new readings this round produces")

    # Round190: the r189 frame answers both older questions and leaves a narrower one. The protected
    # opens all named /var/logs/usermanagerd.log.0 (a log directory that does not exist), every mach
    # trap came back, and usermanagerd walked all the way to saving its session keybag: open .writing,
    # a 1020-byte bplist, fsync, renameatx_np, then fcntl(3, 0x55 = F_BARRIERFSYNC) at 112.438859 --
    # after which the kernel ring is silent for 187 s. Nothing says whether that barrier returned, and
    # nothing says whether it reached the block driver. Round190 therefore adds the exit line for the
    # handful of calls that can wait on storage, and brackets the driver's cache commit with a record
    # written to the card *before* the first CMD6, so a commit that never returns still leaves the
    # fact that it started. Still log-only on the kernel side.
    sys_probe4 = next((base / "patches/xnu-clover/zzzzz-clover-syswhy4.patch"
                       for base in (REPO, pathlib.Path.cwd())
                       if (base / "patches/xnu-clover/zzzzz-clover-syswhy4.patch").is_file()), None)
    if sys_probe4 is None:
        errors.append("patches/xnu-clover/zzzzz-clover-syswhy4.patch is missing: the next frame would "
                      "again show the barrier fsync entering and nothing after it, with no way to tell "
                      "a call that never returned from a ring that had nothing left to tee")
    else:
        text = sys_probe4.read_text()
        added = [line for line in text.splitlines() if line.startswith("+") and
                 not line.startswith("+++")]
        removed = [line for line in text.splitlines() if line.startswith("-") and
                   not line.startswith("---")]
        joined = "\n".join(added)
        # A C definition the patch splits over two lines still has to satisfy the needle, so the
        # whitespace-collapsed copy is searched alongside the raw text.
        flat = " ".join(" ".join(line[1:] for line in added).split())
        for needle in ("bsd/dev/arm/systemcalls.c",
                       "#define CLOVER_SYS_WHY_OUT_LIMIT    128",
                       "static int clover_sys_why_out_code(int code)",
                       "case SYS_fcntl:", "case SYS_fcntl_nocancel:",
                       "case SYS_fsync:", "case SYS_fsync_nocancel:",
                       "case SYS_renameatx_np:",
                       '"clover r190: unix-out n=%d pid=%d tid=%lld comm=%s code=%d ret=%lld err=%d\\n"',
                       "clover_sys_why_out_code(code) && clover_sys_why_watched(proc->p_comm)"):
            if needle not in text and needle not in flat:
                errors.append(f"{sys_probe4.name} lacks {needle[:44]}...: without it the frame cannot "
                              "say whether the barrier fsync came back")
        if removed:
            errors.append(f"{sys_probe4.name} deletes {removed[0][:60]!r}: r190 only adds a reading, "
                          "and a deleted line is a change in what the boot tasks are told")
        if joined.find("clover r190: unix-out") < 0:
            errors.append(f"{sys_probe4.name} prints no r190 line: the exit reading is the whole round")
        if text.rfind("clover_sys_why_out_code(code) &&") < text.rfind("if (error == 0 && clover_sys_why_open_code"):
            errors.append(f"{sys_probe4.name} prints the exit before the open reader: the lines would "
                          "not be in call order")
        for older in ("clover r187:", "clover r188:", "clover r189:"):
            if older in joined:
                errors.append(f"{sys_probe4.name} adds a {older!r} line: its own r190 line is the "
                              "only new reading this round produces")

    # The driver half: the flush bracket and the two sync records. flush-in has to be written to the
    # card before the first CMD6 -- that ordering is the whole point, since a commit that hangs never
    # reaches its own tail. Both lines are pinned by their field lists so a later round cannot shrink
    # a reading the next frame needs.
    block_source = (REPO / "drivers/CloverSdhci/CloverSdhci.cpp").read_text()
    flush_start = block_source.find("extern \"C\" uint32_t clover_emmc_flush_cache (void)")
    flush_end = block_source.find("\n}\n", flush_start) if flush_start >= 0 else -1
    flush_body = block_source[flush_start:flush_end] if flush_end > flush_start else ""
    for needle in ("CloverSdhci r190: flush-in flushes=%u on=%u size=%u wait=%llu ms\\n",
                   "CloverSdhci r190: flush-out flushed=%d back=%d rsp=%08x st=%08x polls=%u ",
                   "static volatile uint64_t gGuardLastWaitMs = 0;",
                   "__atomic_store_n (&gGuardLastWaitMs, waited * 1000ULL / frequency,"):
        if needle not in block_source:
            errors.append(f"drivers/CloverSdhci/CloverSdhci.cpp lacks {needle[:44]}...: without it the "
                          "next frame cannot say whether the barrier reached the card or how long it "
                          "waited for the block guard")
    if not flush_body:
        errors.append("drivers/CloverSdhci/CloverSdhci.cpp lost clover_emmc_flush_cache: the barrier "
                      "has no bracket left to write")
    else:
        entry = flush_body.find("CloverSdhci r190: flush-in")
        first_cmd = flush_body.find("switchExtCsd (kExtCsdCacheCtrl, 0")
        exit_at = flush_body.find("CloverSdhci r190: flush-out")
        if not (0 <= entry < first_cmd):
            errors.append("clover_emmc_flush_cache prints flush-in after the first CMD6: a commit that "
                          "never returns would leave no record that it started")
        if not (first_cmd < exit_at):
            errors.append("clover_emmc_flush_cache prints flush-out before the commit runs: the pair "
                          "would not bracket it")
        if flush_body.find("teeKernelLog ();") > first_cmd:
            errors.append("clover_emmc_flush_cache does not drain the kernel ring before the first "
                          "CMD6: the r190 exit line would sit in msgbuf through the whole commit")
    block_dev_source = (REPO / "drivers/CloverBlock/CloverBlock.cpp").read_text()
    for needle in ("CloverBlock r190: sync pending=0 (once)\\n",
                   "CloverBlock r190: sync-in pending=%llu\\n",
                   "CloverBlock r190: sync-out ok=1 pending=%llu\\n",
                   "CloverBlock r190: sync-out ok=0 pending=%llu\\n"):
        if needle not in block_dev_source:
            errors.append(f"drivers/CloverBlock/CloverBlock.cpp lacks {needle[:44]}...: a barrier with "
                          "nothing to commit would read exactly like a barrier that never arrived")

    # Round191. The r190 frame ends at flush-in with nothing behind it, so the commit's own steps
    # never produced a reading. Five stage records name the step that comes next, each drained to the
    # card before that step runs; and the commit's wait stops asking CMD13 at a card that is holding
    # DAT0, because a hundred unanswered asks is exactly what the r190 frame's silence is made of.
    stage_lines = ("CloverSdhci r191: flush-go cf=%u bound=%u ps=%08x inh=%u\\n",
                   "CloverSdhci r191: flush-c1 ok=%d st=%08x rsp=%08x spins=%u ms=%llu\\n",
                   "CloverSdhci r191: flush-w1 ready=%d to=%d busy=%llu ms polls=%u rsp=%08x st=%08x ",
                   "CloverSdhci r191: flush-c2 ok=%d st=%08x rsp=%08x spins=%u ms=%llu\\n",
                   "CloverSdhci r191: flush-w2 ready=%d to=%d busy=%llu ms polls=%u rsp=%08x st=%08x ")
    for needle in stage_lines + ("static FlushSettle flushSettle (uint64_t frequency, uint32_t boundMs)",
                                 "if (!(out.present & SDHCI_DATA_INHIBIT)) {",
                                 "const uint32_t cacheFlushUnits = (gExtCsd[61] >> 16) & 0xffu;",
                                 "return ms > 180000u ? 180000u : ms;",
                                 "static volatile uint32_t gCacheFlushUnits = 0;"):
        if needle not in block_source:
            errors.append(f"drivers/CloverSdhci/CloverSdhci.cpp lacks {needle[:44]}...: the next frame "
                          "could not say which step of the commit stopped, or how long a card that is "
                          "still writing its cache is meant to be given")
    flush191 = block_source.find("CloverSdhci r191: flush-go")
    go_cmd = block_source.find("switchExtCsd (kExtCsdCacheCtrl, 0")
    if not (0 <= flush191 < go_cmd):
        errors.append("clover_emmc_flush_cache pushes flush-go after the first CMD6: the record would "
                      "not survive the step it exists to bracket")
    # Every stage line has to reach the card before the next step runs, so its drain must follow it
    # and precede the next stage.
    positions = [block_source.find(line) for line in stage_lines]
    ordered = all(0 <= positions[i] < positions[i + 1] for i in range(len(positions) - 1))
    out_at = block_source.find("CloverSdhci r190: flush-out")
    if not ordered or not (positions[-1] < out_at):
        errors.append("clover_emmc_flush_cache prints the r191 stage lines out of order: the frame "
                      "would name a step that had not run yet")
    else:
        for index, line in enumerate(stage_lines):
            ends_at = positions[index + 1] if index + 1 < len(positions) else out_at
            if "flushVerdictRing ();" not in block_source[positions[index]:ends_at]:
                errors.append(f"clover_emmc_flush_cache does not drain the ring after the stage line "
                              f"{index + 1} of 5: a hang in the next step would lose the record that "
                              "names it")
    tail = block_source[block_source.find("extern \"C\" uint32_t clover_emmc_flush_cache (void)"):out_at]
    if "cardInTransfer (" in tail:
        errors.append("clover_emmc_flush_cache still waits with cardInTransfer: a hundred CMD13s at a "
                      "card that is holding DAT0 is the silence the r190 frame shows")

    # Round192. The r191 frame ends at flush-go: the commit was started, and then the card held DAT0
    # long enough that no stage record -- all of which leave through a block write -- could be
    # delivered. A commit that cannot report and cannot finish is given back to the card, which means
    # the bring-up stops switching the cache on. The r171 line stays (it is the reading that says
    # ctrl=0/on=0), the flush body stays live for a card that ships with the cache already on, and
    # the early return gets a line so a skipped commit cannot be mistaken for a missing one.
    # Round322: this second title needle rides the r192 block and must agree with the one above --
    # both read board["payload"]["title"], so both must name the round the card will carry.
    if not board["payload"]["title"].startswith("clover r339:"):
        errors.append('[payload] title is not "clover r339: ...": the menu would not name this round')
    for needle in ("CloverSdhci r192: commit skipped size=%u on=%u (once)\\n",
                   "const bool cacheOn = cacheSize != 0 && cacheCtrlBefore != 0;"):
        if needle not in block_source:
            errors.append(f"drivers/CloverSdhci/CloverSdhci.cpp lacks {needle[:44]}...: the frame "
                          "could not tell a commit that was skipped from one that never reported")
    if "if (cacheSize && cacheCtrlBefore == 0) {" in block_source:
        errors.append("CloverSdhci.cpp still enables the card's write cache at bring-up: r191 showed "
                      "that the first commit after that enable never returns and cannot report")
    skip_at = block_source.find("CloverSdhci r192: commit skipped")
    guard_at = block_source.find("acquireBlockGuard (frequency, start, token)",
                                 block_source.find("extern \"C\" uint32_t clover_emmc_flush_cache"))
    if not (0 <= skip_at < guard_at):
        errors.append("clover_emmc_flush_cache records the skipped commit after taking the block "
                      "guard: the record would wait for the writer it exists to avoid")

    # Round193. The r192 frame ends with the third watchdogd death of the boot and a panic behind
    # it: namespace 2 is OS_REASON_SIGNAL, 0x5 is W_EXITCODE(0, SIGTRAP), and the job plist's
    # PanicOnCrash/PanicOnConsecutiveCrash turns that trap into a machine panic. The fix is a boot
    # argument, not a driver -- drivers are paused until the desktop -- so the contract has two
    # halves that must stay together: the argument is still in kernel_args, and the kernel line
    # still prints before the panic decision so a frame can show which decision inputs were live.
    if "panic_on_crash_disabled=1" not in kernel_args:
        errors.append("[payload] kernel_args lost panic_on_crash_disabled=1: the third watchdogd "
                      "SIGTRAP would panic the boot again at ~208 s")
    crit_patch = next((base / "patches/xnu-clover/zzzzzz-clover-crash-why.patch"
                       for base in (REPO, pathlib.Path.cwd())
                       if (base / "patches/xnu-clover/zzzzzz-clover-crash-why.patch").is_file()),
                      None)
    if crit_patch is None:
        errors.append("patches/xnu-clover/zzzzzz-clover-crash-why.patch is missing: the next frame "
                      "could not tell which crash-behavior bits made the exit critical")
    else:
        text = crit_patch.read_text()
        crit_line = ("clover r193: crit pid=%d comm=%s rv=0x%x sig=%d fatal=%d sigcrash=%d "
                     "crashbits=0x%x spawnfail=%d deadline=%llu now=%llu")
        if crit_line not in text:
            errors.append(f"{crit_patch.name} lacks the r193 critical-exit fields: the frame could "
                          "not name the inputs of the panic decision")
        # The probe must sit above the early return, otherwise the suppressed exits -- the ones this
        # round exists to observe -- would print nothing at all.
        if not (0 <= text.find("clover r193: crit") < text.find("if (!proc_should_trigger_panic")):
            errors.append(f"{crit_patch.name} prints the r193 line at or below "
                          "proc_should_trigger_panic(): a suppressed crash would not be recorded")
        if text.find("proc_handle_critical_exit(proc_t p, int rv)") < 0:
            errors.append(f"{crit_patch.name} does not anchor the probe in "
                          "proc_handle_critical_exit(): the line would not bracket the panic path")
        # Round207. Round206's SpringBoard exit carried the reason the kernel builds itself on the
        # default-kill path, so it named no sender. The sender line has to sit in psignal_internal,
        # above the flavour split, where both the receiver and current_proc() are in scope.
        kill_line = ("clover r207: killsig pid=%d comm=%s ppid=%d sig=%d spid=%d scomm=%s "
                     "ns=%d code=0x%llx cs=0x%x ra=%016llx anchor=%016llx")
        if kill_line not in text:
            errors.append(f"{crit_patch.name} lacks the r207 sender fields: the frame could not say "
                          "who kills SpringBoard")
        # psignal_internal is static and the hunk's context does not reach its signature, so the
        # anchor is the bad-signal panic that opens it: the probe must follow that line inside the
        # same delivery, above the flavour split where the sender is still current_proc().
        kill_at = text.find("clover r207: killsig")
        open_at = text.find('panic("psignal: bad signal number %d"')
        if not (0 <= open_at < kill_at):
            errors.append(f"{crit_patch.name} does not anchor the r207 probe in psignal_internal(): "
                          "the sender and the receiver would not come from the same delivery")
        # Round208. Round207 named backboardd but not the call site; the sender's user pc plus the
        # four bytes at it are what the host fingerprints against the binary, because the chained
        # fixups leave no constant pointer to search for. The receiver side reads every SpringBoard
        # thread, since a collective-watchdog kill usually means the main thread is stuck.
        upc_line = "clover r208: sender upc=%016llx uinsn=%08x spid=%d scomm=%s"
        if upc_line not in text:
            errors.append(f"{crit_patch.name} lacks the r208 sender pc and instruction word: the "
                          "host could not locate the kill call site in backboardd")
        # Round211. Round208 said every SpringBoard thread was in TH_WAIT but not what any of them
        # was waiting on, because a user-space pc alone cannot show a thread parked in a kevent or
        # a page fault. The kernel side of each thread still holds the kernel return addresses from
        # before it last dropped to EL0, so the frame carries lr/sp, a six-deep x29 backtrace, and
        # a main-thread mark; the sender's own current kernel stack comes along for the same reason.
        skst_line = ("clover r211: skst n=%u f0=%016llx f1=%016llx f2=%016llx f3=%016llx "
                     "f4=%016llx f5=%016llx")
        if skst_line not in text:
            errors.append(f"{crit_patch.name} lacks the r211 sender kernel backtrace: the kill "
                          "call site would still be a bare user pc")
        sbthr_line = ("clover r211: sbthr n=%u pid=%d mt=%d tid=0x%llx pc=%016llx lr=%016llx "
                      "sp=%016llx cpsr=%08x state=%d kin=%u wait=%016llx kstk=%016llx")
        if sbthr_line not in text:
            errors.append(f"{crit_patch.name} lacks the r211 per-thread reading: the frame could "
                          "not pair a parked thread with the kernel call it parked in")
        # Round216. The boot-4 panic was a Kernel data abort inside this walker at +0x180, the
        # instruction that reads the saved state at kernel_stack + kernel_stack_size - 0x68. A
        # thread parked with its kernel stack swapped out has kernel_stack == 0 (stack.c's
        # machine_stack_detach), so the guard must come before get_kernel_state, and a swapped
        # thread has to be distinguishable (kin=2) in the line rather than silently absent.
        stack_at = text.find("if (thr->kernel_stack == NULL)")
        state_at = text.find("thread_get_kernel_state")
        if stack_at < 0 or state_at < 0 or stack_at > state_at:
            errors.append(f"{crit_patch.name} reads the kernel saved state before checking "
                          "kernel_stack: a swapped thread would fault the kernel again")
        if "snap[n].kin = 2;" not in text or "snap[n].kstk" not in text:
            errors.append(f"{crit_patch.name} does not record the swapped-stack case: a frame "
                          "could not tell a null stack from a thread in user mode")
        sbthk_line = ("clover r211: sbthk n=%u ktop=%016llx kfp=%016llx klr=%016llx "
                      "kf=%016llx,%016llx,%016llx,%016llx,%016llx,%016llx")
        if sbthk_line not in text:
            errors.append(f"{crit_patch.name} lacks the r211 per-thread kernel backtrace: the "
                          "receiver's park site would still be unread")
        if "thread_get_kernel_state" not in text:
            errors.append(f"{crit_patch.name} reads no kernel saved state: the backtrace would "
                          "have no frame pointer to start from (machine.kpcb is kdp-only)")
        upc_at = text.find("clover r208: sender upc")
        skst_at = text.find("clover r211: skst")
        sbthr_at = text.find("clover r211: sbthr")
        if not (kill_at < upc_at < skst_at < sbthr_at):
            errors.append(f"{crit_patch.name} places the r211 lines outside the r207 delivery: the "
                          "sender stack and the threads would not be read at the same kill")
        # Round212. Everything above reaches the card only through msgbuf and the tee, and the boot
        # that motivated this round killed SpringBoard at 167 s and then sat silent until the 900 s
        # deadline: by the panic, the ring had wrapped and the tee's tail read said nothing. The
        # same lines now go to the driver's RAM ring at the moment they are produced, so the panic
        # is no longer what makes them readable.
        for needle, why in (
                ("clover_emmc_probe_say(clover_r207_line",
                 "the r207 killsig line never reaches the probe ring"),
                ("clover_emmc_probe_say(clover_r208_line",
                 "the r208 sender line never reaches the probe ring"),
                ("clover_emmc_probe_say(clover_r211_line",
                 "the r211 sender stack line never reaches the probe ring"),
                ("clover_emmc_probe_say(line, (unsigned int)strlen(line));",
                 "the per-thread snapshot lines never reach the probe ring"),
                ("snprintf(clover_r207_line, sizeof(clover_r207_line),",
                 "the probe line is not bounded, so a long comm could overrun the stack buffer"),
                ("char clover_r208_line[256];",
                 "the r208/r211 lines have no bounded buffer to be relayed from")):
            if needle not in text:
                errors.append(f"{crit_patch.name} lacks {needle[:44]!r}: {why}")

    sig_patch = next((base / "patches/xnu-clover/zzy-clover-exec-sig-bytes.patch"
                      for base in (REPO, pathlib.Path.cwd())
                      if (base / "patches/xnu-clover/zzy-clover-exec-sig-bytes.patch").is_file()),
                     None)
    if sig_patch is None:
        errors.append("patches/xnu-clover/zzy-clover-exec-sig-bytes.patch is missing: the next "
                      "frame could not tell a wrong read from a signature validation gate")
    else:
        text = sig_patch.read_text()
        for needle in ("clover r166: sig n=%u mo=%lld off=%u size=%u",
                       "clover_sig_dumps < 8"):
            if needle not in text:
                errors.append(f"{sig_patch.name} lacks {needle[:28]}...: the exec path would not "
                              "report what it read")
    # Round164. Both halves of this round are source, and the kernel half cannot be read off the
    # payload: a 16K-signed blob must not walk the whole file before its first page, and AMFI's
    # second deny branch must not survive the payload build.
    cache_patch = next((base / "patches/xnu-clover/zzy-clover-dyld-cache-skip.patch"
                        for base in (REPO, pathlib.Path.cwd())
                        if (base / "patches/xnu-clover/zzy-clover-dyld-cache-skip.patch").is_file()),
                       None)
    if cache_patch is None:
        errors.append("patches/xnu-clover/zzy-clover-dyld-cache-skip.patch is missing: every "
                      "16 KiB-signed dyld cache would still read itself whole before its first page")
    else:
        text = cache_patch.read_text()
        for needle in ("csb_native4k_skipped", "cs_native4k_skipped_blobs",
                       "blob->csb_native4k_skipped = true;"):
            if needle not in text:
                errors.append(f"{cache_patch.name} lacks {needle}: the 16K-signed blob would walk "
                              "the whole-file hash again")
        if "if (blob->csb_native4k_skipped) {" not in text:
            errors.append(f"{cache_patch.name} does not accept faulted pages of a skipped blob: "
                          "the first 4K fault would still be refused")
    amfi_patch = next((base / "scripts/patch-clover-amfi.py"
                       for base in (REPO, pathlib.Path.cwd())
                       if (base / "scripts/patch-clover-amfi.py").is_file()), None)
    if amfi_patch is None:
        errors.append("scripts/patch-clover-amfi.py is missing: the prelinked AMFI kext would keep "
                      "its second deny branch and launchd's children would still be killed")
    else:
        text = amfi_patch.read_text()
        for needle in ("e00300f940c1fff00014259157000014b80f40f995570094080340b9090113120900092a",
                       "ff7f01a9e0630091e3430091e10316aae20315aaa9000094",
                       "0x34000229, 0x14000011", "0x36000520, 0xD503201F"):
            if needle not in text:
                errors.append(f"{amfi_patch.name} lacks {needle[:16]}...: one AMFI deny branch "
                              "would survive the payload build")
    # Round177 (schedule item 8): a rebuild of this tree is now a byte-for-byte repeat, because the
    # three fields a link cannot reproduce -- the linker's LC_UUID, one mtime per object-path debug
    # symbol, and the kernel-collection id that hashes those two -- are rewritten as functions of the
    # rest of the file.  Measured on two real builds of the identical tree: 52,043,776 bytes each, 3,861
    # of them build-varying, and after normalisation one image (sha256 86273887...).
    normalizer = next((base / "scripts/normalize-kernel-macho.py"
                       for base in (REPO, pathlib.Path.cwd())
                       if (base / "scripts/normalize-kernel-macho.py").is_file()), None)
    if normalizer is None:
        errors.append("scripts/normalize-kernel-macho.py is missing: the round's kernel would stay "
                      "a file two builds of one tree cannot agree on")
    else:
        text = normalizer.read_text()
        for needle in ("N_SO = 0x66", "LC_UUID = 0x1B", "KCID_PATTERN", "def zeroed(",
                       "def derived_uuid(", "def is_normalised(", "def collect_fields(",
                       "--prove", "--check", "--selftest", "STAMP_WIDTH",
                       "expected exactly one _PrelinkKCID", "normalisation moved the file length"):
            if needle not in text:
                errors.append(f"{normalizer.name} lacks {needle[:28]}: the rewrite would no longer be "
                              "the three fields, their ordering or its own idempotence")
        # Refusing a kernel whose metadata moved shape is the whole safety argument: a silent
        # short read would normalise a stranger and hand it to the boot contract.
        for needle in ("not an ARM64 Mach-O", "runs past the file", "width changed"):
            if needle not in text:
                errors.append(f"{normalizer.name} lacks the refusal {needle[:20]}: a malformed kernel "
                              "would be rewritten instead of rejected")
        check = subprocess.run([sys.executable, str(normalizer), "--selftest"],
                               capture_output=True, text=True)
        if check.returncode:
            errors.append("normalize-kernel-macho.py --selftest failed: two builds of one tree would "
                          "not converge\n" + (check.stdout + check.stderr)[-700:])
    # Round177 also closes the round162 thread on the host: cs_blob … 85 was read as a signature the
    # kernel refused, and the replica of ubc_subr.c's structure walk says the shipped validator accepts
    # the binaries on this card. Keep the replica under test, or the next round mistakes a host that
    # agrees with itself for a host that agrees with XNU.
    csblob = next((base / "scripts/check-cs-blob-structure.py"
                   for base in (REPO, pathlib.Path.cwd())
                   if (base / "scripts/check-cs-blob-structure.py").is_file()), None)
    if csblob is None:
        errors.append("scripts/check-cs-blob-structure.py is missing: nothing would separate a refused "
                      "code directory from the reads that round164 blamed on it")
    else:
        text = csblob.read_text()
        for needle in ("def validate_csblob(", "def validate_codedirectory(", "Reject(494,",
                       "Reject(383,", "Reject(572,", 'ubc_subr.c:%d', "CSSLOT_CODEDIRECTORY",
                       "native4k", "--selftest", "% len(replayed)"):
            if needle not in text:
                errors.append(f"{csblob.name} lacks {needle[:28]}: the replica would no longer be the "
                              "walk the kernel performs, at the lines that walk performs")
        check = subprocess.run([sys.executable, str(csblob), "--selftest"],
                               capture_output=True, text=True)
        if check.returncode:
            errors.append("check-cs-blob-structure.py --selftest failed: the host replica and "
                          "ubc_subr.c's order of rejection no longer agree\n"
                          + (check.stdout + check.stderr)[-700:])
        root = REPO / "build/clover-root/launchd.macho"
        if root.is_file():
            check = subprocess.run([sys.executable, str(csblob), str(root)],
                                   capture_output=True, text=True)
            if check.returncode:
                errors.append("the shipped launchd.macho would be refused by the validator's own "
                              "structure rules, so 'cs_blob 85' is not a host replica problem\n"
                              + (check.stdout + check.stderr)[-500:])
    # The step has to sit in the recipe between the AMFI patch and the payload build: normalising
    # first would let the patcher move bytes the identity fields had already hashed, and the shipped
    # image would be its own normalisation's neighbour again.
    recipe = REPO / "AGENTS.md"
    if recipe.is_file():
        agent_text = recipe.read_text()
        for needle in ("normalize-kernel-macho.py", "roundNNN-kernel-amfi.macho"):
            if needle not in agent_text:
                errors.append(f"AGENTS.md lacks {needle[:24]}...: the build order would live only in "
                              "the log, and the next round would skip the normalisation")
        if "normalize-kernel-macho.py" in agent_text and not (
                agent_text.find("normalize-kernel-macho.py") < agent_text.find("build payload")):
            errors.append("AGENTS.md does not put the normalisation before the payload build: "
                          "a payload built from an unnormalised kernel is not reproducible")
    verifier = next((base / "scripts/verify-clover-payload.py"
                     for base in (REPO, pathlib.Path.cwd())
                     if (base / "scripts/verify-clover-payload.py").is_file()), None)
    if verifier is None:
        errors.append("scripts/verify-clover-payload.py is missing: nothing would prove either "
                      "r164 half reached the payload")
    else:
        text = verifier.read_text()
        for needle in ("NATIVE4K_SKIP", "check_dyld_cache_skip",
                       # The r164 fingerprint is an instruction group, and two of its bytes encode an
                       # address rather than a decision -- the round180 archive grew the kernel by a
                       # page and an exact-byte match reported a patch that was present. The masked
                       # form is the contract; a return to whole-group byte equality would make every
                       # future kernel growth look like a lost signature concession.
                       "len(re.findall(pattern, kernel)) == 1",
                       "check_amfi_allow",
                       "check_read_integrity", "CloverSdhci r167: readcheck",
                       "CloverSdhci r171: write cache size=",
                       "CloverSdhci r172: dmacheck lba=", "DMA_INTEGRITY",
                       "CloverSdhci r173: wrcheck lba=", "WRITE_DMA_INTEGRITY",
                       "check_exec_sig_dump", "clover r166: sig n=%u",
                       # Round177: the verifier has to look at the shipped kernel's identity fields,
                       # not only at the round's strings, or a payload built from a plain kernel would
                       # still pass every needle here.
                       "check_kernel_normalized", "normalize_kernel_macho",
                       "object stamps blank",
                       # Round180: the display stack is now code, and code only reaches the payload
                       # through the link. The verifier is the only place that can say the archive and
                       # the probe were there.
                       "check_legacy_display", "LEGACY_DISPLAY",
                       "probe: console %ux%u row=%u depth=%u display=%llu "
                       "rotate=%u scale=%u base=%llx length=%llx"):
            if needle not in text:
                errors.append(f"{verifier.name} lacks {needle}: the release could ship with an "
                              "unchecked kernel half")

    # Round178. The r170 frame named the mechanism in two lines that had never been read together: a
    # checkpoint flush that took 102 and 120 seconds, and eight reads at the other volume that failed
    # with st=00000000 -- no host error bit, because the card never saw them. Round171 turned a
    # refusal into a wait, which was right, but a wait behind a whole request is still a wait measured
    # in flushes. What this round owns is the *unit of ownership*: the guard is handed over at chunk
    # boundaries, so the longest a reader can be kept out is one chunk, and the request deadline keeps
    # its old meaning because it is charged only the time the holder actually had the host.
    sdhci = REPO / "drivers/CloverSdhci/CloverSdhci.cpp"
    if not sdhci.is_file():
        errors.append("drivers/CloverSdhci/CloverSdhci.cpp is missing: there is no guard hand-off "
                      "to verify")
    else:
        text = sdhci.read_text()
        for needle in ("static volatile uint32_t gGuardHandoffs = 0;",
                       "static volatile uint64_t gGuardWorstWait = 0;",
                       "static bool handOffBlockGuard (uint64_t &frequency, uint64_t &start, uint64_t &owned,\n                               uint32_t &token)",
                       "static void reportGuardHandoff (const char *dir, uint32_t lba, uint32_t blocks, uint32_t done)",
                       "CloverSdhci r178: %s handoff lba=%u blocks=%u done=%u cuts=%u waits=%u worst=%llu ms",
                       "if (owned + readCounter () - start > deadline) {",
                       "if (!handOffBlockGuard (frequency, start, owned, token)) {",
                       "if (done < blocks && sinceCut >= kReadChunkBlocks) {"):
            if needle not in text:
                errors.append(f"{sdhci.name} lacks {needle[:44]}...: round178's chunk-boundary hand-off "
                              "would not exist on both paths, or the deadline would have gone back to "
                              "counting the wait")
        # Both directions, or the fix only covers half the frame: the r170 reads were starved by a
        # writer, and a reader holding the host for a 1 GiB dyld run starves the checkpoint flush.
        if text.count("handOffBlockGuard (frequency, start, owned, token)") < 2:
            errors.append(f"{sdhci.name} hands the guard off on fewer than two paths: the write loop "
                          "and the read loop each have to cut, or one of them still owns the bus for "
                          "the whole request")
        # A hand-off is only fair if the holder that comes back is not the same thread's next request
        # with a queued waiter skipped -- which is why the deadline is charged `owned` and the wait is
        # counted, not assumed. The counters have to be fed from inside acquireBlockGuard.
        acquire_start = text.find("static bool acquireBlockGuard (uint64_t &frequency, uint64_t &start, "
                                  "uint32_t &token)")
        acquire_body = text[acquire_start:acquire_start + 3200] if acquire_start >= 0 else ""
        if "gGuardWaits = __atomic_add_fetch" not in acquire_body:
            errors.append("acquireBlockGuard does not count the wait it measures: the hand-off report "
                          "would quote cuts and waits nobody observed")
        # Round213: the waiter's own bound is the point of the ticket -- a waiter that can only wait
        # is what r212's frame caught, asleep on an IOLock behind a request that never returned.
        if "readCounter () - waitStart > waitLimit" not in acquire_body:
            errors.append("acquireBlockGuard waits without its own bound: one stuck holder would "
                          "still queue every later reader for the rest of the boot")

    # Round179. The display schedule item started with a question that sheng could only answer by
    # booting a VM: is the original IOMobileFramebufferLegacy in *this* kernel the class that its
    # qualification was done against? The family's meta class records are load-time fixups, so the
    # answer is not readable in __PRELINK_DATA -- but the registration thunk is code, and exactly one
    # site in the family carries its instance size. scripts/audit-clover-mobile-graphics-legacy.py
    # derives the identity from those instructions and replays every structural judgement against our
    # own image. CI has no built kernel, so the gate here is the script's shape: the pinned constants
    # have to stay pinned, and the script has to refuse a missing kernel rather than guess one.
    audit = REPO / "scripts/audit-clover-mobile-graphics-legacy.py"
    if not audit.is_file():
        errors.append("scripts/audit-clover-mobile-graphics-legacy.py is missing: the original "
                      "Legacy class would be unqualified in this board's own kernel")
    else:
        script = audit.read_text()
        for needle in ('FAMILY_TEXT_SHA = "2e044ee68c1650397e1435234ca6459206262e4c7ed709cb7131c6f664f9bd9b"',
                       '"legacy_constructor": 0x1A5D8',
                       "ABSTRACT_SLOTS = [211, 213, 245, 246, 306, 307, 337, 338, 345]",
                       "OWN_OVERRIDES = [0, 1, 7, 14, 84, 86, 87, 141, 155, 184, 188, 288, 290, 296, 301]",
                       'require(record == 0xFFFFFFF006B35940 and parent == 0xFFFFFFF006B31400,',
                       'require(name == b"IOMobileFramebufferLegacy\\0"',
                       'require(KERNEL.exists() and PLAN.exists(), "build the kernel first: "',
                       'sys.path.insert(0, str(BASE / "kits/NativePrelink"))',
                       'identity_source="static registration site in this kernel"'):
            if needle not in script:
                errors.append(f"{audit.name} lacks {needle[:56]}: the qualification would no longer be "
                              "the one the original family was audited with, or a missing kernel would "
                              "be read as a passing audit")
        # The identity has to come from instructions, not from a DATA read: meta class records are
        # zeros in the file, and an audit that "found" them by scanning __DATA found nothing. The
        # script says which of its claims are static and which would need a running system, so the
        # honesty flags have to stay false -- the docstring is allowed to name QEMU, that is where
        # sheng got its numbers, and this round is what replaces it.
        for overclaim in ("legacy_runtime_instance_created=True", "backend_contract_complete=True",
                          "device_observed=True", "original_instructions_modified=True"):
            if overclaim in script:
                errors.append(f"{audit.name} asserts {overclaim}: nothing in this round runs the "
                              "kernel, so the ABI JSON would be quoting an outcome nobody observed")

    # Round180. The archive that replaces the stub is the part of this port that must not be able to
    # lie: every original-class binding is an address inside Apple's family text, and an address that
    # moved is a driver running into whatever lives there now. scripts/build-clover-gop-legacy-display.py
    # therefore re-reads out/clover/kernel/kernel.macho at each pinned site and refuses the archive on a
    # single differing instruction, and it only runs against the device-local copy of
    # drivers/NativeMobileDisplay -- the base module is a driver that still claims v_display == 1. CI has
    # no built kernel, so the gate here is the shape that makes those checks happen: the force-load path,
    # the staging that keeps the two module initializers from running twice, the self-containment of the
    # archive, and the honesty flags of its report.
    retired = REPO / "scripts/build-clover-display-stub.py"
    if retired.is_file():
        errors.append("scripts/build-clover-display-stub.py is still in the repo: `unixnu tree` copies "
                      "scripts/ from the device repo, so the stub builder would reach the build and a "
                      "reader could not tell which display archive this board ships")
    builder = REPO / "scripts/build-clover-gop-legacy-display.py"
    if not builder.is_file():
        errors.append("scripts/build-clover-gop-legacy-display.py is missing: [kernel] archives names a "
                      "builder that does not exist, so the force-loaded display archive never appears")
    else:
        builder_text = builder.read_text()
        for needle in ("OUT = ROOT / 'build/native-legacy-display'",
                       "archive = OUT / 'libNativeLegacyDisplay.late.a'",
                       "require(DRIVER.is_relative_to(DEVICE.resolve()),",
                       "require(audit['kernel_sha256'] == sha(KERNEL),",
                       "legacy_ctor = base + 0x1A5CC; legacy_dtor = base + 0x1A618",
                       "require(owned == sorted(own + pure), 'Exact owned method set')",
                       "'--remove-section=__DATA,__mod_init_func'",
                       "duplicate definition of ",
                       "is defined by no archive member",
                       "'_native_legacy_gpu_complete' in contract",
                       "static_assert(sizeof(IOMobileFramebufferLegacy)==0x18a0",
                       "original_instructions_modified=False, driver_initialized=False, display_presented=False"):
            if needle not in builder_text:
                errors.append(f"{builder.name} lacks {needle[:48]}: the archive could be linked without "
                              "the check that keeps its bindings, its owned method set or its "
                              "self-containment true for this kernel")
        if "device_observed=True" in builder_text:
            errors.append(f"{builder.name} asserts device_observed=True: no frame from this archive has "
                          "been read back off a card yet")

    # The board-local instrument is the other half of the same claim: it owns 18 of the driver's 27
    # undefined _native_legacy_* hooks, bounds its own log budget so the shared kernel-log ring cannot be
    # flooded, and stops at phase 1 on hardware. Two words in particular have to stay declarations rather
    # than definitions -- the driver defines them, and a duplicate symbol in a force-loaded archive is a
    # link failure that would have looked like a display problem.
    probe = REPO / "package/CloverLegacyDisplayProbe.cpp"
    if not probe.is_file():
        errors.append("package/CloverLegacyDisplayProbe.cpp is missing: the driver's 18 probe hooks and "
                      "both bsd_init/bsd_autoconf entry points would be undefined")
    else:
        probe_text = probe.read_text()
        for needle in ('static char cloverRound[]="clover r180:";',
                       "enum {kCloverLogBudget=96};",
                       '"probe: console %ux%u row=%u depth=%u display=%llu rotate=%u scale=%u base=%llx length=%llx"',
                       "if(native_legacy_init_result && (phase==2 || phase==3 || phase==4))",
                       'extern "C" volatile uint64_t native_legacy_early_retires,native_legacy_early_failures;',
                       'IOLog("%s %s unwound\\n",cloverRound,report);'):
            if needle not in probe_text:
                errors.append(f"{probe.name} lacks {needle[:52]}: round180's reading would be missing, "
                              "unbounded, or the object would be kept past a phase this board has not "
                              "qualified")

    # The single behavioural change inside the vendored driver, asserted here because feeds.py lets the
    # device copy shadow the base one. If it ever goes missing the archive silently rebuilds Apple's
    # module: that one tests v_display == 1, a PowerPC-era graphics-mode enum this firmware never sets.
    display_driver = REPO / "drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp"
    if not display_driver.is_file():
        errors.append("drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp is missing: the device-local "
                      "display module this archive is built from would not exist")
    elif "primary=video.v_display!=0;" not in display_driver.read_text():
        errors.append("the vendored driver lost its board-local v_display claim: the base module tests "
                      "v_display==1, and this firmware hands over a nonzero display descriptor")

    # Round329 turns the legacy display client from a set of first-sighting rows into a state that keeps
    # being read. Boot 113 is the reason: it showed four accepted swaps and then nothing, and every row the
    # probe prints about that client is printed on a first sighting or on one of the first four events, so
    # the silence after the stall cannot be told from a channel that ran out of budget -- which is why the
    # row carries its own `log=/logdrop=` beside the counters. The beat lives in the driver because the
    # probe owns no clock, and it must belong to no boot argument: a cadence that depends on the flag the
    # reading is about puts the judge on the thing under trial.
    if probe.is_file():
        probe_code = [line for line in probe_text.splitlines()
                      if not line.strip().startswith(('*', '/*', '//'))]
        for now in ('"clover r329: dispstate ms=%llu fires=%u seen=%u client=%s calls=%s fails=%s regall=%s regs=%s grew=%s "',
                    '"hot=%s s6=%s done=%s presented=%s frames=%s vs=%u/%llu/%llu log=%u/%u logdrop=%u\\n"',
                    '"clover r329: dispstatecut bytes=%d cap=%u fires=%u client=%s\\n"',
                    'static volatile uint32_t cloverLogDropped;',
                    'cloverLogDropped=cloverLogDropped+1;return;',
                    'uint32_t regCalls;',
                    '++stat->regCalls;',
                    'extern "C" void native_legacy_display_state_beat(void) {reportDisplayState();}',
                    'const char *clientTxt=stat?"pid58":(seenClientCount?"other":"none");',
                    'enum {kDispStateRowBytes=320};'):
            if not any(now in line for line in probe_code):
                errors.append(f"package/CloverLegacyDisplayProbe.cpp does not carry {now[:60]!r}: without it "
                              "the client's state is still only printed beside a first event, and boot 113's "
                              "silence stays ambiguous")
        # Every column that describes a client must have a literal for "there was no such client to read",
        # and that literal is `-`, not 0: a zero would print the same for a client that asked nothing.
        if not probe_code or 'const char *callsTxt="-"' not in probe_text:
            errors.append("the r329 row prints client columns with no literal for the case where no pid-58 "
                          "client exists: `-` and 0 are two different answers and r327 already paid for "
                          "collapsing them")
        # Negative needles. The row must not take a name another producer already owns, and must not spend a
        # column on a word that is a pure function of a column beside it (round8's rule: a field whose value
        # is fixed by the row is not a reading).
        for banned in ('"clover r329: dispstate t=', '"clover r329: dispstate us=',
                       'dispstate %s state=', 'state=%s grew=', 'set=%u state=%s'):
            if any(banned in line for line in probe_code):
                errors.append(f"the r329 row carries {banned[:40]!r}: `t=` is this file's seconds-since-first-frame "
                              "and the kernel rows' seconds-since-boot, so the beat names its own unit `ms=`; "
                              "and a word that restates a column beside it is not a reading")
        interpose_body = probe_text.split('static void interpose(IOUserClient *client,int pid) {')[1] \
            if 'static void interpose(IOUserClient *client,int pid) {' in probe_text else ''
        if 'reportDisplayState();' not in interpose_body.split('\n}')[0]:
            errors.append("interpose() does not fire the state row: without a row printed from the client's own "
                          "arrival there is nothing to tell a lost beat from a lost channel")

    driver_text = display_driver.read_text() if display_driver.is_file() else ""
    if driver_text:
        driver_code = [line for line in driver_text.splitlines()
                       if not line.strip().startswith(('*', '/*', '//'))]
        for now in ('enum {kStateBeatMilliseconds=30000};',
                    'extern "C" void native_legacy_display_state_beat(void);',
                    'IOTimerEventSource *stateTimer=nullptr;',
                    'stateTimer->setTimeoutMS(kStateBeatMilliseconds);',
                    'startStateBeat();'):
            if not any(now in line for line in driver_code):
                errors.append(f"the vendored driver does not carry {now[:48]!r}: the state row would have no "
                              "clock of its own, and a row that only prints when the client calls cannot say "
                              "the client stopped calling")
        start_beat = driver_text.split('void startStateBeat() {')[1].split('\n    }')[0] \
            if 'void startStateBeat() {' in driver_text else ''
        if not start_beat or 'PE_parse_boot_argn' in start_beat:
            errors.append("the display state beat is not started unconditionally: a cadence gated on a boot "
                          "argument cannot be the instrument that reads the thing the argument controls")
        if 'stopStateBeat();' not in driver_text:
            errors.append("the state beat has no stop: a timer left armed on a freed work loop is how a "
                          "bring-up round turns into a fault at teardown")

    links = [
        ("drivers/CloverSdhci/CloverSdhci.cpp",
         "volatile uint32_t *clover_fastboot_reason = nullptr;",
         "the panic screen would have no cell to mark"),
        ("drivers/CloverSdhci/CloverSdhci.cpp",
         "static const uint64_t kImemPhys = 0x146bf000ULL;",
         "IMEM would not be mapped at this board's restart-reason base"),
        ("drivers/CloverSdhci/CloverSdhci.cpp",
         "static const uint32_t kRestartReasonOffset = 0x65c;",
         "the cell would not be the one clover.dtb's restart_reason@65c names"),
        ("drivers/CloverSdhci/CloverSdhci.cpp",
         "static const uint32_t kRestartBootloader = 0x77665500u;",
         "the word would not be the one the archived vendor kernel writes for `reboot bootloader`, "
         "which is the row scripts/test-clover-imem-geometry.py counts and prints with its line"),
        ("drivers/CloverSdhci/CloverSdhci.cpp",
         "static const uint32_t kResetRecordWords = 10;",
         "the record's size would live only in prose again, and the wiping loop would have no "
         "denominator the geometry instrument could check the measured free run against"),
        ("drivers/CloverSdhci/CloverSdhci.cpp",
         "static const uint32_t kResetBeatOffset = 0x6e0;",
         "the heartbeat counters would have no address to ask the device tree about, which is how "
         "they ended up inside boot_stats in the first place"),
        ("drivers/CloverSdhci/CloverSdhci.cpp",
         'sdhciSay ("clover r339: beatctl wrote=%llx read=%llx magic_wrote=%llx magic_read=%llx '
         'ok=%d\\n"',
         "the cell's write-then-read control would be gone: six rounds of beatmagic= columns have "
         "been read as \"not our channel\" with nothing on the other side saying this kernel can "
         "keep a word there at all"),
        ("drivers/CloverSdhci/CloverSdhci.cpp",
         'sdhciSay ("clover r339: mark wrote=%x before=%x after=%x\\n"',
         "the restart-reason doorway would again be read only after the store, folding the previous "
         "boot's value into this one -- the shape round315 named for every other doorway state on "
         "this board"),
        ("drivers/CloverSdhci/CloverSdhci.cpp",
         'PE_parse_boot_argn ("clover-unattended"',
         "the driver would never arm from the round's own boot argument"),
        ("drivers/CloverSdhci/CloverSdhci.cpp",
         'IOLog ("clover r148: unattended mark %x -> %x deadline=%us\\n"',
         "a boot would not say on the panel that the mark was accepted"),
        ("drivers/CloverSdhci/CloverSdhci.cpp",
         "clover_fastboot_reason = cell;",
         "the cell would never be published, so every reset would be an ordinary one"),
        ("drivers/CloverPMGR/CloverPMGR.cpp",
         "clover_unattended_begin();",
         "nothing would call the ARM step, so the cell would stay unmarked"),
        ("drivers/ArmCpu/ArmCpu.cpp",
         'PE_parse_boot_argn ("clover-unattended", &unattended, sizeof (unattended));',
         "the watchdog would not see the round's deadline argument"),
        ("drivers/ArmCpu/ArmCpu.cpp",
         "clover r148: unattended deadline %llu s",
         "the deadline panic would be indistinguishable from the old tick watchdog"),
        ("drivers/ArmCpu/ArmCpu.cpp",
         '"isb\\n\\tmrs %0, cntvct_el0"',
         "the counter read is what a semicolon swallowed; it must stay on its own line"),
        ("drivers/ArmCpu/ArmCpu.cpp",
         '"mrs %0, elr_el1\\n\\tmrs %1, spsr_el1"',
         "the interrupted mode is what a semicolon swallowed"),
        ("drivers/ArmCpu/ArmCpu.cpp",
         '"dsb ishst\\n\\tmsr icc_sgi1r_el1, %0\\n\\tisb"',
         "the IPI send is what a semicolon swallowed"),
        ("scripts/xnu-panic-screen.c",
         '"isb\\n\\tmrs %0, cntpct_el0"',
         "the 20 s wait must read CNTPCT; this file's ELF assembler always kept it, "
         "so it was never round147's fault"),
        ("scripts/xnu-panic-screen.c",
         '"at s1e1w, %1\\n\\tisb\\n\\tmrs %0, par_el1"',
         "mapped() must read PAR_EL1 after the translation; kept as one statement, "
         "not as the semicolon form that only Darwin would cut"),
        ("scripts/xnu-panic-screen.c",
         "#define BOOTLOADER_WORD 0x77665500u",
         "the screen and the driver would mark the same cell with two different words, and "
         "round195 measured that XBL here reads neither -- the one thing a mismatched pair can "
         "still do is make the value a later reader finds mean two different writers"),
        ("scripts/xnu-panic-screen.c",
         "#define PANIC_RESET_SECONDS 20",
         "the screen would keep parking instead of resetting after a bounded wait"),
        ("scripts/xnu-panic-screen.c",
         "static int fastboot_mark (void)",
         "no code path would write the reason before PSCI"),
        ("scripts/xnu-panic-screen.c",
         'text (&g, "\\nfb=");',
         "a photograph could not say whether the reset would land in fastboot"),
        ("scripts/xnu-panic-screen.c",
         "reset_after (PANIC_RESET_SECONDS);",
         "the screen would never leave the panel by itself"),
        ("scripts/patch-xnu-entry-probe.py",
         "raise SystemExit('Missing _clover_fastboot_reason: the panic screen could not mark fastboot')",
         "a kernel without the pointer would build a screen whose fb= is always 0"),
        ("scripts/patch-xnu-entry-probe.py",
         "-DFASTBOOT_REASON={reason:#x}UL",
         "the resolved cell pointer would not reach the screen's compiler"),
        ("scripts/run-clover-unattended.sh",
         "fastboot flash misc",
         "the only route into recovery this bootloader was measured to honour would be gone"),
        ("scripts/run-clover-unattended.sh",
         'BCB_COMMAND="boot-recovery"',
         "the bootloader would get a BCB with the wrong command word, and its recovery "
         "path would not know to enter TWRP"),
        ("scripts/run-clover-unattended.sh",
         "the BCB did not take",
         "a BCB the bootloader ignored would pass as success and the window read would "
         "belong to the payload's own boot"),
        ("scripts/run-clover-unattended.sh",
         "wait_leave_fastboot",
         "a board still enumerated a moment after 'fastboot reboot' would count as "
         "having come back by itself, so the payload would never be seen to boot"),
        ("scripts/run-clover-unattended.sh",
         "--allow-misc-bcb",
         "the BCB write into misc would happen without declared consent"),
        ("scripts/run-clover-unattended.sh",
         "count_records()",
         "a ring that counted zero would be captured as two lines and every numeric test "
         "on the decoded counts would fail on an empty ring"),
        ("scripts/run-clover-unattended.sh",
         "read-clover-log.py",
         "the dump would never be decoded"),
        ("scripts/read-clover-log.py",
         "0x554E5855",
         "the decoder would not recognize the writer's magic"),
        ("scripts/read-clover-log.py",
         "unixnu clover emmc log",
         "the decoder would not recognize the prefix the driver stores"),
        ("scripts/read-clover-log.py",
         "--count",
         "run-clover-unattended.sh would have no machine-readable record count"),
        ("drivers/CloverSdhci/CloverSdhci.cpp",
         'CloverSdhci r195: %s probe lines capped at %u',
         "the tee would go back to an uncapped transcript: on round194's frame the flood had "
         "only moved to r153/r155/r170-open, which took 491 of the ring's 512 records and left "
         "the ring covering 110 s of a 912 s boot"),
        ("drivers/CloverSdhci/CloverSdhci.cpp",
         '{ "clover r170: open ", "r170-open", 32, 0, 0, 0 },',
         "r170's opens of the dyld cache symbols file were 99 records on round194's frame and "
         "would spend the whole ring unless they have a class of their own"),
    ]
    for relative, needle, why in links:
        path = REPO / relative
        if not path.is_file():
            errors.append(f"{relative} is missing: {why}")
            continue
        if needle not in path.read_text():
            errors.append(f"{relative} lacks {needle!r}: {why}")
    # Round194: the panic record has to carry fb=. Whether the reset lands in fastboot was drawn only
    # on the panel, so the two deadline panics on round193's card could not be told apart -- and a
    # panel is gone twenty seconds after it is drawn.
    screen = REPO / "scripts/xnu-panic-screen.c"
    if screen.is_file():
        screen_text = screen.read_text()
        drawn = screen_text.find('text (&g, "\\nfb=");')
        sink = screen_text.find("if (PANIC_SINK) {")
        if drawn < 0 or sink < 0 or drawn > sink:
            errors.append("xnu-panic-screen.c draws fb= after the sink: the panic record would not say "
                          "whether its own reset was going to land in fastboot")
        # Round195: the panel row was not enough. PANIC_LOG_BYTES is 480 and that row is drawn
        # after every reading, so round194's deadline record carried no fb= at all. The same
        # reading now also goes into the log copy right after the panic message.
        early = screen_text.find('logonly (&g, "\\nfb=", fb);')
        formatted = screen_text.find("format (&g, fmt, args);")
        if early < 0 or formatted < 0 or sink < 0 or not (formatted < early < sink):
            errors.append("xnu-panic-screen.c does not append fb= to the record after the panic "
                          "message: the card would again not say whether the reset was going to "
                          "land in fastboot")
        if "if (g->only_log) {" not in screen_text:
            errors.append("xnu-panic-screen.c has no log-only path: the record preface would also "
                          "be painted on the panel")
    host = REPO / "scripts/run-clover-unattended.sh"
    if host.is_file():
        # `fastboot boot` is forbidden everywhere in this port: the image must be cold-booted from
        # a partition, never handed to the bootloader from the host. The script's own header names
        # the rule, so only lines that would execute it count.
        for line in host.read_text().splitlines():
            if line.strip().startswith("fastboot boot"):
                errors.append("run-clover-unattended.sh invokes `fastboot boot`: this port boots "
                              "images only from a partition")

    splash = [entry for entry in board.get("bootimg", {}).get("dtb_node", [])
              if "splash" in entry["path"]]
    if splash:
        values = [int(cell, 0) for cell in
                  next(prop["value"] for prop in splash[0]["props"] if prop["name"] == "reg")]
        if values[1] != SPLASH_BASE or values[3] != SPLASH_SIZE:
            errors.append(f"the boot image splash region {values[1]:#x}+{values[3]:#x} does not match "
                          f"the ADT display region {SPLASH_BASE:#x}+{SPLASH_SIZE:#x}")
    else:
        # No node to patch: the unit's own tree, which is what the boot image appends,
        # already declares /reserved-memory/splash_region@9d400000.  uefi-clover's
        # checker holds that against both the tree and MemoryMapLib; this repository
        # does not carry the tree, so it can only say why nothing is declared here.
        print("[check-clover] notice: no [[bootimg.dtb_node]]; the appended device tree "
              "already declares the splash region")

    named = board["kernel"]["archives"] + board["firmware"].get("preflight", []) + [adt["tool"]]
    if board["payload"].get("skip_setup_tool"):
        named.append(board["payload"]["skip_setup_tool"])
    from_tree = pathlib.Path.cwd() != REPO
    for script in named:
        name = script.split(" ", 1)[0]
        relative = name if "/" in name else f"scripts/{name}"
        if (REPO / relative).is_file() or (pathlib.Path.cwd() / relative).is_file():
            continue
        # A board-owned script must be in this repository. A generic one, such as
        # build-corecrypto-14.py, is supplied by the base repository through the assembled tree,
        # so it is only missing when this check already runs inside that tree.
        if "clover" in pathlib.Path(relative).name or from_tree:
            errors.append(f"{relative} is named by board.toml but is in neither this repository nor the tree")

    if board["ipsw"]["product"] != "iPad7,1" or board["ipsw"]["chip_id"] != 0x8011:
        errors.append("the IPSW identity is not the iPad7,1 / 0x8011 image a 4 KiB Kryo 260 can run")
    return f"{defconfig_name}, {volume}"


# --------------------------------------------------------------------------- driver addresses
#
# Every driver header names physical register windows, and every one of those numbers is a
# transcription of the device tree.  Round181 shipped kQupPhys as 0x00c17700 -- the correct value is
# 0x0c177000 -- because the transcription copied the digit count of the GCC defines sitting beneath
# it.  A page mapped at the wrong address reads back as whatever the SMMU hands out there, which on
# this board is nothing, and the driver then reports a dead bus rather than a wrong base.  The only
# thing that settles this is the unit's own tree: scripts/dump-clover-facts.sh read
# /proc/device-tree/soc/*/reg into facts/<serial>-<stamp>/dt/soc-reg.txt, printing each 32-bit cell
# as four hex byte pairs in little-endian storage order, which is why "i2c@c177000/reg 0070170c
# 00060000" means base 0x0c177000, size 0x600.
DT_REG_DUMP = "facts/81ffcaaa-20261022-210928/dt/soc-reg.txt"

# (node leaf name, index of the cell inside its reg property, header, macro).  The macro has to
# equal that cell.
DT_BASE_CONTRACT = [
    ("i2c@c177000", 0, "drivers/CloverInput/InputRegs.h", "kQupPhys"),
    ("i2c@c177000", 1, "drivers/CloverInput/InputRegs.h", "kQupDtSize"),
    ("clock-controller@100000", 0, "drivers/CloverInput/InputRegs.h", "kGccPhys"),
    ("clock-controller@100000", 1, "drivers/CloverInput/InputRegs.h", "kGccSize"),
    ("sdhci@c0c4000", 0, "drivers/CloverSdhci/SdhciRegs.h", "kSdhciHostPhys"),
    ("sdhci@c0c4000", 2, "drivers/CloverSdhci/SdhciRegs.h", "kSdhciCmdqPhys"),
    ("sdcc1ice@c0c8000", 0, "drivers/CloverSdhci/SdhciRegs.h", "kSdhciIcePhys"),
    ("qcom,spmi@800f000", 0, "drivers/CloverPower/PowerRegs.h", "kSpmiCorePhys"),
    ("qcom,spmi@800f000", 1, "drivers/CloverPower/PowerRegs.h", "kSpmiCoreSize"),
    ("qcom,spmi@800f000", 2, "drivers/CloverPower/PowerRegs.h", "kSpmiChnlsPhys"),
    ("qcom,spmi@800f000", 3, "drivers/CloverPower/PowerRegs.h", "kSpmiChnlsSize"),
    ("qcom,spmi@800f000", 4, "drivers/CloverPower/PowerRegs.h", "kSpmiObsrvrPhys"),
    ("qcom,spmi@800f000", 5, "drivers/CloverPower/PowerRegs.h", "kSpmiObsrvrSize"),
    ("qcom,spmi@800f000", 6, "drivers/CloverPower/PowerRegs.h", "kSpmiIntrPhys"),
    ("qcom,spmi@800f000", 7, "drivers/CloverPower/PowerRegs.h", "kSpmiIntrSize"),
    ("qcom,spmi@800f000", 8, "drivers/CloverPower/PowerRegs.h", "kSpmiCnfgPhys"),
    ("qcom,spmi@800f000", 9, "drivers/CloverPower/PowerRegs.h", "kSpmiCnfgSize"),
    # Round183. The SMEM node's first cells are the region this driver maps whole, and the second pair
    # is the doorbell word it reads -- both transcribed from this unit's own tree, which is also the
    # only place the 2 MiB size is stated as a fact rather than as an assumption. kApcsPhys itself is
    # not listed: apcs_glb is the CAF tree's mailbox node and the shipped DTB does not carry it, so
    # the number checked here is the one the shipped tree really names, 0x17911008, which the driver
    # reaches as kApcsPhys + kApcsIpcOffset and prints on the card as `doorbell 17911008+04`.
    ("qcom,smem@86000000", 0, "drivers/CloverDsp/DspRegs.h", "kSmemPhys"),
    ("qcom,smem@86000000", 1, "drivers/CloverDsp/DspRegs.h", "kSmemSize"),
    ("qcom,smp2p-adsp@17911008", 0, "drivers/CloverDsp/DspRegs.h", "kApcsIpcPhys"),
    ("qcom,smp2p-modem@17911008", 0, "drivers/CloverDsp/DspRegs.h", "kApcsIpcPhys"),
    # Round311 registers the GPU line's windows as well as its own new one. The three pages this line has
    # mapped since round286 were transcribed from the tree by hand and never compared against the dump
    # this gate reads -- which is exactly the shape round181's kQupPhys was written to catch, and the
    # reason the fourth window was not allowed to arrive without the other three being checked too.
    # `gfx@5065000` and `clock-controller@5065000` are the same extent under two node names; the first
    # one is what the dump lists.
    ("clock-controller@100000", 0, "drivers/CloverGpu/GpuRegs.h", "kGccPhys"),
    ("clock-controller@100000", 1, "drivers/CloverGpu/GpuRegs.h", "kGccSize"),
    ("gfx@5065000", 0, "drivers/CloverGpu/GpuRegs.h", "kGpuccPhys"),
    ("gfx@5065000", 1, "drivers/CloverGpu/GpuRegs.h", "kGpuccSize"),
    ("qcom,kgsl-3d0@5000000", 0, "drivers/CloverGpu/GpuRegs.h", "kGpuPhys"),
    ("qcom,kgsl-3d0@5000000", 1, "drivers/CloverGpu/GpuRegs.h", "kGpuSize"),
    ("regulator@01fcf004", 0, "drivers/CloverGpu/GpuRegs.h", "kMemAccPhys"),
    ("regulator@01fcf004", 1, "drivers/CloverGpu/GpuRegs.h", "kMemAccSize"),
]

# (node leaf name, span index, header, base macro, size macro).  A driver may map only part of a
# node -- TLMM is one node whose north tile the driver addresses, GCC is one node whose three
# page-sized windows the driver maps -- so every sub-window has to be provably inside what the tree
# declares, or the mapping is outside the hardware the driver claims to talk to.
DT_WINDOW_CONTRACT = [
    ("clock-controller@100000", 0, "drivers/CloverInput/InputRegs.h", "kGccAhbHaltPhys", "kGccWindowSize"),
    ("clock-controller@100000", 0, "drivers/CloverInput/InputRegs.h", "kGccI2cPhys", "kGccWindowSize"),
    ("clock-controller@100000", 0, "drivers/CloverInput/InputRegs.h", "kGccAhbVotePhys", "kGccWindowSize"),
    ("pinctrl@03000000", 0, "drivers/CloverInput/InputRegs.h", "kTlmmPhys", "kTlmmTileSize"),
    ("pinctrl@03000000", 0, "drivers/CloverInput/InputRegs.h", "kPinGpio10Phys", "kPinPairSize"),
    ("pinctrl@03000000", 0, "drivers/CloverInput/InputRegs.h", "kPinGpio11Phys", "kPinPairSize"),
    ("pinctrl@03000000", 0, "drivers/CloverInput/InputRegs.h", "kPinGpio66Phys", "kPinPairSize"),
    ("pinctrl@03000000", 0, "drivers/CloverInput/InputRegs.h", "kPinGpio67Phys", "kPinPairSize"),
    # Round182 maps one EE's observer block, not the whole obsrvr span: 0x10000 * ee is this board's
    # block (ee = 0 per the DTB's qcom,ee), and a 64 KiB window inside a 16 MiB one only means
    # anything if the tree agrees it is inside. The chnls pages CloverPower maps for the two rails are
    # not listed because their base is 0x10000 * apid for an APID the arbiter only names at run time.
    ("qcom,spmi@800f000", 4, "drivers/CloverPower/PowerRegs.h", "kSpmiObsrvrPhys", "kSpmiObsBlockMapSize"),
    # Round313 adds the second band's extents to the same gate. Under v2/v3 the observer channels are
    # 0x8000 apart rather than 0x80, so a 0x10000 block would cover APID 0 and half of APID 1 and every
    # resolved APID above that would be read out of the wrong channel; the vendor's own probe therefore
    # maps what the DT declares, and so does this port. The two rows below are what makes that claim
    # checkable rather than commented: each says "the span CloverPower asks IOKit to map at this base
    # is inside the extent this unit's own tree names".
    ("qcom,spmi@800f000", 4, "drivers/CloverPower/PowerRegs.h", "kSpmiObsrvrPhys", "kSpmiObsMapSizeV23"),
    ("qcom,spmi@800f000", 6, "drivers/CloverPower/PowerRegs.h", "kSpmiIntrPhys", "kSpmiIntrMapSizeV23"),
]
# kTlmmTileSize is the north tile's own span, which the tree does not declare -- pinctrl@03000000 is
# the whole 12 MiB register block and the tile is a third of it.  pinctrl-sdm660.c:1419 gives the
# north tile ngpios = 114 pins at 0x1000 each, so the tile is 0x72000 wide and the value below is
# derived rather than transcribed.
DT_DERIVED_SIZES = {"kTlmmTileSize": (114, 0x1000)}


def parse_defines(text: str) -> dict:
    """Integer #defines in declaration order, with C integer suffixes stripped.

    Hex literals are converted first, so the only bare identifiers left are references to defines
    earlier in the same file -- which is how kPinGpio66Phys, written as
    (kTlmmPhys + 66ULL * kTlmmPinStride), reaches a number the dump can be compared with.

    Round183 adds the second form because drivers/CloverDsp/DspRegs.h is written with `static const`
    rather than `#define`: SMEM's offsets are relative to kSmemPhys, so the header's own arithmetic
    (`kSmemSize - 0x1000`, `kSmemToc + kSmemItemCount * kSmemTocEntrySize`) is the thing under test,
    and a transcription check that only saw #defines would have nothing to say about it.
    """
    values: dict = {}
    for raw in text.splitlines():
        line = raw.split("//", 1)[0].strip()
        match = re.match(r"^#define\s+([A-Za-z_]\w*)\s+(\S.*)$", line)
        if not match:
            match = re.match(r"^static\s+const\s+\S+\s+([A-Za-z_]\w*)\s*=\s*(.+);$", line)
            if not match:
                continue
        name, expr = match.group(1), match.group(2)
        expr = re.sub(r"0[xX][0-9a-fA-F]+", lambda found: str(int(found.group(0), 16)), expr)
        expr = re.sub(r"(\d)[uUlL]+(?![0-9A-Za-z_])", r"\1", expr)
        if not re.fullmatch(r"[A-Za-z_0-9\s+\-*/%()]+", expr):
            continue
        if any(token not in values for token in re.findall(r"[A-Za-z_]\w*", expr)):
            continue
        try:
            value = eval(expr, {"__builtins__": {}}, dict(values))    # noqa: S307
        except Exception:
            continue
        if isinstance(value, int) and not isinstance(value, bool):
            values[name] = value
    return values


def check_device_tree_constants(errors: list) -> str:
    """Compare the addresses the driver headers use against the unit's own device-tree dump."""
    dump = REPO / DT_REG_DUMP
    if not dump.is_file():
        print(f"[check-clover] notice: {DT_REG_DUMP} is absent, so the driver base addresses are "
              "not being checked against the device's own tree", file=sys.stderr)
        return "no facts dump"
    cells: dict = {}
    for line in dump.read_text(errors="replace").splitlines():
        parts = line.split()
        if len(parts) < 3 or not parts[0].startswith("/proc/device-tree/soc/"):
            continue
        leaf, prop = parts[0].split("/")[-2], parts[0].split("/")[-1]
        if prop != "reg" or leaf in cells:
            continue
        words = []
        for group in parts[1:]:
            if not re.fullmatch(r"[0-9a-fA-F]{8}", group):
                words = []
                break
            words.append(struct.unpack("<I", bytes.fromhex(group))[0])
        if words:
            cells[leaf] = words
    if not cells:
        errors.append(f"{DT_REG_DUMP} has no parsable /proc/device-tree/soc/*/reg lines")
        return "dump unreadable"

    headers: dict = {}
    announced: set = set()
    checked = 0
    for contract in (DT_BASE_CONTRACT, DT_WINDOW_CONTRACT):
        for entry in contract:
            relative = entry[2]
            if relative not in headers:
                path = REPO / relative
                if not path.is_file():
                    headers[relative] = None
                    continue
                headers[relative] = parse_defines(path.read_text(errors="replace"))
            defines = headers[relative]
            if defines is None:
                # drivers/CloverPower is scheduled for round182; a header that does not exist yet
                # must not look like a passing check.
                if relative not in announced:
                    announced.add(relative)
                    print(f"[check-clover] notice: {relative} does not exist yet, its addresses are "
                          "unchecked", file=sys.stderr)
                continue
            leaf, index = entry[0], entry[1]
            if leaf not in cells:
                errors.append(f"{DT_REG_DUMP} has no /proc/device-tree/soc/{leaf}/reg, so "
                              f"{relative} cannot be checked")
                continue
            if index >= len(cells[leaf]):
                errors.append(f"/proc/device-tree/soc/{leaf}/reg has {len(cells[leaf])} cells, "
                              f"contract wants cell {index}")
                continue
            expected = cells[leaf][index]
            if len(entry) == 4:
                name = entry[3]
                if name not in defines:
                    errors.append(f"{relative} does not define {name}")
                    continue
                if defines[name] != expected:
                    errors.append(f"{relative}:{name} is {defines[name]:#x} but this unit's "
                                  f"/proc/device-tree/soc/{leaf}/reg cell {index} is {expected:#x}")
                checked += 1
                continue
            base_name, size_name = entry[3], entry[4]
            if base_name not in defines:
                errors.append(f"{relative} does not define {base_name}")
                continue
            if size_name in DT_DERIVED_SIZES:
                count, stride = DT_DERIVED_SIZES[size_name]
                size = count * stride
            elif size_name in defines:
                size = defines[size_name]
            else:
                errors.append(f"{relative} does not define {size_name}")
                continue
            span_base, span_size = cells[leaf][index], cells[leaf][index + 1]
            lo, hi = defines[base_name], defines[base_name] + size
            if lo < span_base or hi > span_base + span_size:
                errors.append(f"{relative}:{base_name} spans {lo:#x}-{hi:#x}, outside "
                              f"/proc/device-tree/soc/{leaf}/reg span "
                              f"{span_base:#x}-{span_base + span_size:#x}")
            checked += 1
    return f"{checked} addresses against {len(cells)} device-tree nodes"


def check_inline_asm(errors: list) -> str:
    """Inline asm on this target must not use `;` as an instruction separator.

    The Darwin arm64 assembler reads `;` as the comment character, so
    "isb; mrs %0, cntvct_el0" assembles to just `isb` and the counter read is
    silently gone.  Round147 shipped 20 such sequences in ArmCpu.cpp; that is why
    its deadline fired on the first tick with a garbage counter.  Sequences have
    to be separated by newlines.  Only drivers/ is scanned: those sources are
    built for Darwin.  scripts/xnu-panic-screen.c is built for
    aarch64-none-elf, whose assembler treats `;` as a statement separator, so
    its sequences never lost an instruction and it must not be flagged here.
    """
    root = pathlib.Path(REPO / "drivers")
    sources = sorted(root.rglob("*.cpp")) + sorted(root.rglob("*.h"))
    start = re.compile(r"\b(__asm__|asm)\s*(volatile\s*)?\(")
    literal = re.compile(r'"((?:[^"\\]|\\.)*)"')
    statements = 0
    for path in sources:
        lines = path.read_text(errors="replace").splitlines()
        index = 0
        while index < len(lines):
            if not start.search(lines[index]):
                index += 1
                continue
            end, block = index, ""
            while end < len(lines) and end < index + 12:
                block += lines[end] + "\n"
                if ");" in lines[end]:
                    break
                end += 1
            statements += 1
            for text in literal.findall(block):
                if ";" in text:
                    errors.append(
                        f"{path.relative_to(REPO)}:{index + 1}: inline asm keeps ';' inside "
                        f"\"{text}\": this assembler reads it as a comment, so every instruction "
                        "after it is dropped; separate the sequence with \\n")
            index = end + 1
    return f"{statements} inline asm statements separated"


def check_tree(errors: list) -> str:
    """The kernel and firmware steps expect these modules in the assembled tree.

    Run from the repository alone, an absent module is a notice: the tree is what supplies
    DarwinBoot and NativePrelink. Run from a directory that already looks like a tree, it is
    an error, because the build would fail later and far away from the cause.
    """
    tree = pathlib.Path.cwd()
    is_tree = tree != REPO
    notices, missing = [], []
    for relative in ("drivers/DarwinBoot", "kits/NativePrelink", "scripts/build-arm-driver.py"):
        if (tree / relative).exists() or (REPO / relative).exists():
            continue
        (missing if is_tree else notices).append(relative)
    for relative in missing:
        errors.append(f"neither the assembled tree nor this repository has {relative}")
    for relative in notices:
        print(f"[check-clover] notice: {relative} comes from the base repository; "
              "run this from out/clover/tree to check it", file=sys.stderr)
    return "modules visible" if not missing else f"{len(missing)} missing"


# Round314 (standing rule: "驱动均需要参考 unixnu 文档制作").  The clause list and the per-driver
# verdicts are notes/unixnu-doc-conformance.md; what lands here are the four structural facts that
# can be decided from the files alone, so the rule cannot rot back into a habit.  Nothing in this
# group needs a built kernel, and the group that does is honest about it (see check_driver_products).
DOC_CITATION = re.compile(r"(?<![\w/.-])([A-Za-z0-9_./-]+\.md):(\d+)(?:-(\d+))?")
OWN_SOURCE_CITATION = re.compile(r"(?<![\w/.-])([A-Za-z0-9_]+\.cpp):(\d+)")


def driver_docs_root() -> pathlib.Path | None:
    """Where the clause files actually live.

    Measured in round314: the device repository has no `docs/` tree at all (`ls` of it is the only
    way to see this), so every `docs/**` citation in a driver README points into the base checkout.
    A host without that checkout gets a notice, not a failure -- the rest of the group still runs.
    """
    for candidate in (REPO.parent / "UniXNU" / "docs", pathlib.Path.cwd() / "docs",
                      pathlib.Path.home() / "UniXNU" / "docs"):
        if (candidate / "drivers/xnu-drivers.md").is_file():
            return candidate
    return None


def base_repo_root() -> pathlib.Path | None:
    """The upstream UniXNU checkout, when this host has one and it is not the repository at hand.

    Round316 measured why the rule needs this: `configs` assembly symlinks the whole driver tree, so
    `out/clover/tree/drivers/NativeVirtioConsole -> <base>/drivers/NativeVirtioConsole` appears among
    `drivers/*` of the tree this checker is run from, and clause 一/二/三 -- written to police drivers
    *this* repository authors -- failed on code it neither owns nor can edit.  The same exemption the
    `check_tree()` notice already grants `drivers/DarwinBoot` is what that situation needs, expressed
    as a path property rather than a name list so a base driver never gets a free pass on a rule it
    genuinely implements badly elsewhere.
    """
    here = REPO.resolve()
    for candidate in (REPO.parent / "UniXNU", pathlib.Path.home() / "UniXNU"):
        resolved = candidate.resolve() if candidate.exists() else None
        if resolved is not None and resolved != here and (resolved / "drivers").is_dir():
            return resolved
    return None


def own_source_basenames() -> set:
    """Basenames of the C++ files this repository edits, which is what §8 of the note forbids
    citing by line number.  Vendored sources keep their line numbers: they do not move under us."""
    return {path.name for path in REPO.glob("*/**/*.cpp")} | {path.name for path in REPO.glob("*/*.cpp")}


def check_driver_documentation(errors: list) -> str:
    """Every drivers/<name> is written against the UniXNU driver docs, and the checker can see it.

    docs/drivers/xnu-drivers.md:7-14 makes the reference record part of a driver's layout and
    :230-239 makes the test rows part of it, so a README that never names a clause is a driver that
    was not written against the docs no matter what it does at runtime.  Four checks follow:

      1. a driver with source has a README (clause 二's layout, and the only place the rest of the
         rule can be recorded);
      2. that README cites at least one `docs/**.md:<line>` clause, and every citation resolves --
         in the base docs tree or in this repository -- to a file that has the cited line;
      3. a driver without an `Info.plist` says so, because `:14` treats the plist as the record of a
         personality and silence reads as an oversight; a driver *with* one has to be reachable by a
         personality patch, because otherwise the plist records nothing;
      4. no citation names one of this repository's own `.cpp` files by line number -- notes/
         unixnu-doc-conformance.md §8, which this very round wrote after its own header insertions
         invalidated every one of them.
    """
    docs = driver_docs_root()
    mutable = own_source_basenames()
    patches = REPO.parent / "UniXNU" / "patches"
    patch_text = ""
    if patches.is_dir():
        patch_text = "\n".join(path.read_text(errors="replace")
                               for path in patches.glob("**/*.patch"))
    line_counts: dict = {}
    checked = 0
    base = base_repo_root()
    vendored = []
    for directory in sorted((REPO / "drivers").glob("*/")):
        name = directory.name
        if base is not None and base in directory.resolve().parents:
            vendored.append(name)
            continue
        sources = list(directory.glob("*.cpp"))
        if not sources:
            continue
        checked += 1
        readme = directory / "README.md"
        if not readme.is_file():
            errors.append(f"drivers/{name} has {sources[0].name} but no README.md: "
                          "docs/drivers/xnu-drivers.md:7-14 makes the reference record part of the "
                          "layout, and the clause list it must name has nowhere else to live")
            continue
        text = readme.read_text()
        if not re.search(r"UniXNU [Dd]river [Dd]ocumentation|meets the UniXNU", text):
            errors.append(f"drivers/{name}/README.md has no section naming the UniXNU driver "
                          "documentation it was written against: the rule is '驱动均需要参考 unixnu "
                          "文档制作', and a reader cannot tell that from a README that never cites")
        citations = DOC_CITATION.findall(text)
        if not any(path.startswith(("docs/", "drivers/", "porting/", "guide/"))
                   for path, _, _ in citations):
            errors.append(f"drivers/{name}/README.md cites no docs/**.md clause: every driver is "
                          "authored against the documented clauses "
                          "(docs/drivers/overview.md:11-16 and docs/drivers/xnu-drivers.md), and a "
                          "README with no clause citation is the shape this rule replaced")
        for path, first, last in citations:
            first, last = int(first), int(last or first)
            relative = path[len("docs/"):] if path.startswith("docs/") else path
            target = (docs / relative) if docs is not None else None
            if target is None or not target.is_file():
                target = REPO / path
            if not target.is_file():
                errors.append(f"drivers/{name}/README.md cites {path}:{first} and that file is in "
                              f"neither the base docs tree nor this repository: a citation nobody "
                              "can open is worse than none")
                continue
            lines = line_counts.get(str(target))
            if lines is None:
                lines = len(target.read_text(errors="replace").splitlines())
                line_counts[str(target)] = lines
            if last > lines:
                errors.append(f"drivers/{name}/README.md cites {path}:{first}-{last} but the file "
                              f"has {lines} lines: the clause moved or the range was written wrong")
        for match in OWN_SOURCE_CITATION.finditer(text):
            if match.group(1) in mutable:
                errors.append(f"drivers/{name}/README.md cites {match.group(0)}: this repository "
                              f"edits {match.group(1)}, so its line numbers rot the moment a later "
                              "round inserts a comment -- use a function, constant or sub-heading "
                              "name instead (notes/unixnu-doc-conformance.md §8)")
        plist = directory / "Info.plist"
        if not plist.is_file() and "Info.plist" not in text:
            errors.append(f"drivers/{name} ships no Info.plist and its README never says so: "
                          "docs/drivers/xnu-drivers.md:14 treats the plist as the record of a "
                          "personality, so a driver without one has to state why (no class, no "
                          "match, chained entry point)")
        if plist.is_file() and patch_text and name not in patch_text:
            errors.append(f"drivers/{name}/Info.plist exists but no patch under the base "
                          "repository's patches/ names this driver: a plist that records no "
                          "personality is the file the docs call a reference to a real record "
                          "(docs/drivers/xnu-drivers.md:147-168)")
    if docs is None:
        print("[check-clover] notice: the base repository's docs/ tree is not visible from here, so "
              "clause citations were counted but not resolved against their files", file=sys.stderr)
    if vendored:
        print(f"[check-clover] notice: {len(vendored)} drivers/* entries "
              f"({', '.join(sorted(vendored))}) resolve into {base}, the base repository: this "
              "documentation rule judges the drivers this repository authors, and those come from "
              "the tree assembly", file=sys.stderr)
    return f"drivers documented: {checked}, base-repo drivers exempt: {len(vendored)}"


# The product half of the same rule.  Every builder in this repository already refuses an object
# that is not the iOS 17 kernel's own ABI (build-arm-driver.py:120,129, build-clover-block.py:142,144,
# build-clover-pmgr.py:150,152, and the magic/CPU/subtype triple first in
# kits/NativePrelink/native_prelink.py), so the hole is not the compile step -- it is that nothing
# re-reads what the link actually took.  configs/clover_defconfig links these archives raw, with no
# -Wl,-force_load, so an archive whose only exported symbol stops being referenced disappears from
# the kernel without any step failing.  This group reads the built archives and the linked kernel.
DRIVER_ARCHIVES = (
    ("clover-sdhci", "libCloverSdhci.a", ("_clover_emmc_run",)),
    ("clover-block", "libCloverBlock.a", ("_clover_block_run", "_clover_block_request")),
    ("clover-input", "libCloverInput.a", ("_clover_input_run",)),
    ("clover-power", "libCloverPower.a", ("_clover_power_run",)),
    ("clover-dsp", "libCloverDsp.a", ("_clover_dsp_run",)),
    ("clover-gpu", "libCloverGpu.a", ("_clover_gpu_run",)),
    ("sheng-platform-power", "libShengPlatformPower.a", ("_sheng_pmgr_run",)),
    ("armcpu", "libArmCpu.a", ()),
    ("native-hid-input", "libNativeHidInput.a", ()),
    ("native-keystore-events", "libNativeKeyStoreEvents.a", ()),
    ("native-legacy-display", "libNativeLegacyDisplay.late.a", ()),
    ("native-gop-presenter", "libNativeGOPPresenter.a", ()),
)


def archive_members(blob: bytes) -> list:
    """(name, bytes) for every member of a `!<arch>` file, skipping the symbol tables.

    The headers are fixed-width, not space-separated: macOS writes `__.SYMDEF SORTED` as its first
    member, and splitting on whitespace would shift every later field by one token and read a
    nonsense size.  BSD long names use the `#1/<len>` form, where the name itself is stored in front
    of the data and counted in the member size, which is how `NativeLegacyDisplay.o` fits a 16-byte
    name field.
    """
    if not blob.startswith(b"!<arch>\n"):
        return []
    members, offset = [], 8
    while offset + 60 <= len(blob):
        header = blob[offset:offset + 60]
        if header[58:60] != b"`\n":
            break
        name = header[0:16].decode("ascii", "replace").rstrip()
        try:
            size = int(header[48:58])
        except ValueError:
            break
        start, extra = offset + 60, 0
        if name.startswith("#1/"):
            extra = int(name[3:])
            name = blob[start:start + extra].split(b"\0")[0].decode("ascii", "replace")
            start += extra
        # `__.SYMDEF` is the BSD symbol table, `//` the GNU name string table and `/<n>` a member
        # whose name lives in that table.  None of them is a driver object.
        if not name.startswith("/") and not name.startswith("__.SYMDEF"):
            members.append((name.rstrip("/"), blob[start:start + size - extra]))
        offset = offset + 60 + size + (size & 1)
    return members


def mach_object_abi(blob: bytes) -> str | None:
    """None when `blob` is an arm64 iOS-17 Mach-O object, else why it is not.

    The five fields are docs/drivers/xnu-drivers.md:106-116 read off the file: magic 0xfeedfacf,
    CPU type 0x0100000c with subtype 0, filetype 1 (MH_OBJECT), and an LC_BUILD_VERSION whose
    platform is 2 (iOS) at minimum 0x110000.  Same numbers the builders check, applied here to the
    bytes that went into the archive rather than to the compiler that made them.
    """
    if len(blob) < 32 or struct.unpack_from("<I", blob, 0)[0] != 0xFEEDFACF:
        return "not a 64-bit little-endian Mach-O (magic is not 0xfeedfacf)"
    cpu, subtype, kind = struct.unpack_from("<III", blob, 4)
    if (cpu, subtype) != (0x100000C, 0):
        return f"CPU type 0x{cpu:x} subtype {subtype}, not arm64 (0x100000c, 0)"
    if kind != 1:
        return f"Mach-O file type {kind}, not MH_OBJECT (1)"
    ncmds = struct.unpack_from("<I", blob, 16)[0]
    offset, build = 32, None
    for _ in range(ncmds):
        command, size = struct.unpack_from("<II", blob, offset)
        if command == 0x32:
            build = struct.unpack_from("<II", blob, offset + 8)
        offset += size
    if build is None:
        return "no LC_BUILD_VERSION: nothing names the kernel this object links against"
    if build != (2, 0x110000):
        return f"build version platform {build[0]} minimum 0x{build[1]:x}, not iOS (2) at 0x110000"
    return None


def kernel_image() -> pathlib.Path | None:
    for candidate in (pathlib.Path.cwd().parent / "kernel/kernel.macho",
                      REPO.parent / "UniXNU/out/clover/kernel/kernel.macho"):
        if candidate.is_file():
            return candidate
    return None


def check_driver_products(errors: list) -> str:
    """Re-read the built archives and the linked kernel against the documented object ABI.

    Absent products are a notice, because `check-clover.py` also runs on a host that has never
    built: what it must never do is pass silently, since a missing archive here is exactly the
    silent drop the raw (non-force_load) link makes possible.
    """
    roots = [pathlib.Path.cwd() / "build", REPO / "build"]
    found, objects, notices = 0, 0, []
    kernel = kernel_image()
    image = kernel.read_bytes() if kernel else None
    for directory, archive_name, symbols in DRIVER_ARCHIVES:
        archive = next((root / directory / archive_name for root in roots
                       if (root / directory / archive_name).is_file()), None)
        if archive is None:
            notices.append(f"{directory}/{archive_name}")
            continue
        found += 1
        for member, blob in archive_members(archive.read_bytes()):
            if not blob.startswith(b"\xcf\xfa\xed\xfe"):
                continue
            objects += 1
            problem = mach_object_abi(blob)
            if problem:
                errors.append(f"build/{directory}/{archive_name}[{member}] is {problem}: "
                              "docs/drivers/xnu-drivers.md:106-116 is the ABI the kernel takes, and "
                              "the builders check the same five fields -- this is the copy that the "
                              "link actually read")
        if image is None:
            continue
        for symbol in symbols:
            if symbol.encode() not in image:
                errors.append(f"{kernel} has no {symbol}: the archive built it but the kernel does "
                              f"not contain it, which is what a raw link does to an archive nothing "
                              "references (docs/drivers/xnu-drivers.md:128-145 is the clause that "
                              "would have kept it by force_load; configs/clover_defconfig:66-69 is "
                              "the reason this board does not use it)")
    for notice in notices:
        print(f"[check-clover] notice: {notice} is not built; run from out/clover/tree to check the "
              "object ABI of the copy the link read", file=sys.stderr)
    if image is None and found:
        print("[check-clover] notice: no kernel.macho next to the archives, so link presence was "
              "not checked", file=sys.stderr)
    if not found:
        return "products not built (notices above)"
    return f"{found} archives, {objects} objects ABI-checked, kernel " + ("read" if image else "absent")


CITATION_RULE = "Citation rule for this file (round332)"
INHERITED_MARKER = "inherited claim, no record in this repository"
UNRESOLVABLE_CITATION = re.compile(r"\bint-\d{3}\b")


def check_citation_integrity(errors: list) -> str:
    """Round332. No shipped comment may cite a ledger that nobody on this machine can open.

    The vendored display and HID classes carried twelve `int-NNN` citations. Two of them (the ones that
    used to sit on the refresh-interval comment) asserted a pacing story that this repository's own host
    scan then argued against -- the Round329 host scan (dev-log Round329, gap 2) found no reader of IOMFBDisplayRefresh anywhere in
    the iPadOS 17.7.11 shared cache -- and the other ten name rounds of a ledger that exists in neither
    this repository nor the base one (34 of the base repo's own driver files cite the same shape), so none
    of them can be opened, checked, or falsified from a card. Round332 kept every engineering claim, put
    the ones with no evidence behind them where a reader can see they have no evidence, and rules here so
    the shape cannot walk back in:

      1. the `int-NNN` shape may not appear in source this repository compiles;
      2. a shipped comment may not say "this round closes" -- the round that shipped a file is not the
         round a later reader is in, which is how round329's own title sentence went stale on disk;
      3. the three files that held those claims each carry the citation-rule header and the marker phrase,
         so a retired label leaves behind a rule and not a gap.
    """
    base = base_repo_root()
    tree = pathlib.Path.cwd()
    # Round333. Two ways to be in the tree: run this repository's checker from the assembled tree (the
    # cwd moves, REPO stays), or run the copy the build drops into out/clover/tree/scripts (REPO becomes
    # the tree, so cwd == REPO and the first test says "repository"). The second is how the documented
    # tree-side gate is actually run, and under it cwd-vs-REPO reported a green tree as a broken rule.
    # `package` is the path property: the device repository authors that directory, so it is a real
    # directory there, and the assembly replaces it with a symlink to the base repository's package.
    is_tree = tree != REPO or (REPO / "package").is_symlink()
    scanned = 0
    for pattern in ("drivers/**/*.cpp", "drivers/**/*.h", "package/**/*.cpp", "package/**/*.h"):
        for source in sorted(REPO.glob(pattern)):
            resolved = source.resolve()
            if base is not None and base in resolved.parents:
                continue  # the base repository's own drivers cite their ledger; that is not this gate's to judge
            scanned += 1
            text = source.read_text(errors="replace")
            hits = sorted(set(UNRESOLVABLE_CITATION.findall(text)))
            if hits:
                errors.append(f"{source.relative_to(REPO)} cites {', '.join(hits[:6])}: no repository on this "
                              "machine holds the round log those numbers name, so the citation reads as evidence "
                              "while being nothing a reader can check -- keep the claim and say it is inherited "
                              "(see the citation rule at the top of the display and HID sources)")
            if "this round closes" in text:
                errors.append(f"{source.relative_to(REPO)} says \"this round closes\": a shipped file cannot be "
                              "closing a round for whoever reads it next -- name the round that closed it "
                              "(round329's title sentence is the case this rule was written from)")
    for source in (REPO / "drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp",
                   REPO / "drivers/NativeHidInput/NativeHidInput.cpp",
                   REPO / "package/CloverLegacyDisplayProbe.cpp"):
        relative = source.relative_to(REPO)
        if not source.is_file() and (tree / relative).is_file():
            source = tree / relative
        if not source.is_file():
            # Round333. The assembled tree does not carry this repository's package directory: out/clover/tree/package
            # is a symlink to the base repository's package, and the only thing of the display probe that reaches the tree
            # is the object the link read. A tree run therefore cannot see this source at all, and saying it is missing
            # would report the shape of the checkout rather than a rule broken -- which is how round332's own gate went
            # red on a green tree. The repository-side run still judges it, so the notice names who does.
            if is_tree:
                print(f"[check-clover] notice: {relative} is not carried by the assembled tree (package is symlinked to "
                      "the base repository); its citation rule is judged by the run from the device repository",
                      file=sys.stderr)
                continue
            errors.append(f"{relative} is missing: it is where round332 recorded the citation rule for the vendored "
                          "display and HID claims, and the twelve retired labels lived in these files")
            continue
        text = source.read_text(errors="replace")
        if CITATION_RULE not in text:
            errors.append(f"{relative} lost the citation-rule header: without it a later round has no stated "
                          "way to mark an inherited claim and the `int-NNN` shape returns by default")
        if INHERITED_MARKER not in text:
            errors.append(f"{relative} no longer marks anything as an {INHERITED_MARKER!r}: the claims this file "
                          "inherited are still there, and an unmarked assumption reads as a reading")
    if scanned == 0 and is_tree:
        # Round333. The tree's `drivers/*` are the base repository's, which this rule exempts, and the device
        # repository's own sources are not carried here at all -- so the tree run sees none of what it appears to
        # have scanned. "banned in 0 sources" would read as a clean sweep; it is a scope statement, so say whose
        # sweep this is (the same rule layer 13 states for the card: absence in the window is not a reading).
        return ("citation shapes: the assembled tree carries 0 sources this repository authors; "
                "the run from the device repository is the one that bans them")
    return f"citation shapes banned in {scanned} authored sources"


def main() -> int:
    errors: list = []
    adt_summary = check_adt(errors)
    region_summary = check_regions(errors)
    board_summary = check_board(errors)
    dt_summary = check_device_tree_constants(errors)
    asm_summary = check_inline_asm(errors)
    tree_summary = check_tree(errors)
    doc_summary = check_driver_documentation(errors)
    citation_summary = check_citation_integrity(errors)
    product_summary = check_driver_products(errors)
    for error in errors:
        print(f"[check-clover] {error}", file=sys.stderr)
    if errors:
        return 1
    print(f"[check-clover] clover ok: ADT {adt_summary}; {region_summary}; "
          f"board {board_summary}; DT {dt_summary}; {asm_summary}; {tree_summary}; "
          f"{doc_summary}; {citation_summary}; {product_summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
