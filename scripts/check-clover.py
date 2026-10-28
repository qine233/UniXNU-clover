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
DRAM_BASE = 0x80000000
DRAM_SIZE = 0x80000000
ARM_IO_BASE = 0x17000000
GICD_PHYS = 0x17A00000
TIMEBASE_HZ = 19200000

REQUIRED_CHOSEN = ["random-seed", "nvram-proxy-data", "nvram-bank-size", "dram-base",
                   "dram-size", "crypto-hash-method", "display-base",
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
        if "libCloverDisplayStub.a" in text and \
                not any("build-clover-display-stub.py" in entry for entry in board["kernel"]["archives"]):
            errors.append("the defconfig links the display stub but [kernel] archives omits its builder")
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
    # Round149. Round148's deadline works, but it also caps every observation at 180 s, and this
    # round's question is how far iPadOS gets when nothing interrupts it. So the argument is gone
    # from the shipped boot while the code that reads it stays: a panic still prints and its screen
    # still resets after 20 s, but with no IMEM mark that reset goes through the ordinary path.
    # "How far" is then answered by the panel, and nothing else, which is the contract asserted here.
    kernel_args = board["payload"]["kernel_args"]
    if any(argument.startswith("clover-unattended=") for argument in kernel_args):
        errors.append("[payload] kernel_args still carries clover-unattended=: round149 removes the "
                      "deadline so the screen, not a timer, is the stop")
    if any(argument.startswith("armcpu-clock-panic=") for argument in kernel_args):
        errors.append("[payload] kernel_args still carries armcpu-clock-panic=: the tick-count "
                      "watchdog would also cut the observation short")
    # Round178. The menu has to name the round under test: the payload and its readings have to agree
    # about which round is on the card, or an image would be claiming a round it does not run.
    if not board["payload"]["title"].startswith("clover r178:"):
        errors.append('[payload] title is not "clover r178: ...": the menu would not name this round')
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
                       "switchExtCsd (kExtCsdCacheCtrl, 0, response, status)",
                       "switchExtCsd (kExtCsdCacheCtrl, 1, backResponse, backStatus)",
                       "CloverSdhci r171: write cache size=",
                       "CloverSdhci r171: flush failed flushed=",
                       "static IOLock *gBlockLock = nullptr;",
                       "gBlockLock = IOLockAlloc ();",
                       "IOLockLock (gBlockLock);",
                       "static void releaseBlockGuard (void)"):
            if needle not in text:
                errors.append(f"{sdhci.name} lacks {needle[:40]}...: r171's batch verdict, cache "
                              "commit or host serialization would be incomplete")
        # A spin-flag guard is what made a contended request degrade: the loser fell out of the
        # batch path and the winner kept the only core busy.
        if "gBlockGuard" in text:
            errors.append(f"{sdhci.name} still arbitrates the host with a spin flag: the batch path "
                          "the flag protects is the one the frame shows is missing")
        if "__atomic_store_n (&gBlockReady, 1, __ATOMIC_RELEASE);" in text and \
                "if (gBlockLock) {" not in text:
            errors.append(f"{sdhci.name} publishes the block device without proving the host lock "
                          "exists: an unsynchronized card engine is worse than no device")
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
    # card. So the gate rewrites a run inside the logfs window -- the partition whose only contents are
    # this port's own bring-up reports -- with bytes it read from that run a moment earlier, and the
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
                       "static const uint32_t kWriteCheckLba = kLogfsFirstLba + 4096u;",
                       "if (!gWriteDmaCheckDone && blocks >= 4) {",
                       "if (chunk >= 4 && gWriteDmaTrusted) {",
                       "gWriteDmaTrusted = controlHeld && dmaWrite && dmaRead && diffDma == 0;",
                       "write16 (SDHCI_TRANSFER_MODE, SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_MULTI | SDHCI_TRNS_DMA);",
                       "countWordDiff (gCheckBatchA, gCheckSingle, kWriteCheckWords, firstDma)",
                       "kWriteCheckLba + kWriteCheckBlocks - 1 > kLogfsLastLba",
                       "CloverSdhci r173: wrcheck lba=",
                       "CloverSdhci r173: ADMA2 writes trusted for this boot",
                       "CloverSdhci r173: writes stay on the PIO path for this boot",
                       "CloverSdhci r173: first dma write lba=",
                       "CloverSdhci r173: dma writes failed %u times, writes go back to the PIO path"):
            if needle not in text:
                errors.append(f"{sdhci_r173.name} lacks {needle[:44]}...: round173's DMA write path, "
                              "the logfs run it is confined to, or the read-back that earns it would "
                              "be missing")
        # Earned, not assumed -- the same rule the read path lives under.
        if "gWriteDmaTrusted = true;" in text:
            errors.append(f"{sdhci_r173.name} trusts DMA writes without reading them back: the card "
                          "would be changed by a descriptor nobody compared")
        # The gate's sample has to stay inside logfs. 4096 sectors past the first LBA, sixteen blocks
        # long, and the window's own bounds are what the refusal reads -- a sample that drifted into
        # userdata would be a port rewriting APFS with an unproven transport.
        sample_start = text.find("static const uint32_t kWriteCheckLba =")
        sample_end = text.find("static void reportWriteDmaFailure", sample_start)
        sample = text[sample_start:sample_end] if sample_start >= 0 and sample_end > sample_start else ""
        if "kLogfsFirstLba + 4096u" not in sample:
            errors.append("the write gate's sample LBA is no longer kLogfsFirstLba + 4096: it has to "
                          "stay past the sixteen panic slots and inside the logfs window")
        # The last thing a dying boot does must not depend on a flag this round introduced: the panic
        # screen and the bring-up report keep their own two-block PIO writes.
        panic_start = text.find("extern \"C\" unsigned int clover_emmc_panic_append")
        panic_body = text[panic_start:panic_start + 4000] if panic_start >= 0 else ""
        if panic_body.count("writePioBlock (") < 2:
            errors.append("clover_emmc_panic_append no longer writes its two blocks through PIO: "
                          "the last line of a dying boot would depend on this round's flag")

    # Round174. The reading is the deliverable, and for nine rounds the deliverable has been a
    # photograph of a panel that had already scrolled: r167 lost `write cache ... off=`, r169 lost the
    # `read <stage>` lines, r170 exists because one probe's own repetition filled the screen and left a
    # whole frame unable to say whether launchd was looping. So this round gives every line the driver
    # measures a second copy on the card -- kind 21 records in a ring at logfs sector 64, clear of the
    # 32 blocks the panic writer owns -- and `read-clover-log.py --verdict` reads them back from the
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
                       "static bool ringWriteRecord (uint32_t which, const char *text, uint32_t length)",
                       "static void flushVerdictRing (void)",
                       "gRingBlock[5] = (which == kRingKernel) ? 22 : 21;",
                       "gRingBlock[8] = gRingSeq[which];",
                       "const uint32_t lba = gVerdictFirst + kRingOffset[which] +",
                       'IOLog ("%s\\n", line);',
                       "gVerdictFirst = entryFirst14;",
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
    if sdhci_r174 is not None:
        text = sdhci_r174.read_text()
        for needle in ("#include <sys/msgbuf.h>",
                       "struct msgbuf *buffer = msgbufp;",
                       "static const uint32_t kTeeScanPerPass = 4096;",
                       "static const uint32_t kTeePushPerPass = 8;",
                       'static const char *const kTeeAllowlist[] = {',
                       '"clover r",',
                       '"cs_blob",',
                       '"mountroot",',
                       "if (magic != MSG_MAGIC)",
                       "static void teeKernelLog (void)",
                       "static void ringPush (uint32_t which, const char *line, uint32_t length)",
                       "ringPush (kRingKernel, gTeeLine + start, length - start);",
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

    reader = next((base / "scripts/read-clover-log.py"
                   for base in (REPO, pathlib.Path.cwd())
                   if (base / "scripts/read-clover-log.py").is_file()), None)
    if reader is None:
        errors.append("scripts/read-clover-log.py is missing: nothing would read the verdict ring back")
    else:
        text = reader.read_text()
        for needle in ('21: "verdict-21"', '22: "kernel-22"', "VERDICT_OFFSET = 64",
                       "VERDICT_SLOTS = 64", "KERNEL_OFFSET = 192", "KERNEL_SLOTS = 512",
                       "DUMP_SECTORS = KERNEL_OFFSET + 2 * KERNEL_SLOTS",
                       '"--verdict"', 'struct.unpack_from("<9I", block)',
                       "order_by_seq=kind is not None", "def selftest()"):
            if needle not in text:
                errors.append(f"{reader.name} lacks {needle[:40]}...: a ring's slots, its place in the "
                              "dump, its kind or its sequence order after a wrap would be missing")
        # Round175: the host has to be able to ask the two producers apart -- "the tee never ran" and
        # "the tee ran and msgbuf matched nothing" are different answers. Round176: they no longer share
        # a ring, so what is asserted is that each ring holds only its own kind, and that reading a
        # driver record from the kernel ring is reported rather than silently trusted.
        for needle in ('DRIVER_PREFIX = b"CloverSdhci "', "def producer(content)",
                       '"--tee"', '"--driver"', "expect_kind is not None and kind != expect_kind",
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
        for needle in ("bs=512 count=1216", "622592", "--verdict", "verdict line(s)"):
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
        for needle in ("NATIVE4K_SKIP", "check_dyld_cache_skip", "check_amfi_allow",
                       "check_read_integrity", "CloverSdhci r167: readcheck",
                       "CloverSdhci r171: write cache size=",
                       "CloverSdhci r172: dmacheck lba=", "DMA_INTEGRITY",
                       "CloverSdhci r173: wrcheck lba=", "WRITE_DMA_INTEGRITY",
                       "check_exec_sig_dump", "clover r166: sig n=%u",
                       # Round177: the verifier has to look at the shipped kernel's identity fields,
                       # not only at the round's strings, or a payload built from a plain kernel would
                       # still pass every needle here.
                       "check_kernel_normalized", "normalize_kernel_macho",
                       "object stamps blank"):
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
                       "static bool handOffBlockGuard (uint64_t &frequency, uint64_t &start, uint64_t &owned)",
                       "static void reportGuardHandoff (const char *dir, uint32_t lba, uint32_t blocks, uint32_t done)",
                       "CloverSdhci r178: %s handoff lba=%u blocks=%u done=%u cuts=%u waits=%u worst=%llu ms",
                       "if (owned + readCounter () - start > deadline) {",
                       "if (!handOffBlockGuard (frequency, start, owned)) {",
                       "if (done < blocks && sinceCut >= kReadChunkBlocks) {"):
            if needle not in text:
                errors.append(f"{sdhci.name} lacks {needle[:44]}...: round178's chunk-boundary hand-off "
                              "would not exist on both paths, or the deadline would have gone back to "
                              "counting the wait")
        # Both directions, or the fix only covers half the frame: the r170 reads were starved by a
        # writer, and a reader holding the host for a 1 GiB dyld run starves the checkpoint flush.
        if text.count("handOffBlockGuard (frequency, start, owned)") < 2:
            errors.append(f"{sdhci.name} hands the guard off on fewer than two paths: the write loop "
                          "and the read loop each have to cut, or one of them still owns the bus for "
                          "the whole request")
        # A hand-off is only fair if the holder that comes back is not the same thread's next request
        # with a queued waiter skipped -- which is why the deadline is charged `owned` and the wait is
        # counted, not assumed. The counters have to be fed from inside acquireBlockGuard.
        acquire_start = text.find("static bool acquireBlockGuard (uint64_t &frequency, uint64_t &start)")
        acquire_body = text[acquire_start:acquire_start + 2600] if acquire_start >= 0 else ""
        if "gGuardWaits = __atomic_add_fetch" not in acquire_body:
            errors.append("acquireBlockGuard does not count the wait it measures: the hand-off report "
                          "would quote cuts and waits nobody observed")

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
         "fastboot reboot recovery",
         "the host half would have no way from fastboot to recovery"),
        ("scripts/run-clover-unattended.sh",
         "--allow-misc-bcb",
         "the BCB write the bootloader may perform would happen without declared consent"),
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
    ]
    for relative, needle, why in links:
        path = REPO / relative
        if not path.is_file():
            errors.append(f"{relative} is missing: {why}")
            continue
        if needle not in path.read_text():
            errors.append(f"{relative} lacks {needle!r}: {why}")
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
    asm_summary = check_inline_asm(errors)
    tree_summary = check_tree(errors)
    for error in errors:
        print(f"[check-clover] {error}", file=sys.stderr)
    if errors:
        return 1
    print(f"[check-clover] clover ok: ADT {adt_summary}; {region_summary}; "
          f"board {board_summary}; {asm_summary}; {tree_summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
