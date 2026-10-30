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
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

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
        # stack. The contract therefore inverts: the argument must be *absent*, and the build+link
        # half still has to agree with itself, because "parked" and "silently never existed" must not
        # be the same reading.
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

        # round184, the unattended deadline returns. Everything above is parked *because* the
        # critical path is the boot itself, and a boot nobody can read is not an answer: round149
        # removed the deadline to let the panel be the stop, and round183 found out what that cost --
        # a boot that gets anywhere never comes back to fastboot, and the panel is a printer that
        # stops drawing when the screen is full -- round183's frame still had the kernel's own version
        # banner on row one and the instrument's `part(3)` on its last row, so nothing after that row
        # was drawn anywhere. So the deadline is back, and the assert is its mirror image:
        # the argument must be present, in a numeric form ArmCpu can turn into a CNTVCT deadline, with
        # a ceiling the host script's own wait can still outlast.
        unattended_args = [argument.split("=", 1)[1] for argument in board["payload"]["kernel_args"]
                           if argument.startswith("clover-unattended=")]
        if not unattended_args:
            errors.append("kernel_args carries no clover-unattended=: round184 wants the board to "
                          "come back to fastboot by itself, and with no deadline a boot that reaches "
                          "userland never does")
        for value in unattended_args:
            if not value.isdigit():
                errors.append(f"clover-unattended={value} is not a plain second count; ArmCpu's "
                              "deadline would not parse")
            elif not 60 <= int(value) <= 1800:
                errors.append(f"clover-unattended={value} is outside the 60..1800 s window: below it "
                              "the deadline cuts off the boot it exists to read, above it the host "
                              "wait gives up before the board does")

        # round184, the panic-time ring drain. The deadline is only useful if the reading survives
        # it, and the two RAM rings do not: kind 21 and kind 22 hold their slots in kernel memory and
        # only reach the card when a driver tail flushes them, so a panic -- which is exactly what the
        # deadline produces -- reset the board with the last lines still in RAM. The drain is asserted
        # as source, not as an argument: it is a property of the sink the panic path already calls.
        sdhci_source = (REPO / "drivers/CloverSdhci/CloverSdhci.cpp").read_text()
        sink = sdhci_source.find("extern \"C\" unsigned int clover_emmc_panic_append "
                                 "(const char *text, unsigned int length)")
        if sink < 0:
            errors.append("drivers/CloverSdhci no longer defines clover_emmc_panic_append(const char "
                          "*, unsigned int): that name and signature are the patcher's PANIC_SINK "
                          "contract")
        elif "panicDrainRings ();" not in sdhci_source[sink:sink + 600]:
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
        if sink >= 0 and "panicMarkRecovery ();" not in sdhci_source[sink:sink + 900]:
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
        if '{ "clover r227: ", "r227-rcv", 256, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round227's receive dump no tee class: one "
                          "summary plus sixty-four receive and body lines at the kill would land "
                          "in the ring uncounted and evict the earlier readings")
        if '{ "clover r217: ", "r217-sbs", 64, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round217's live sample no tee class: a line "
                          "that changes every eight seconds for the length of the boot would land "
                          "in the ring uncounted")
        if '{ "clover r211: ", "r211-kill", 64, 0, 0, 0 },' not in sdhci_source:
            errors.append("drivers/CloverSdhci gives round211's stack-backtrace lines no tee class: "
                          "twelve lines per kill would land in the ring uncounted and could push out "
                          "the exec/exit pairs")
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
                ("CloverSdhci r212: drain exit boot=%u wrote=%u dropped=%u tee_passes=%u ",
                 "no drain exit line: the flush would not report how much it wrote"),
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
        missing = [needle for needle in
                   ("qupWrite (kQupSwReset, 1);",
                    "clover_input_observation[15] = kStepResetCore",
                    "qupWrite (kQupI2cClkCtl, kQupI2cClk400k);",
                    "uint8_t tags[kQupMaxTagsLen]",
                    "(address << 1) | 1",
                    "kQupTagV2DataRdStop",
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
        else:
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
        if "done += written;" not in text:
            errors.append("CloverBlock lost the count of whole blocks a failed write did commit")

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
                    "const uint32_t expectFlags = data ? 0u : 1u;",
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
        if "rememberEntry(root ? &gOsEntry : system ? &gSystemEntry : &gDataEntry, entry);" not in text:
            errors.append("CloverBlock does not keep the Data entry's offset/size/sha256: the "
                          "manifest digest and native_ram_block_volume_seed() would describe "
                          "different extents")

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
    # `part(3)`. Both halves of the reading were lost that way. The argument's presence and its range
    # are asserted above, next to the archives that schedule it; what stays here is the rule that the
    # *tick-count* watchdog must not come back with it, because a clock that panics at N ticks
    # measures the scheduler rather than the boot.
    kernel_args = board["payload"]["kernel_args"]
    if not any(argument.startswith("clover-unattended=") for argument in kernel_args):
        errors.append("[payload] kernel_args has no clover-unattended=: round184 wants the board to "
                      "return to fastboot on its own, and with no deadline a boot that reaches "
                      "userland never does")
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
    if not board["payload"]["title"].startswith("clover r227:"):
        errors.append('[payload] title is not "clover r227: ...": the menu would not name this round')
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
    # The wait now answers both landings, TWRP's adb means the panic's own BCB took, and fastboot
    # means it did not and the host writes its own. Both halves are asserted by name because the
    # sequence around them is the same reboot-then-watch earlier rounds used.
    script_text = (REPO / "scripts/run-clover-unattended.sh").read_text()
    if "watch_after_deadline() {" not in script_text:
        errors.append("scripts/run-clover-unattended.sh has no watch_after_deadline: after "
                      "--boot-after-flash the script would again read a fastboot landing as proof "
                      "of a reset whose recovered landing it never checked")
    # Round202's frame reached SpringBoard at 358 s, the payload's deadline is 900 s of its own
    # uptime, and the script gave the landing exactly WAIT seconds of host time -- so the board's
    # own reset landed after the window closed and the run reported "neither". The reset delay and
    # TWRP's adb coming up have to be inside the same budget.
    if "DEADLINE_SLACK=120" not in script_text:
        errors.append("scripts/run-clover-unattended.sh has no DEADLINE_SLACK: the landing budget "
                      "would again expire before the payload's 900 s deadline plus its reset delay, "
                      "and a reset that did happen would read as nothing coming back")
    if "returned to fastboot by itself, so the deadline fired" in script_text:
        errors.append("scripts/run-clover-unattended.sh still says the board 'returned to fastboot "
                      "by itself, so the deadline fired': round195 showed that sentence on a board "
                      "that had come up in Android and was put into fastboot by hand")
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
                       "static bool cardRecoverHeld (const char *why, uint32_t detail);",
                       "static bool cardRecoverHeld (const char *why, uint32_t detail)",
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
        wrapper = text[wrapper_start:wrapper_start + 600] if wrapper_start >= 0 else ""
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
    if not board["payload"]["title"].startswith("clover r227:"):
        errors.append('[payload] title is not "clover r227: ...": the menu would not name this round')
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
         "static const uint32_t kRestartFastboot = 0x77665501u;",
         "the word would not be the one the stock kernel writes for reboot bootloader"),
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
         "#define FASTBOOT_WORD 0x77665501u",
         "the screen would mark the cell with something XBL does not read as fastboot"),
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
        ("board.toml",
         '"clover-unattended=900"',
         "the deadline would cut a boot that round193's frame shows still moving at 312.9 s, "
         "and the round would be measuring the deadline instead of the boot"),
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


def main() -> int:
    errors: list = []
    adt_summary = check_adt(errors)
    region_summary = check_regions(errors)
    board_summary = check_board(errors)
    dt_summary = check_device_tree_constants(errors)
    asm_summary = check_inline_asm(errors)
    tree_summary = check_tree(errors)
    for error in errors:
        print(f"[check-clover] {error}", file=sys.stderr)
    if errors:
        return 1
    print(f"[check-clover] clover ok: ADT {adt_summary}; {region_summary}; "
          f"board {board_summary}; DT {dt_summary}; {asm_summary}; {tree_summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
