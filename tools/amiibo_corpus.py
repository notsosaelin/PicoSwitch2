#!/usr/bin/env python3
"""Structurally classify a directory of amiibo dumps before hardware is involved.

Why this exists
---------------
The v3 (NTAG I2C Plus 2K) investigation lost hardware iterations to a single
assumption: that one physical figure was representative. Six "genuine" captures
were byte-identical because they came from the same Warp Star, which made a
tag-specific SRAM CRC look like a controller constant. Every dump needed to
answer that question was already on disk.

This tool answers the static questions offline so console runs are spent on
what only a console can tell us:

* is the image structurally valid for the runtime, and why not;
* which images are actually distinct, and along which axis;
* which images are unwritten, which carry console-written game state;
* where a written image put its allocation-relative capability page.

It never mutates a dump, never writes key material, and never needs a console.
Cryptographic validity is delegated to ``tools/verify_amiibo_crypto.mjs`` so
there is exactly one amiitool port in the repository (the portal's).

Usage
-----
    python tools/amiibo_corpus.py <file-or-directory>
    python tools/amiibo_corpus.py <dir> --retail-key <path> --json build/corpus.json

The retail key is optional, is only forwarded to the verifier, and never
appears in the manifest. Only the standard library is required; ``--retail-key``
additionally needs ``node`` on PATH.

Exit status: 0 when every image is runtime-safe, 1 when any image is not,
2 on a usage or I/O error.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable
from urllib.request import Request, urlopen

# Keep these in sync with include/ns2_amiibo_v3.h and
# include/ns2_amiibo_v3_write.h. They are duplicated rather than parsed because
# this tool must run without a build tree, but any change to the C constants
# must be mirrored here; tools/test_amiibo_corpus.py asserts the ones that are
# observable in the captured corpus.
V3_SIZE = 2048
V3_SRAM_OFFSET = 0x3C0
V3_SRAM_SIZE = 64
V3_SRAM_DATA_SIZE = 62
V3_SECTOR1_OFFSET = 0x400
V3_WRITE_END = 0x248
# The 355-byte clear operation proves sector-0 pages 0x92..0xE1 are writable.
V3_SECTOR0_RECORD_FIRST_PAGE = 0x92
V3_SECTOR0_RECORD_LAST_PAGE = 0xE1
V3_SECTOR0_RECORD_SIZE = 0x20
V3_SECTOR1_RECORD_SIZE = 0x60

NTAG215_SIZES = (540, 572)
IDENTITY_OFFSET = 0x54
IDENTITY_SIZE = 8
VARIANT_OFFSET = 0x5C
VARIANT_SIZE = 4

AMIIBOAPI_MIRRORS = (
    "https://amiiboapi.org/api/amiibo/",
    "https://www.amiiboapi.com/api/amiibo/",
)


class CorpusError(RuntimeError):
    """The corpus could not be read or classified."""


def crc16_mcrf4xx(data: bytes) -> int:
    """Mirror of ns2_amiibo_v3_crc16_mcrf4xx in src/nfc/ns2_amiibo_v3.c."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x8408 if crc & 1 else 0)
    return crc


def nonzero_runs(data: bytes, base: int = 0, gap: int = 16) -> list[tuple[int, int]]:
    """Coalesce nonzero bytes into ``(start, end_inclusive)`` absolute ranges.

    Short zero gaps are absorbed so a 96-byte record with internal zero bytes
    reads as one allocation rather than a dozen fragments.
    """
    runs: list[list[int]] = []
    for index, value in enumerate(data):
        if value == 0:
            continue
        position = base + index
        if runs and position - runs[-1][1] <= gap + 1:
            runs[-1][1] = position
        else:
            runs.append([position, position])
    return [(start, end) for start, end in runs]


def _hex(data: bytes) -> str:
    return data.hex().upper()


@dataclass
class Image:
    """One classified dump. ``issues`` being empty is the runtime-safe test."""

    path: Path
    relative: str
    data: bytes = field(repr=False)
    issues: list[str] = field(default_factory=list)
    fields: dict[str, Any] = field(default_factory=dict)

    @property
    def safe(self) -> bool:
        return not self.issues


def classify_ntag215(image: Image) -> None:
    data = image.data
    bcc0 = 0x88 ^ data[0] ^ data[1] ^ data[2]
    bcc1 = data[4] ^ data[5] ^ data[6] ^ data[7]
    if data[3] != bcc0 or data[8] != bcc1:
        image.issues.append("invalid UID/BCC interleave")
    uid = bytes((data[0], data[1], data[2], data[4], data[5], data[6], data[7]))
    image.fields.update({
        "format": "ntag215" if len(data) == 540 else "ntag215_extended",
        "uid": _hex(uid),
        "amiibo_id": _hex(data[IDENTITY_OFFSET:IDENTITY_OFFSET + IDENTITY_SIZE]),
        "variant": _hex(data[VARIANT_OFFSET:VARIANT_OFFSET + VARIANT_SIZE]),
        "product_type": data[0x57],
    })


def v3_capability_pages(data: bytes) -> list[dict[str, int]]:
    """Sector-1 pages holding a valid chip-managed ``A5 00 gg 00`` marker.

    The generation byte is what a genuine controller advances on every Air
    Riders write. Its page is allocation-relative -- Kirby uses sector-1 page 0,
    King Dedede page 0x64 -- so this is discovered, never assumed.
    """
    found: list[dict[str, int]] = []
    for page in range(256):
        offset = V3_SECTOR1_OFFSET + page * 4
        marker = data[offset:offset + 4]
        if len(marker) == 4 and marker[0] == 0xA5 and marker[1] == 0x00 \
                and marker[2] != 0x00 and marker[3] == 0x00:
            found.append({"page": page, "generation": marker[2]})
    return found


def v3_sector0_record_pages(data: bytes) -> list[int]:
    """Start pages of written records inside the proven-writable window.

    A sliding 32-byte window would report eight overlapping pages for one
    32-byte record. What matters is where each contiguous written run begins,
    because that is the allocation the console's envelope will name.
    """
    start = V3_SECTOR0_RECORD_FIRST_PAGE * 4
    end = (V3_SECTOR0_RECORD_LAST_PAGE + 1) * 4
    return [first // 4 for first, _ in nonzero_runs(data[start:end], start, gap=3)]


def classify_v3(image: Image) -> None:
    data = image.data
    if data[0] != 0x04 or data[7] != 0x00 or data[8] != 0x44:
        image.issues.append(
            f"not an NTAG I2C 2K header: [0]={data[0]:#04x} [7]={data[7]:#04x} "
            f"[8]={data[8]:#04x} (expected 0x04/0x00/0x44)")

    sram = data[V3_SRAM_OFFSET:V3_SRAM_OFFSET + V3_SRAM_SIZE]
    stored_crc = int.from_bytes(sram[V3_SRAM_DATA_SIZE:V3_SRAM_DATA_SIZE + 2], "big")
    expected_crc = crc16_mcrf4xx(sram[:V3_SRAM_DATA_SIZE])

    capability = v3_capability_pages(data)
    sector0_records = v3_sector0_record_pages(data)
    body_written = any(data[0x14:V3_WRITE_END])

    image.fields.update({
        "format": "v3",
        "uid": _hex(data[0:7]),
        "amiibo_id": _hex(data[IDENTITY_OFFSET:IDENTITY_OFFSET + IDENTITY_SIZE]),
        "variant": _hex(data[VARIANT_OFFSET:VARIANT_OFFSET + VARIANT_SIZE]),
        "product_type": data[0x57],
        "sram": _hex(sram),
        "sram_crc": f"{stored_crc:04X}",
        "sram_crc_expected": f"{expected_crc:04X}",
        "sram_crc_valid": stored_crc == expected_crc,
        "capability_pages": capability,
        "sector0_record_pages": [f"0x{page:02X}" for page in sector0_records],
        "body_written": body_written,
        "nonzero_ranges": [
            f"0x{start:03X}-0x{end:03X}" for start, end in nonzero_runs(data)
        ],
    })

    # An unwritten ecosystem dump has no capability marker at all; that is
    # normal and must not be reported as unsafe. More than one marker means the
    # allocation cannot be inferred statically, which the runtime resolves from
    # the console's own envelope -- flag it so it is never silently guessed.
    if len(capability) > 1:
        image.fields["allocation"] = "ambiguous"
    elif not capability and sector0_records:
        # Records were written but the chip-managed capability page was not
        # retained. Genuine hardware always advances it, so this shape is a
        # known regression signature: the tag reads back once and then fails
        # its next reuse. Recorded, not treated as unsafe, because these are
        # real historical outputs worth keeping analyzable.
        image.fields["allocation"] = "indeterminate"
        image.fields.setdefault("notes", []).append(
            "sector-0 records present with no retained A5 00 gg 00 capability "
            "page; reuse will not match genuine behavior")
    elif capability:
        page = capability[0]["page"]
        image.fields["allocation"] = {
            "sector1_capability_page": f"0x{page:02X}",
            "sector1_data_page": f"0x{page + 1:02X}",
            "generation": capability[0]["generation"],
        }
        if page + 1 + (V3_SECTOR1_RECORD_SIZE // 4) - 1 > 0xFF:
            image.issues.append(
                f"sector-1 record at page 0x{page + 1:02X} overruns the sector")
    else:
        image.fields["allocation"] = "unwritten"

    image.fields["extended_state"] = (
        "written" if capability or sector0_records
        else ("body-written" if body_written else "unwritten"))


def load_image(path: Path, root: Path) -> Image:
    data = path.read_bytes()
    try:
        relative = str(path.relative_to(root)).replace("\\", "/")
    except ValueError:
        relative = path.name
    image = Image(path=path, relative=relative, data=data)
    image.fields.update({
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest().upper(),
        "crc32": f"{zlib.crc32(data) & 0xFFFFFFFF:08X}",
    })
    if len(data) in NTAG215_SIZES:
        classify_ntag215(image)
    elif len(data) == V3_SIZE:
        classify_v3(image)
    else:
        # A dump directory legitimately holds SPI images and controller dumps.
        # Those are skipped, not failed, so the exit code keeps meaning "an
        # amiibo image in this corpus is unsafe for the runtime".
        image.fields["format"] = "unsupported"
        image.fields["skipped"] = f"size {len(data)} is not 540, 572, or 2048"
    return image


def group_images(images: Iterable[Image]) -> None:
    """Assign equivalence-group ids along each independently varying axis.

    v3 rider identity lives in the encrypted body and machine identity lives
    entirely in the SRAM window, so two dumps can be "the same amiibo" on one
    axis and different on the other. Collapsing them into a single hash is what
    made four distinct Kirby machines look like one figure.
    """
    body: dict[str, int] = {}
    sram: dict[str, int] = {}
    identity: dict[str, int] = {}
    for image in images:
        if image.fields.get("format") == "v3":
            body_key = hashlib.sha256(
                image.data[:V3_SRAM_OFFSET] +
                image.data[V3_SRAM_OFFSET + V3_SRAM_SIZE:]).hexdigest()
            sram_key = hashlib.sha256(
                image.data[V3_SRAM_OFFSET:V3_SRAM_OFFSET + V3_SRAM_SIZE]).hexdigest()
            image.fields["body_group"] = body.setdefault(body_key, len(body))
            image.fields["sram_group"] = sram.setdefault(sram_key, len(sram))
        amiibo_id = image.fields.get("amiibo_id")
        if amiibo_id:
            image.fields["identity_group"] = identity.setdefault(amiibo_id, len(identity))


def verify_crypto(root: Path, key: Path, images: list[Image]) -> dict[str, Any]:
    """Delegate HMAC validation to the portal's amiitool port.

    Reimplementing amiibo crypto here would create a second implementation that
    can drift from the one the portal actually uses, which is exactly how a
    "the portal accepts it" claim stops predicting console behavior.
    """
    node = shutil.which("node")
    if node is None:
        raise CorpusError("--retail-key needs node on PATH for tools/verify_amiibo_crypto.mjs")
    script = Path(__file__).with_name("verify_amiibo_crypto.mjs")
    if not script.is_file():
        raise CorpusError(f"missing {script}")
    completed = subprocess.run(
        [node, str(script), str(root), str(key), "--json"],
        capture_output=True, text=True, check=False)
    if completed.returncode not in (0, 1):
        raise CorpusError(
            f"verify_amiibo_crypto.mjs failed ({completed.returncode}): "
            f"{completed.stderr.strip() or completed.stdout.strip()}")
    try:
        report = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise CorpusError(f"verify_amiibo_crypto.mjs did not emit JSON: {exc}") from exc

    by_path = {Path(entry["path"]).resolve(): entry for entry in report.get("images", [])}
    verified = invalid = 0
    for image in images:
        entry = by_path.get(image.path.resolve())
        if entry is None:
            continue
        image.fields["hmac_valid"] = bool(entry.get("ok"))
        if entry.get("nickname") or entry.get("owner"):
            image.fields["register_info"] = {
                "nickname": entry.get("nickname", ""),
                "owner": entry.get("owner", ""),
            }
        if entry.get("ok"):
            verified += 1
        else:
            invalid += 1
            # A failing HMAC is a fact about the dump, not about the runtime:
            # the console rejects it, but PicoSwitch2 will still serve it.
            # Report it without making the corpus exit non-zero on its own.
            image.fields.setdefault("notes", []).append("HMAC invalid")
    return {"checked": verified + invalid, "valid": verified, "invalid": invalid}


def match_catalog(images: list[Image]) -> dict[str, Any]:
    """Enhancement only: a catalog miss never invalidates a dump."""
    entries: list[dict[str, Any]] = []
    errors: list[str] = []
    for url in AMIIBOAPI_MIRRORS:
        try:
            request = Request(url, headers={"User-Agent": "PicoSwitch2-amiibo-corpus/1"})
            with urlopen(request, timeout=30) as response:
                entries = json.load(response).get("amiibo", [])
            break
        except Exception as exc:  # noqa: BLE001 - any mirror failure is non-fatal
            errors.append(f"{url}: {exc}")
    if not entries:
        return {"available": False, "errors": errors}

    catalog = {
        f"{entry.get('head', '')}{entry.get('tail', '')}".upper(): entry.get("name", "")
        for entry in entries
    }
    matched = 0
    for image in images:
        name = catalog.get(str(image.fields.get("amiibo_id", "")).upper())
        if name:
            image.fields["catalog_name"] = name
            matched += 1
    return {"available": True, "entries": len(catalog), "matched": matched}


def build_manifest(root: Path, images: list[Image], extras: dict[str, Any]) -> dict[str, Any]:
    formats: dict[str, int] = {}
    allocations: set[str] = set()
    for image in images:
        formats[str(image.fields.get("format"))] = \
            formats.get(str(image.fields.get("format")), 0) + 1
        allocation = image.fields.get("allocation")
        if isinstance(allocation, dict):
            allocations.add(allocation["sector1_capability_page"])

    duplicate_uids: dict[str, list[str]] = {}
    for image in images:
        uid = image.fields.get("uid")
        if uid:
            duplicate_uids.setdefault(str(uid), []).append(image.relative)

    manifest = {
        "tool": "amiibo_corpus",
        "version": 1,
        "root": str(root),
        "images": [
            {"path": image.relative, "safe": image.safe, "issues": image.issues,
             **image.fields}
            for image in sorted(images, key=lambda item: item.relative)
        ],
        "summary": {
            "images": len(images),
            "skipped": sum(1 for image in images if "skipped" in image.fields),
            "runtime_safe": sum(1 for image in images
                                if image.safe and "skipped" not in image.fields),
            "unsafe": sum(1 for image in images if not image.safe),
            "formats": dict(sorted(formats.items())),
            "body_groups": len({image.fields["body_group"] for image in images
                                if "body_group" in image.fields}),
            "sram_groups": len({image.fields["sram_group"] for image in images
                                if "sram_group" in image.fields}),
            "identity_groups": len({image.fields["identity_group"] for image in images
                                    if "identity_group" in image.fields}),
            "extended_allocations": sorted(allocations),
            "shared_uids": {uid: paths for uid, paths in sorted(duplicate_uids.items())
                            if len(paths) > 1},
            **extras,
        },
    }
    return manifest


def render(manifest: dict[str, Any]) -> str:
    summary = manifest["summary"]
    lines = [
        f"{summary['images']} images under {manifest['root']}",
        "  formats: " + ", ".join(f"{name}={count}"
                                  for name, count in summary["formats"].items()),
        f"  runtime-safe: {summary['runtime_safe']}  unsafe: {summary['unsafe']}"
        + (f"  skipped: {summary['skipped']}" if summary["skipped"] else ""),
    ]
    if summary["identity_groups"]:
        lines.append(
            f"  distinct: {summary['identity_groups']} identities, "
            f"{summary['body_groups']} encrypted-body groups, "
            f"{summary['sram_groups']} machine-SRAM groups")
    if summary["extended_allocations"]:
        lines.append("  extended allocations: " +
                     ", ".join(summary["extended_allocations"]))
    if "crypto" in summary:
        crypto = summary["crypto"]
        lines.append(f"  HMAC: {crypto['valid']} valid, {crypto['invalid']} invalid "
                     f"of {crypto['checked']} checked")
    for uid, paths in summary["shared_uids"].items():
        # Not an error: four machine variants of one physical figure legitimately
        # share a UID. It is a warning against treating them as independent
        # samples of a "constant".
        lines.append(f"  NOTE uid {uid} is shared by {len(paths)} images: "
                     + ", ".join(paths))
    for image in manifest["images"]:
        if not image["safe"]:
            lines.append(f"UNSAFE {image['path']}: " + "; ".join(image["issues"]))
            continue
        if image.get("hmac_valid") is False:
            lines.append(f"HMAC-INVALID {image['path']} (structurally serviceable)")
        for note in image.get("notes", []):
            lines.append(f"NOTE {image['path']}: {note}")
    return "\n".join(lines)


def collect(target: Path) -> list[Path]:
    if target.is_file():
        return [target]
    if not target.is_dir():
        raise CorpusError(f"no such file or directory: {target}")
    return sorted(target.rglob("*.bin"))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("path", type=Path, help="a .bin file or a directory tree of them")
    parser.add_argument("--retail-key", type=Path,
                        help="verify HMACs via tools/verify_amiibo_crypto.mjs; "
                             "used in memory only and never written to the manifest")
    parser.add_argument("--json", type=Path, dest="json_out",
                        help="write the full manifest here")
    parser.add_argument("--amiiboapi", action="store_true",
                        help="annotate catalog names (enhancement only, never gating)")
    parser.add_argument("--quiet", action="store_true", help="suppress the text summary")
    args = parser.parse_args(argv)

    try:
        root = args.path if args.path.is_dir() else args.path.parent
        files = collect(args.path)
        if not files:
            raise CorpusError(f"no .bin files under {args.path}")
        images = [load_image(path, root) for path in files]
        group_images(images)

        extras: dict[str, Any] = {}
        if args.retail_key:
            extras["crypto"] = verify_crypto(args.path, args.retail_key, images)
        if args.amiiboapi:
            extras["catalog"] = match_catalog(images)

        manifest = build_manifest(root, images, extras)
        if args.json_out:
            args.json_out.parent.mkdir(parents=True, exist_ok=True)
            args.json_out.write_text(json.dumps(manifest, indent=2) + "\n",
                                     encoding="utf-8")
        if not args.quiet:
            print(render(manifest))
    except (CorpusError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    return 0 if manifest["summary"]["unsafe"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
