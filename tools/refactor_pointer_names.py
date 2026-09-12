#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = ROOT / "build"
COMPILE_COMMANDS = BUILD_DIR / "compile_commands.json"
LIBCLANG_CANDIDATES = [
    Path("/opt/homebrew/Cellar/llvm/20.1.5/lib/libclang.dylib"),
    Path("/opt/homebrew/opt/llvm/lib/libclang.dylib"),
]
PYTHON_BINDING_CANDIDATES = [
    Path("/opt/homebrew/Cellar/llvm/20.1.5/lib/python3.12/site-packages"),
    Path("/opt/homebrew/Cellar/llvm/20.1.5/lib/python3.13/site-packages"),
    Path("/opt/homebrew/opt/llvm/lib/python3.12/site-packages"),
]
PROJECT_ROOTS = (ROOT / "ZenCore", ROOT / "ZenSamples")
PROJECT_ROOT_STRS = tuple(str(root) for root in PROJECT_ROOTS)
EXCLUDED_DIRS = {"External", "build", "bin", ".git", ".idea"}
REFERENCE_KINDS = {
    "DECL_REF_EXPR",
    "MEMBER_REF_EXPR",
    "MEMBER_REF",
    "UNEXPOSED_EXPR",
    "FIELD_DECL",
    "PARM_DECL",
    "VAR_DECL",
}
FUNCTION_LIKE_KINDS = {
    "FUNCTION_DECL",
    "CXX_METHOD",
    "CONSTRUCTOR",
    "DESTRUCTOR",
    "FUNCTION_TEMPLATE",
    "CONVERSION_FUNCTION",
}


@dataclass(frozen=True)
class Target:
    usr: str
    category: str
    old_name: str
    new_name: str
    path: Path
    line: int
    column: int


@dataclass(frozen=True)
class Occurrence:
    path: Path
    line: int
    column: int
    spelling: str


def ensure_clang():
    for candidate in PYTHON_BINDING_CANDIDATES:
        if candidate.exists():
            sys.path.insert(0, str(candidate))
            break
    try:
        from clang import cindex  # type: ignore
    except ModuleNotFoundError as exc:
        raise SystemExit(
            "Unable to import clang Python bindings. "
            "Add LLVM's site-packages directory to PYTHONPATH."
        ) from exc

    for candidate in LIBCLANG_CANDIDATES:
        if candidate.exists():
            cindex.Config.set_library_file(str(candidate))
            return cindex
    raise SystemExit("Unable to locate libclang.dylib.")


def in_project(path: Path) -> bool:
    path_str = str(path)
    if not any(
        path_str == root_str or path_str.startswith(f"{root_str}/")
        for root_str in PROJECT_ROOT_STRS
    ):
        return False
    return not any(part in EXCLUDED_DIRS for part in path.parts)


def location_path(location_name: str, cache: dict[str, Path]) -> Path:
    normalized = os.path.normpath(location_name)
    path = cache.get(normalized)
    if path is None:
        path = Path(normalized)
        cache[normalized] = path
    return path


def load_compile_commands() -> list[dict]:
    if not COMPILE_COMMANDS.exists():
        raise SystemExit(f"Missing compile database: {COMPILE_COMMANDS}")
    return json.loads(COMPILE_COMMANDS.read_text())


def get_system_include_args() -> list[str]:
    cmd = ["/usr/bin/c++", "-E", "-Wp,-v", "-x", "c++", "/dev/null"]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
    lines = proc.stderr.splitlines() + proc.stdout.splitlines()

    args: list[str] = []
    capture = False
    for raw_line in lines:
        line = raw_line.strip()
        if line == "#include <...> search starts here:":
            capture = True
            continue
        if line == "End of search list.":
            break
        if not capture or not line:
            continue
        if line.endswith("(framework directory)"):
            args.extend(["-iframework", line.removesuffix("(framework directory)").rstrip()])
        else:
            args.extend(["-isystem", line])
    return args


def filtered_args(arguments: list[str], system_include_args: list[str]) -> list[str]:
    args: list[str] = []
    iterator = iter(arguments[1:])
    for arg in iterator:
        if arg in {"-c", "--"}:
            continue
        if arg == "-o":
            next(iterator, None)
            continue
        if arg.endswith((".cpp", ".cc", ".cxx", ".c")) and Path(arg).exists():
            continue
        args.append(arg)
    return args + system_include_args


def is_raw_pointer_type(cindex, type_obj) -> bool:
    kind = type_obj.kind
    if kind == cindex.TypeKind.POINTER:
        return True
    if kind in (cindex.TypeKind.CONSTANTARRAY, cindex.TypeKind.INCOMPLETEARRAY):
        return type_obj.element_type.kind == cindex.TypeKind.POINTER
    return False


def split_identifier_parts(name: str) -> list[str]:
    name = name.strip("_")
    if not name:
        return []

    if "_" in name:
        return [part for part in name.split("_") if part]

    parts = re.findall(r"[A-Z]+(?=[A-Z][a-z0-9]|$)|[A-Z]?[a-z0-9]+", name)
    return parts or [name]


def build_base_name(name: str) -> str:
    if name.startswith("m_p") and len(name) > 3 and name[3].isupper():
        name = name[3:]
    elif name.startswith("m_"):
        name = name[2:]

    if re.match(r"^p+(?=[A-Z])", name):
        name = re.sub(r"^p+(?=[A-Z])", "", name)

    parts = split_identifier_parts(name)
    if not parts:
        return ""
    return "".join(part[:1].upper() + part[1:] for part in parts)


def classify_decl(cindex, cursor) -> str | None:
    kind_name = cursor.kind.name
    if kind_name == "FIELD_DECL":
        parent = cursor.semantic_parent
        if parent is not None:
            parent_kind = parent.kind.name
            if parent_kind in {
                "CLASS_DECL",
                "CLASS_TEMPLATE",
                "CLASS_TEMPLATE_PARTIAL_SPECIALIZATION",
            }:
                return "class_field"
            if parent_kind == "STRUCT_DECL":
                return "struct_field"
        return "field"

    if kind_name == "PARM_DECL":
        return "param"

    if kind_name != "VAR_DECL":
        return None

    parent = cursor.semantic_parent
    if parent is not None and parent.kind.name in FUNCTION_LIKE_KINDS:
        return "local"
    return "global"


def compute_target_name(category: str, old_name: str, include_globals: bool) -> str | None:
    if category == "global" and not include_globals:
        return None

    if category == "class_field":
        if old_name.startswith("m_p") and len(old_name) > 3 and old_name[3].isupper():
            return None
        base = build_base_name(old_name)
        return f"m_p{base}" if base else None

    if category in {"struct_field", "field", "param", "local", "global"}:
        if old_name.startswith("p") and len(old_name) > 1 and old_name[1].isupper():
            return None
        base = build_base_name(old_name)
        return f"p{base}" if base else None

    return None


def target_from_decl(cindex, cursor, include_globals: bool, path_cache: dict[str, Path]) -> Target | None:
    spelling = cursor.spelling
    if not spelling:
        return None

    if cursor.kind.name not in {"FIELD_DECL", "PARM_DECL", "VAR_DECL"}:
        return None

    if not is_raw_pointer_type(cindex, cursor.type):
        return None

    location = cursor.location
    if not location.file:
        return None

    decl_path = location_path(location.file.name, path_cache)
    if not in_project(decl_path):
        return None

    category = classify_decl(cindex, cursor)
    if category is None:
        return None

    new_name = compute_target_name(category, spelling, include_globals)
    if not new_name or new_name == spelling:
        return None

    usr = cursor.get_usr() or f"{decl_path}:{location.line}:{location.column}:{spelling}"
    return Target(
        usr=usr,
        category=category,
        old_name=spelling,
        new_name=new_name,
        path=decl_path,
        line=location.line,
        column=location.column,
    )


def collect_targets_and_occurrences(include_globals: bool):
    cindex = ensure_clang()
    compile_commands = load_compile_commands()
    system_include_args = get_system_include_args()
    compilation_db = cindex.CompilationDatabase.fromDirectory(str(BUILD_DIR))
    index = cindex.Index.create()
    path_cache: dict[str, Path] = {}

    source_files = sorted(
        {
            Path(entry["file"]).resolve()
            for entry in compile_commands
            if in_project(Path(entry["file"]).resolve())
        }
    )

    targets: dict[str, Target] = {}
    occurrences: dict[str, set[Occurrence]] = defaultdict(set)
    parse_errors: list[str] = []

    for i, path in enumerate(source_files, start=1):
        commands = list(compilation_db.getCompileCommands(str(path)))
        if not commands:
            parse_errors.append(f"no compile command: {path}")
            continue

        args = filtered_args(list(commands[0].arguments), system_include_args)
        try:
            tu = index.parse(str(path), args=args)
        except Exception as exc:  # pragma: no cover - one-off tooling
            parse_errors.append(f"failed to parse {path}: {exc}")
            continue

        for diag in tu.diagnostics:
            if diag.severity >= diag.Error:
                parse_errors.append(f"{path}: {diag}")

        for cursor in tu.cursor.walk_preorder():
            location = cursor.location
            if not location.file:
                continue

            cursor_path = location_path(location.file.name, path_cache)
            if not in_project(cursor_path):
                continue

            spelling = cursor.spelling
            kind_name = cursor.kind.name

            target = target_from_decl(cindex, cursor, include_globals, path_cache)
            if target is not None:
                targets.setdefault(target.usr, target)
                occurrences[target.usr].add(
                    Occurrence(
                        path=cursor_path,
                        line=location.line,
                        column=location.column,
                        spelling=spelling,
                    )
                )

            referenced = getattr(cursor, "referenced", None)
            if (
                referenced is not None
                and kind_name in REFERENCE_KINDS
                and spelling
            ):
                target = target_from_decl(cindex, referenced, include_globals, path_cache)
                if target is None:
                    continue
                targets.setdefault(target.usr, target)
                occurrences[target.usr].add(
                        Occurrence(
                            path=cursor_path,
                            line=location.line,
                            column=location.column,
                            spelling=spelling,
                        )
                    )

        print(
            f"[{i:>3}/{len(source_files)}] parsed {path.relative_to(ROOT)} "
            f"targets={len(targets)}",
            flush=True,
        )

    return targets, occurrences, parse_errors


def compute_offset(text: str, line: int, column: int) -> int:
    current_line = 1
    offset = 0
    for chunk in text.splitlines(keepends=True):
        if current_line == line:
            return offset + column - 1
        offset += len(chunk)
        current_line += 1
    raise ValueError(f"Invalid location line={line} column={column}")


def build_replacements(
    targets: dict[str, Target], occurrences: dict[str, set[Occurrence]]
) -> dict[Path, list[tuple[int, str, str]]]:
    replacements: dict[Path, list[tuple[int, str, str]]] = defaultdict(list)
    file_cache: dict[Path, str] = {}

    for usr, target in targets.items():
        for occurrence in occurrences.get(usr, set()):
            if occurrence.spelling != target.old_name:
                continue
            text = file_cache.setdefault(occurrence.path, occurrence.path.read_text())
            offset = compute_offset(text, occurrence.line, occurrence.column)
            if text[offset : offset + len(target.old_name)] != target.old_name:
                continue
            replacements[occurrence.path].append((offset, target.old_name, target.new_name))

    for path, edits in replacements.items():
        unique = {}
        for offset, old, new in edits:
            unique[(offset, old)] = (offset, old, new)
        replacements[path] = sorted(unique.values(), reverse=True)
    return replacements


def apply_replacements(replacements: dict[Path, list[tuple[int, str, str]]]) -> Counter:
    counter: Counter = Counter()
    for path, edits in replacements.items():
        text = path.read_text()
        new_text = text
        for offset, old, new in edits:
            if new_text[offset : offset + len(old)] != old:
                raise RuntimeError(f"Text mismatch at {path}:{offset}: expected {old}")
            new_text = new_text[:offset] + new + new_text[offset + len(old) :]
            counter["replacements"] += 1
        if new_text != text:
            path.write_text(new_text)
            counter["files"] += 1
    return counter


def print_summary(
    targets: dict[str, Target],
    replacements: dict[Path, list[tuple[int, str, str]]],
    parse_errors: list[str],
):
    categories = Counter(target.category for target in targets.values())
    print("\nTarget counts:")
    for category, count in sorted(categories.items()):
        print(f"  {category}: {count}")

    print("\nSample renames:")
    for target in sorted(targets.values(), key=lambda item: (str(item.path), item.line, item.column))[:40]:
        rel = target.path.relative_to(ROOT)
        print(f"  {rel}:{target.line} {target.old_name} -> {target.new_name}")

    file_count = len(replacements)
    replacement_count = sum(len(edits) for edits in replacements.values())
    print(f"\nPlanned edits: {replacement_count} replacements across {file_count} files")

    if parse_errors:
        print("\nParse diagnostics:")
        for item in parse_errors[:50]:
            print(f"  {item}")
        if len(parse_errors) > 50:
            print(f"  ... {len(parse_errors) - 50} more")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Refactor raw pointer variable names to pXxx / m_pXxx."
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Write edits back to source files.",
    )
    parser.add_argument(
        "--include-globals",
        action="store_true",
        help="Also rename global and namespace-scope raw pointer variables.",
    )
    args = parser.parse_args()

    targets, occurrences, parse_errors = collect_targets_and_occurrences(
        include_globals=args.include_globals
    )
    replacements = build_replacements(targets, occurrences)
    print_summary(targets, replacements, parse_errors)

    if not args.apply:
        return 0

    result = apply_replacements(replacements)
    print(
        f"\nApplied {result['replacements']} replacements across {result['files']} files."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
