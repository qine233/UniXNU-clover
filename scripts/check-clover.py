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
                   "dram-size", "crypto-hash-method", "root-matching", "display-base",
                   "unique-chip-id", "amfi-only-platform-code"]
# One RAMDisk placeholder, no PackedRAMDisk: this board's root is the md0/mockfs image
# (scripts/make-clover-root.py), which DarwinBoot hands over dense, and its AdtNode
# refuses a memory-map that declares a ramdisk twice.
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
            "--root-match-volume-uuid", "04769D26-2CA0-4825-B443-E2E5F88300F2",
            "--chip-id", hex(MSM_ID), "--cpu-regs", ",".join(hex(v) for v in CPU_REGS),
            "--psci-method", "smc", "--unique-chip-id", "0x0123456789ab",
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
        if len(chosen["properties"].get("random-seed", b"")) != 64:
            errors.append("/chosen/random-seed is not 64 bytes")
        if chosen["properties"].get("dram-base") != struct.pack("<Q", DRAM_BASE):
            errors.append(f"/chosen/dram-base is not 0x{DRAM_BASE:x}")
        # AMFI reads this at load and panics ("no amfi-only-platform-code device tree
        # node!", AppleMobileFileIntegrity.cpp:5025) unless it is a 4-byte property.
        if len(chosen["properties"].get("amfi-only-platform-code", b"")) != 4:
            errors.append("/chosen/amfi-only-platform-code is missing or is not a 4-byte integer")

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

    # The root contract. clover has no block driver, so the only root this kernel can
    # find is the RAM one: the payload carries the md0/mockfs image as initrd, the ADT
    # carries its plain RAMDisk placeholder, rd=md0 tells IOFindBSDRoot to take it, and
    # the pinned trust cache carries the init program's cdhash so exec can validate it.
    # A packed placeholder would mean the Data-seed shape instead, which needs NativeRamBlock.
    if adt.get("packed_data_ramdisk"):
        errors.append("[payload.adt] packed_data_ramdisk is true, but this board's root is the "
                      "dense md0/mockfs image; the payload step would emit PackedRAMDisk")
    if "rd=md0" not in board["payload"]["kernel_args"]:
        errors.append('[payload] kernel_args has no rd=md0: without it IOFindBSDRoot waits for '
                      'a root IOMedia that no clover driver publishes')
    if not board["payload"].get("trust_cache_sha256"):
        errors.append("[payload] trust_cache_sha256 is empty: the md0/mockfs init program would "
                      "have no offline trust, and the exec of /sbin/launchd is a panic")

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
