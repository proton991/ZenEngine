"""Reject deliberate C++ exception handling in ZenEngine-owned source files."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
TOKENS = re.compile(
    r'//[^\n]*|/\*[\s\S]*?\*/|R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\([\s\S]*?\)(?P=delimiter)"'
    r'|"(?:\\.|[^"\\])*"|\x27(?:\\.|[^\x27\\])*\x27'
    r'|\b(?:throw|try|catch|LOG_ERROR_AND_THROW|EXPECT_THROW|ASSERT_THROW|EXPECT_NO_THROW|ASSERT_NO_THROW)\b'
)
FORBIDDEN = {"throw", "try", "catch", "LOG_ERROR_AND_THROW", "EXPECT_THROW", "ASSERT_THROW", "EXPECT_NO_THROW", "ASSERT_NO_THROW"}


def main():
    failures = []
    count = 0
    for directory in ("ZenCore", "ZenSamples", "ZenUI"):
        for path in sorted((ROOT / directory).rglob("*")):
            if path.suffix not in {".h", ".hpp", ".cpp", ".inl", ".cc", ".cxx"}:
                continue
            count += 1
            text = path.read_text(encoding="utf-8-sig")
            for token in TOKENS.finditer(text):
                if token.group() in FORBIDDEN:
                    line = text.count("\n", 0, token.start()) + 1
                    failures.append(f"{path.relative_to(ROOT)}:{line}: {token.group()}")
    print("\n".join(failures) if failures else f"Checked {count} owned C++ files: no exception syntax or exception test macros.")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
