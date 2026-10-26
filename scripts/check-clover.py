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

    # The root contract, round138. The kernel's own vfs_mountroot() is the only mounter. The board
    # declares the root once, in /chosen/root-matching (IOFindBSDRoot deserializes it at
    # IOKitBSDInit.cpp:790-797 and waits for that IOMedia). One `rd=` remains, `rd=md0s39`, and it is
    # not a second declaration: the root search never reaches its rd= branch (:888 is guarded by
    # !matching) and the md shortcut demands a three-character mdX (:849, :1080). Its only reader is
    # bsd_rooted_ramdisk() (bsd_init.c:450-466), which matches the "md0" prefix and skips the
    # FSIOC_KERNEL_ROOTAUTH block -- the gate round137 died at (bsd_init.c:1098) because an unsealed
    # APFS copy cannot pass it. Any other rd= would still declare a root, so it is refused.
    # drivers/CloverBlock makes the service exist before that wait because its run is triggered by the
    # earlier publishResource("IOBSD") inside IOKitBSDInit; under clover-apfs=3 it returns instead of
    # mounting anything itself.
    # Round119's failure was a dictionary naming a volume no service here carried, not the mechanism.
    if str(adt.get("root_match_volume_uuid", "")).upper() != ROOT_VOLUME_UUID:
        errors.append(f"[payload.adt] root_match_volume_uuid must be {ROOT_VOLUME_UUID}: "
                      "/chosen/root-matching is the only root declaration this board has")
    if adt.get("packed_data_ramdisk"):
        errors.append("[payload.adt] packed_data_ramdisk is true, but this board's RAMDisk is the "
                      "unchanged mockfs image; the payload step would emit PackedRAMDisk")
    rd_args = [argument for argument in board["payload"]["kernel_args"]
               if argument == "rd" or argument.startswith("rd=")]
    if rd_args != ["rd=md0s39"]:
        errors.append('[payload] kernel_args must carry exactly one rd=, "rd=md0s39": four characters '
                      "keep it out of IOFindBSDRoot's root declaration (root-matching is the root) "
                      "while bsd_rooted_ramdisk() matches its md0 prefix and skips ROOTAUTH")
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
        for macro in ("XNU_OWNER_CACHE_MOUNT", "XNU_OWNER_CACHE_STARTUP",
                      "XNU_OWNER_UNION_SYSTEMOS"):
            if f"#undef {macro}" not in text or f"#define {macro} 1" not in text:
                errors.append(f"{patch.name} does not #undef then #define {macro} 1: the command "
                              "line's =0 would win and the SystemOS union mount would not run")
    windows = (userdata or {}).get("windows") or []
    if [window.get("name") for window in windows] != ["OS", "SystemOS"]:
        errors.append('[userdata] windows must be OS then SystemOS: CloverBlock publishes them in '
                      'that order so the OS volume is attached and named before the SystemOS window '
                      'is handed to a driver whose single-caller guard forbids overlap')
    if len(windows) == 2 and windows[1].get("component") != "Cryptex1,SystemOS":
        errors.append('[userdata] windows[1].component is not Cryptex1,SystemOS: the manifest slot '
                      "is the dyld cache volume sheng union-mounts, not the OS snapshot")

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
    tree_summary = check_tree(errors)
    for error in errors:
        print(f"[check-clover] {error}", file=sys.stderr)
    if errors:
        return 1
    print(f"[check-clover] clover ok: ADT {adt_summary}; {region_summary}; "
          f"board {board_summary}; {tree_summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
