"""Inventory glTF assets and verify each import in an isolated native process.

This checks importer structure and finite geometry. It does not establish visual
fidelity; image comparisons or renderer smoke tests remain separate checks.
"""

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from decimal import Decimal
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time


RESULT_MARKER = "ZEN_GLTF_IMPORT_RESULT "
FAILURE_STATUSES = {"failed", "crashed", "timeout"}


def decode_json_number(token):
    value = Decimal(token)
    # JSON's decimal and exponent encodings can still represent schema integers.
    # Decide before float conversion, which can round a fractional value to an integer.
    if value.is_zero():
        return 0
    if value == value.to_integral_value():
        return int(value)
    return float(value)


def read_document(path):
    data = path.read_bytes()
    if path.suffix.lower() == ".glb":
        if len(data) < 20 or data[:4] != b"glTF":
            raise ValueError("Invalid GLB header")
        version, length = struct.unpack_from("<II", data, 4)
        if version != 2 or length != len(data):
            raise ValueError("Invalid GLB version or file length")
        json_length, chunk_type = struct.unpack_from("<II", data, 12)
        if chunk_type != 0x4E4F534A or 20 + json_length > len(data):
            raise ValueError("Invalid GLB JSON chunk")
        data = data[20:20 + json_length]
    return json.loads(data, parse_float=decode_json_number)


def find_high_uv_bindings(value, location="materials"):
    bindings = []
    if isinstance(value, dict):
        uv = value.get("texCoord")
        if isinstance(uv, int) and uv > 1:
            bindings.append({"location": location, "texcoord": uv})
        for key, child in value.items():
            bindings.extend(find_high_uv_bindings(child, f"{location}/{key}"))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            bindings.extend(find_high_uv_bindings(child, f"{location}/{index}"))
    return bindings


def inspect_asset(path, root):
    document = read_document(path)
    attributes = Counter()
    modes = Counter()
    primitive_count = 0
    morph_count = 0
    variant_count = 0
    vertex_count = 0
    for mesh in document.get("meshes", []):
        for primitive in mesh.get("primitives", []):
            if "KHR_materials_variants" in primitive.get("extensions", {}):
                variant_count += 1
            if primitive.get("targets"):
                morph_count += 1
            attributes.update(primitive.get("attributes", {}).keys())
            modes[str(primitive.get("mode", 4))] += 1
            position_index = primitive.get("attributes", {}).get("POSITION")
            if position_index is not None:
                count = document["accessors"][position_index]["count"]
                if count:
                    primitive_count += 1
                    vertex_count += count
    return {
        "asset": path.relative_to(root).as_posix(),
        "bytes": path.stat().st_size,
        "required_extensions": document.get("extensionsRequired", []),
        "used_extensions": document.get("extensionsUsed", []),
        "attributes": dict(attributes),
        "primitive_modes": dict(modes),
        "high_uv_bindings": find_high_uv_bindings(document.get("materials", [])),
        "expected": {
            "min_meshes": len(document.get("meshes", [])),
            "min_submeshes": primitive_count,
            "min_vertices": vertex_count,
            "min_morph_primitives": morph_count,
            "min_variant_primitives": variant_count,
            "min_material_variants": len(
                document.get("extensions", {}).get("KHR_materials_variants", {}).get("variants", [])
            ),
            "min_animation_channels": sum(
                len(animation.get("channels", [])) for animation in document.get("animations", [])
            ),
            "skins": len(document.get("skins", [])),
            "animations": len(document.get("animations", [])),
        },
    }


def verify_asset(entry, root, executable, timeout):
    started = time.monotonic()
    result = dict(entry)
    environment = os.environ.copy()
    try:
        process = subprocess.run(
            [str(executable), str(root / entry["asset"])],
            cwd=executable.parent,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
        )
        result["exit_code"] = process.returncode
        result["status"] = "passed" if process.returncode == 0 else "failed"
        if process.returncode not in (0, 1, 2):
            result["status"] = "crashed"
        output = process.stdout
        for line in output.splitlines():
            if line.startswith(RESULT_MARKER):
                result["imported"] = json.loads(line[len(RESULT_MARKER):])
            elif line.startswith("ZEN_GLTF_IMPORT_STATE "):
                result["imported_before_verification"] = json.loads(line[len("ZEN_GLTF_IMPORT_STATE "):])
            elif line.startswith("ZEN_GLTF_IMPORT_STAGE "):
                result["last_stage"] = line[len("ZEN_GLTF_IMPORT_STAGE "):]
        if process.returncode == 0:
            issues = []
            imported = result.get("imported", {})
            if not imported:
                issues.append("Importer did not emit a verification result")
            else:
                for key in ("skins", "animations"):
                    if imported[key] != entry["expected"][key]:
                        issues.append(f"{key}: {imported[key]} != {entry['expected'][key]}")
                for key in ("meshes", "submeshes", "vertices", "morph_primitives", "animation_channels",
                            "material_variants", "variant_primitives"):
                    if imported[key] < entry["expected"][f"min_{key}"]:
                        issues.append(
                            f"{key}: {imported[key]} < {entry['expected'][f'min_{key}']}"
                        )
            if issues:
                result["status"] = "failed"
                output += "\nStructural verification: " + "; ".join(issues)
        if result["status"] != "passed":
            result["output"] = output[-12000:]
    except subprocess.TimeoutExpired as error:
        result["status"] = "timeout"
        result["exit_code"] = None
        output = error.stdout or b""
        result["output"] = output.decode(errors="replace") if isinstance(output, bytes) else output
    result["seconds"] = round(time.monotonic() - started, 3)
    return result


def summarize(entries):
    required = Counter()
    used = Counter()
    attributes = Counter()
    modes = Counter()
    statuses = Counter()
    failures_by_extension = Counter()
    for entry in entries:
        required.update(entry.get("required_extensions", []))
        used.update(entry.get("used_extensions", []))
        attributes.update(entry.get("attributes", {}))
        modes.update(entry.get("primitive_modes", {}))
        statuses.update([entry.get("status", "inventory")])
        if entry.get("status") in FAILURE_STATUSES:
            failures_by_extension.update(entry.get("required_extensions", []))
    return {
        "assets": len(entries),
        "statuses": dict(statuses),
        "required_extensions": dict(sorted(required.items())),
        "used_extensions": dict(sorted(used.items())),
        "attributes": dict(sorted(attributes.items())),
        "primitive_modes": dict(sorted(modes.items())),
        "failures_by_required_extension": dict(sorted(failures_by_extension.items())),
    }


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, required=True, help="Corpus root directory")
    parser.add_argument("--importer", type=Path, help="GLTFCorpusImport executable; omit for inventory")
    parser.add_argument("--report", type=Path, required=True, help="JSON output file")
    parser.add_argument("--timeout", type=float, default=120, help="Seconds allowed per asset")
    parser.add_argument("--jobs", type=int, default=1, help="Concurrent isolated imports")
    parser.add_argument("--match", default="", help="Only paths containing this text")
    args = parser.parse_args()
    root = args.assets.resolve()
    paths = sorted(
        path for path in root.rglob("*")
        if path.suffix.lower() in {".gltf", ".glb"}
        and args.match.casefold() in path.relative_to(root).as_posix().casefold()
    )
    entries = [inspect_asset(path, root) for path in paths]
    if not entries:
        parser.error("No .gltf or .glb assets found")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.importer:
        executable = args.importer.resolve()
        if not executable.is_file():
            parser.error(f"Importer executable does not exist: {executable}")
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [
                pool.submit(verify_asset, entry, root, executable, args.timeout)
                for entry in entries
            ]
            entries = []
            for future in as_completed(futures):
                entry = future.result()
                entries.append(entry)
                print(f"[{len(entries)}/{len(paths)}] {entry['status']}: {entry['asset']}", flush=True)
    entries.sort(key=lambda entry: entry["asset"])
    report = {
        "corpus": str(root),
        "importer": str(args.importer.resolve()) if args.importer else None,
        "scope": "Import structure and finite geometry; visual fidelity is not verified",
        "summary": summarize(entries),
        "results": entries,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report["summary"], indent=2))
    return 1 if any(entry.get("status") in FAILURE_STATUSES for entry in entries) else 0


if __name__ == "__main__":
    raise SystemExit(main())
