#!/usr/bin/env python3
"""Check translated Markdown structure and local links.

Chinese is the source language. Translation-source commit markers are advisory:
changes to Chinese after that commit print warnings but do not fail CI.
"""

from __future__ import annotations

import re
import subprocess
import sys
from collections import Counter
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
LANGUAGES = ("zh", "en", "ja")
TRANSLATIONS = {
    "README.md": "README.zh-CN.md",
    "README.ja.md": "README.zh-CN.md",
}
SOURCE_MARKER = re.compile(
    r"<!-- Translation source: ([0-9a-f]{7,40}) \(([^)]+)\) -->"
)
LINK = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
HEADING = re.compile(r"^#{1,6}\s+(.+?)\s*$")
ANCHOR = re.compile(r'<a\s+id=["\x27]([^"\x27]+)["\x27]\s*></a>', re.IGNORECASE)
FENCE = re.compile(r"^\s*(`{3,}|~{3,})")
ANGLE_PLACEHOLDER = re.compile(r"<([^<>\n]+)>")
HAN = re.compile(r"[\u3400-\u9fff]")
# These simplified forms are not used in normal Japanese placeholders.
SIMPLIFIED_HAN = re.compile(r"[资录设备缓两签说类应对后为这们个码载开关进检测试编译]")
CHINESE_EXAMPLE_TERMS = (
    "返回类型",
    "名字_Hook",
    "名字_Orig",
    "ADD_HOOK(名字",
    "参数个数",
    "程序集",
    "命名空间",
    "类名",
    "方法名",
    "判断函数",
    "原始字节",
    "新字节",
    "之后的增量",
    "地址",
    "说明",
)


def markdown_lines(path: Path) -> list[tuple[int, str]]:
    """Return numbered lines outside fenced code blocks."""
    lines: list[tuple[int, str]] = []
    fence: str | None = None
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        match = FENCE.match(line)
        if match:
            marker = match.group(1)
            if fence is None:
                fence = marker
            elif marker[0] == fence[0] and len(marker) >= len(fence):
                fence = None
            continue
        if fence is None:
            lines.append((number, line))
    return lines


def heading_slug(heading: str) -> str:
    heading = re.sub(r"<[^>]*>", "", heading)
    heading = re.sub(r"!\[([^\]]*)\]\([^)]+\)", r"\1", heading)
    heading = re.sub(r"\[([^\]]*)\]\([^)]+\)", r"\1", heading)
    heading = re.sub(r"[^\w\- ]", "", heading.lower())
    return heading.replace(" ", "-")


def anchors(path: Path) -> set[str]:
    result: set[str] = set()
    counts: Counter[str] = Counter()
    for _, line in markdown_lines(path):
        result.update(ANCHOR.findall(line))
        match = HEADING.match(line)
        if match:
            slug = heading_slug(match.group(1).rstrip("#").strip())
            if slug:
                result.add(slug if counts[slug] == 0 else f"{slug}-{counts[slug]}")
                counts[slug] += 1
    return result


def check_file_parity(root: Path) -> list[str]:
    errors: list[str] = []
    for lang in LANGUAGES:
        if not (root / "docs" / lang).is_dir():
            errors.append(f"docs/{lang}: missing language directory")
    names = {
        lang: {p.name for p in (root / "docs" / lang).glob("*.md")}
        for lang in LANGUAGES
    }
    if not names["zh"]:
        errors.append("docs/zh: no source Markdown files")
    for lang in LANGUAGES:
        missing = sorted(names["zh"] - names[lang])
        extra = sorted(names[lang] - names["zh"])
        if missing:
            errors.append(f"docs/{lang}: missing {', '.join(missing)}")
        if extra:
            errors.append(f"docs/{lang}: extra {', '.join(extra)}")
    return errors


def check_links(root: Path) -> list[str]:
    paths = [root / name for name in ("README.md", "README.zh-CN.md", "README.ja.md")]
    paths += sorted((root / "docs").rglob("*.md"))
    errors: list[str] = []
    anchor_cache: dict[Path, set[str]] = {}
    for source in paths:
        if not source.is_file():
            errors.append(f"missing file: {source.relative_to(root)}")
            continue
        for line_number, line in markdown_lines(source):
            for match in LINK.finditer(line):
                raw = match.group(1).strip()
                target = raw[1:raw.index(">")] if raw.startswith("<") and ">" in raw else raw.split(" ", 1)[0]
                parsed = urlsplit(target)
                if parsed.scheme or parsed.netloc:
                    continue
                destination = (source.parent / unquote(parsed.path)).resolve() if parsed.path else source
                where = f"{source.relative_to(root)}:{line_number}"
                if not destination.is_relative_to(root.resolve()) or not destination.is_file():
                    errors.append(f"{where}: missing file: {target}")
                    continue
                if parsed.fragment:
                    found = anchor_cache.setdefault(destination, anchors(destination))
                    fragment = unquote(parsed.fragment)
                    if fragment not in found:
                        errors.append(f"{where}: missing anchor: {target}")
    return errors


def check_placeholders(root: Path) -> list[str]:
    """Reject Chinese example placeholders left in English or Japanese docs."""
    chinese = [root / "README.zh-CN.md", *sorted((root / "docs" / "zh").glob("*.md"))]
    source_placeholders = {
        token
        for path in chinese if path.is_file()
        for token in ANGLE_PLACEHOLDER.findall(path.read_text(encoding="utf-8"))
        if HAN.search(token)
    }
    errors: list[str] = []
    for lang, paths in (
        ("en", [root / "README.md", *sorted((root / "docs" / "en").glob("*.md"))]),
        ("ja", [root / "README.ja.md", *sorted((root / "docs" / "ja").glob("*.md"))]),
    ):
        for path in paths:
            if not path.is_file():
                continue
            fence: str | None = None
            for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                marker = FENCE.match(line)
                if marker:
                    current = marker.group(1)
                    if fence is None:
                        fence = current
                    elif current[0] == fence[0] and len(current) >= len(fence):
                        fence = None
                    continue
                where = f"{path.relative_to(root)}:{number}"
                for token in ANGLE_PLACEHOLDER.findall(line):
                    if (token in source_placeholders
                            or (lang == "en" and HAN.search(token))
                            or (lang == "ja" and SIMPLIFIED_HAN.search(token))):
                        errors.append(f"{where}: untranslated placeholder <{token}>")
                if fence is not None or "`" in line:
                    for term in CHINESE_EXAMPLE_TERMS:
                        if term in line:
                            errors.append(f"{where}: untranslated example term {term}")
    return errors


def git(*args: str, root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *args], cwd=root, text=True, capture_output=True, check=False
    )


def check_translation_sources(root: Path) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []
    translated = [root / name for name in TRANSLATIONS]
    translated += sorted((root / "docs" / lang / name)
                         for lang in ("en", "ja")
                         for name in {p.name for p in (root / "docs" / "zh").glob("*.md")})
    for path in translated:
        if not path.is_file():
            continue  # File parity reports this.
        relative = path.relative_to(root).as_posix()
        expected = (
            TRANSLATIONS[relative]
            if relative in TRANSLATIONS
            else f"docs/zh/{path.name}"
        )
        marker = SOURCE_MARKER.search(path.read_text(encoding="utf-8"))
        if not marker or marker.group(2) != expected:
            errors.append(f"{relative}: expected Translation source marker for {expected}")
            continue
        commit = marker.group(1)
        if git("rev-parse", "--verify", f"{commit}^{{commit}}", root=root).returncode:
            warnings.append(f"{relative}: source commit {commit[:8]} unavailable; cannot check translation age")
            continue
        changed = git("diff", "--quiet", commit, "--", expected, root=root)
        if changed.returncode == 1:
            warnings.append(f"{relative}: {expected} changed after {commit[:8]}; review translation")
        elif changed.returncode != 0:
            warnings.append(f"{relative}: could not compare {expected} with {commit[:8]}")
    return errors, warnings


def main() -> int:
    errors = check_file_parity(ROOT) + check_links(ROOT) + check_placeholders(ROOT)
    source_errors, warnings = check_translation_sources(ROOT)
    errors.extend(source_errors)
    for message in errors:
        print(f"ERROR: {message}")
    for message in warnings:
        print(f"WARNING: {message}")
    if errors:
        print(f"Documentation check failed: {len(errors)} error(s), {len(warnings)} warning(s)")
        return 1
    print(f"Documentation check passed: 3 locales, {len(warnings)} translation warning(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
