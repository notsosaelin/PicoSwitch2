#!/usr/bin/env python3
"""Validate raw/extended NTAG215 amiibo images without modifying them."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from urllib.request import Request, urlopen


def validate(path: Path) -> tuple[str | None, dict[str, str | int] | None]:
    data = path.read_bytes()
    if len(data) not in (540, 572):
        return f"unsupported size {len(data)}", None
    bcc0 = 0x88 ^ data[0] ^ data[1] ^ data[2]
    bcc1 = data[4] ^ data[5] ^ data[6] ^ data[7]
    if data[3] != bcc0 or data[8] != bcc1:
        return "invalid UID/BCC", None
    uid = bytes((data[0], data[1], data[2], data[4],
                 data[5], data[6], data[7])).hex().upper()
    return None, {
        "uid": uid,
        "amiibo_id": data[0x54:0x5C].hex().upper(),
        "variant": data[0x5C:0x60].hex().upper(),
        "product_type": data[0x57],
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check every .bin under a file or directory for PicoSwitch2 compatibility.")
    parser.add_argument("path", type=Path)
    parser.add_argument("--json", action="store_true", dest="as_json")
    parser.add_argument(
        "--amiiboapi",
        action="store_true",
        help="download the full public AmiiboAPI catalog once and report local exact-ID matches")
    args = parser.parse_args()

    root = args.path
    files = [root] if root.is_file() else sorted(root.rglob("*.bin"))
    failures: list[dict[str, str]] = []
    sizes: Counter[int] = Counter()
    uids: list[str] = []
    amiibo_ids: list[str] = []
    variants: list[str] = []
    product_types: Counter[int] = Counter()
    for path in files:
        sizes[path.stat().st_size] += 1
        error, identity = validate(path)
        if error:
            failures.append({"path": str(path), "error": error})
        elif identity:
            uids.append(str(identity["uid"]))
            amiibo_ids.append(str(identity["amiibo_id"]))
            variants.append(str(identity["variant"]))
            product_types[int(identity["product_type"])] += 1

    result = {
        "root": str(root),
        "files": len(files),
        "valid": len(files) - len(failures),
        "invalid": len(failures),
        "sizes": dict(sorted(sizes.items())),
        "unique_uids": len(set(uids)),
        "unique_amiibo_ids": len(set(amiibo_ids)),
        "unique_variants": len(set(variants)),
        "product_types": {f"0x{code:02X}": count
                          for code, count in sorted(product_types.items())},
        "failures": failures,
    }
    if args.amiiboapi:
        request = Request(
            "https://amiiboapi.org/api/amiibo/",
            headers={"User-Agent": "PicoSwitch2-collection-validator/1"})
        with urlopen(request, timeout=30) as response:
            catalog_body = json.load(response)
        catalog_entries = catalog_body.get("amiibo", [])
        catalog_ids = {
            f"{entry.get('head', '')}{entry.get('tail', '')}".upper()
            for entry in catalog_entries
        }
        matched_files = sum(amiibo_id in catalog_ids for amiibo_id in amiibo_ids)
        unmatched_ids = sorted(set(amiibo_ids) - catalog_ids)
        result["amiiboapi"] = {
            "catalog_entries": len(catalog_entries),
            "catalog_ids": len(catalog_ids),
            "matched_files": matched_files,
            "unmatched_files": len(amiibo_ids) - matched_files,
            "unmatched_ids": unmatched_ids,
        }
    if args.as_json:
        print(json.dumps(result, indent=2))
    else:
        print(f"files={result['files']} valid={result['valid']} "
              f"invalid={result['invalid']} unique_uids={result['unique_uids']} "
              f"unique_amiibo_ids={result['unique_amiibo_ids']} "
              f"unique_variants={result['unique_variants']}")
        print("sizes=" + ", ".join(f"{size}:{count}"
                                  for size, count in result["sizes"].items()))
        print("product_types=" + ", ".join(
            f"{code}:{count}" for code, count in result["product_types"].items()))
        if "amiiboapi" in result:
            catalog = result["amiiboapi"]
            print(
                f"amiiboapi_entries={catalog['catalog_entries']} "
                f"matched_files={catalog['matched_files']} "
                f"unmatched_files={catalog['unmatched_files']} "
                f"unmatched_ids={','.join(catalog['unmatched_ids']) or 'none'}")
        for failure in failures:
            print(f"INVALID {failure['path']}: {failure['error']}")
    return 1 if failures or not files else 0


if __name__ == "__main__":
    raise SystemExit(main())
