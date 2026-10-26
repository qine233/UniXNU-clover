#!/usr/bin/env python3
"""Join clover's own trust module with the two original Apple modules of the target IPSW.

The boot trust cache is what AMFI consults for the first exec of /sbin/launchd on the
APFS root, exactly as sheng's boot image carries one owner module followed by the
StaticTrustCache and Cryptex1,SystemTrustCache modules of its own IPSW. clover's root
is the OS volume (rd=md0s39), so the module that vouches for launchd is the static one;
the System module is kept beside it for the cryptex binaries.

Layout (the same one drivers/DarwinBoot ValidTrustCaches accepts):

    <count=3, offsets[3]...> <own module> <static module> <system module>
    module = <version=2, uuid[16], n> <n x (cdhash[20], type, 3 pad)>

The original modules are copied byte for byte and checked against the sha256 of the
unwrapped IM4P payloads of the pinned IPSW, so a different image cannot slip in.

Usage: make-clover-trust-bundle.py [--own FILE] [--static FILE] [--system FILE] [--out DIR]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IPSW_CACHE = Path.home() / "UniXNU/out/clover/ipsw/21H461"
DEFAULT_OWN = ROOT / "build/clover-root/trustcache.bundle"
DEFAULT_STATIC = IPSW_CACHE / "044-76040-038.dmg.trustcache"
DEFAULT_SYSTEM = IPSW_CACHE / "044-76034-038.dmg.trustcache"
DEFAULT_OUT = ROOT / "build/clover-trust"

# sha256 of the unwrapped version-2 modules, as pinned by the upstream OS profile.
STATIC_SHA256 = "da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2"
SYSTEM_SHA256 = "d121ba89167e03aafd00fa687c30cd8ebc4ccafd10c1408adb2ba3de12403651"
# sha256 of the two IPSW members those payloads come from (the files in out/ipsw).
MEMBER_SHA256 = {
    "static": "4563bdbf6ed8f72237d45dbba3ccde432634338f95c10d79d5d4631678a97c7c",
    "system": "9742f3d566369e6c48f63b3dc0740b06c243d13c6650e25bbf44616e13d6d097",
}


def der(data: bytes, at: int):
    if at + 2 > len(data):
        raise SystemExit("truncated DER")
    tag, size = data[at], data[at + 1]
    at += 2
    if size & 0x80:
        count = size & 0x7F
        if not 0 < count <= 4 or at + count > len(data):
            raise SystemExit("invalid DER length")
        size = int.from_bytes(data[at:at + count], "big")
        at += count
    if at + size > len(data):
        raise SystemExit("DER exceeds input")
    return tag, data[at:at + size], at + size


def im4p_payload(member: Path, kind: str) -> bytes:
    """The trust-cache module inside an IM4P-wrapped IPSW member."""
    data = member.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != MEMBER_SHA256[kind]:
        raise SystemExit(f"{member} sha256 {digest} is not the pinned {MEMBER_SHA256[kind]}")
    tag, sequence, end = der(data, 0)
    fields, cursor = [], 0
    while cursor < len(sequence):
        _, body, cursor = der(sequence, cursor)
        fields.append(body)
    if tag != 0x30 or end != len(data) or len(fields) != 4 or fields[0] != b"IM4P":
        raise SystemExit(f"{member} is not an IM4P")
    if fields[1] != (b"trst" if kind == "static" else b"trcs"):
        raise SystemExit(f"{member} carries {fields[1]!r}, not the expected type")
    return fields[3]


def modules_of(bundle: bytes) -> list[bytes]:
    """Split a bundle into module byte ranges."""
    count = struct.unpack_from("<I", bundle)[0]
    if not 0 < count <= 128 or len(bundle) < 4 + 4 * count:
        raise SystemExit("not a trust bundle")
    starts = [struct.unpack_from("<I", bundle, 4 + 4 * i)[0] for i in range(count)]
    return [bundle[starts[i]:(starts[i + 1] if i + 1 < count else len(bundle))] for i in range(count)]


def valid_like_the_loader(data: bytes) -> bool:
    """The structural checks of drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c ValidTrustCaches."""
    count = struct.unpack_from("<I", data)[0]
    if not 0 < count <= 128:
        return False
    for index in range(count):
        start = struct.unpack_from("<I", data, 4 + 4 * index)[0]
        end = struct.unpack_from("<I", data, 8 + 4 * index)[0] if index + 1 < count else len(data)
        if start < 4 + 4 * count or start & 3 or end > len(data) or end - start < 24:
            return False
        if struct.unpack_from("<I", data, start)[0] != 2:
            return False
        entries = struct.unpack_from("<I", data, start + 20)[0]
        if end - start != 24 + entries * 24:
            return False
        for entry in range(entries):
            at = start + 24 + entry * 24
            if not 1 <= data[at + 20] <= 4:
                return False
            if entry and data[at - 24:at - 4] >= data[at:at + 20]:
                return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--own", type=Path, default=DEFAULT_OWN)
    parser.add_argument("--static", type=Path, default=DEFAULT_STATIC)
    parser.add_argument("--system", type=Path, default=DEFAULT_SYSTEM)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()

    own_modules = modules_of(args.own.read_bytes())
    if len(own_modules) != 1:
        raise SystemExit(f"{args.own} carries {len(own_modules)} modules, not the single owner module")
    own = own_modules[0]
    own_entries = struct.unpack_from("<I", own, 20)[0]
    if len(own) != 24 + own_entries * 24:
        raise SystemExit("the owner module is not a complete version-2 module")
    own_hashes = [own[24 + 24 * i:24 + 24 * i + 20] for i in range(own_entries)]

    static = im4p_payload(args.static, "static")
    system = im4p_payload(args.system, "system")
    for module, digest, label in ((static, STATIC_SHA256, "StaticTrustCache"),
                                  (system, SYSTEM_SHA256, "Cryptex1,SystemTrustCache")):
        if hashlib.sha256(module).hexdigest() != digest:
            raise SystemExit(f"the {label} module differs from the pinned payload")
        if struct.unpack_from("<I", module)[0] != 2 or len(module) != 24 + struct.unpack_from("<I", module, 20)[0] * 24:
            raise SystemExit(f"the {label} module is not a complete version-2 module")

    modules = [own, static, system]
    cursor, starts = 16, []
    for module in modules:
        starts.append(cursor)
        cursor += len(module)
    bundle = struct.pack("<4I", 3, *starts) + b"".join(modules)
    if not valid_like_the_loader(bundle):
        raise SystemExit("the merged bundle would be refused by the boot loader")

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "trustcache.bundle").write_bytes(bundle)
    digest = hashlib.sha256(bundle).hexdigest()
    (args.out / "report.json").write_text(json.dumps(dict(
        purpose="boot trust cache: clover owner module, then the two original Apple modules",
        bundle_sha256=digest,
        bundle_bytes=len(bundle),
        modules=3,
        own_cdhashes=[h.hex() for h in own_hashes],
        static_entries=struct.unpack_from("<I", static, 20)[0],
        system_entries=struct.unpack_from("<I", system, 20)[0],
        static_sha256=STATIC_SHA256,
        system_sha256=SYSTEM_SHA256,
        original_modules_modified=False,
        apple_trust_chain_verified=False,
        board_toml_line=f'trust_cache_sha256 = ["{digest}"]',
    ), indent=2) + "\n")
    print(f"trustcache.bundle {len(bundle)} bytes sha256 {digest}")
    print(f"  modules: owner({own_entries}) static({struct.unpack_from('<I', static, 20)[0]}) "
          f"system({struct.unpack_from('<I', system, 20)[0]})")
    print(f"board.toml [payload]: trust_cache_sha256 = [\"{digest}\"]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
