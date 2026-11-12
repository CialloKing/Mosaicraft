#!/usr/bin/env python3
"""Check local documentation links, GitHub-style anchors, and index coverage."""

import argparse
from collections import Counter
from html import unescape
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit


def prose(text):
    """Ignore examples while retaining line numbers for actionable diagnostics."""
    fence = None
    lines = []
    for line in text.splitlines():
        marker = re.match(r"^\s*(`{3,}|~{3,})", line)
        if fence:
            if marker and marker[1][0] == fence[0] and len(marker[1]) >= len(fence):
                fence = None
            lines.append("")
        elif marker:
            fence = marker[1]
            lines.append("")
        else:
            lines.append(line)
    return "\n".join(lines)


def anchors(text):
    result = set(re.findall(r'\b(?:id|name)=["\']([^"\']+)["\']', text))
    counts = Counter()
    for match in re.finditer(r"^ {0,3}#{1,6}\s+(.+?)\s*#*\s*$", text, re.M):
        title = unescape(re.sub(r"<[^>]+>", "", match[1])).lower()
        title = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", title)
        slug = re.sub(r"[^\w\-\s]", "", title).replace(" ", "-")
        result.add(slug if not counts[slug] else f"{slug}-{counts[slug]}")
        counts[slug] += 1
    return result


def links(text):
    # Inline code can contain illustrative Markdown without creating a real link.
    text = re.sub(r"(`+).*?\1", lambda m: " " * len(m[0]), text)
    patterns = (
        r"!?\[[^\]\n]*\]\(\s*(<[^>]+>|[^\s)]+)(?:\s+[^)]*)?\)",
        r"^ {0,3}\[[^\]\n]+\]:\s*(<[^>]+>|\S+)",
        r"\b(?:href|src)=[\"']([^\"']+)[\"']",
    )
    for pattern in patterns:
        for match in re.finditer(pattern, text, re.M):
            yield text[:match.start()].count("\n") + 1, unescape(match[1].strip("<>"))


def check(root):
    pages = [root / "README.md", *sorted((root / "docs").rglob("*.md"))]
    texts = {p.resolve(): prose(p.read_text(encoding="utf-8-sig")) for p in pages if p.is_file()}
    ids = {p: anchors(text) for p, text in texts.items()}
    edges = {p: set() for p in texts}
    errors = []
    count = 0
    for page, text in texts.items():
        for line, url in links(text):
            parts = urlsplit(url)
            if parts.scheme or parts.netloc:
                continue
            count += 1
            target = (page.parent / unquote(parts.path)).resolve() if parts.path else page
            location = f"{page.relative_to(root).as_posix()}:{line}"
            if not target.is_relative_to(root):
                errors.append(f"{location}: link escapes documentation root: {url}")
            elif not target.exists():
                errors.append(f"{location}: missing target: {url}")
            elif parts.fragment and target in ids and unquote(parts.fragment) not in ids[target]:
                errors.append(f"{location}: missing anchor: {url}")
            if target in texts:
                edges[page].add(target)

    index = root / "docs/README.md"
    if not index.is_file():
        errors.append("Missing documentation index: docs/README.md")
    visited, pending = set(), [index]
    while pending:
        page = pending.pop()
        if page not in visited:
            visited.add(page)
            pending.extend(edges.get(page, set()) - visited)
    for page in texts:
        if page != root / "README.md" and page not in visited:
            errors.append(f"Not reachable from docs/README.md: {page.relative_to(root).as_posix()}")
    return len(texts), count, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="Repository root or an extracted package root")
    args = parser.parse_args()
    pages, links_count, errors = check(args.root.resolve())
    for error in errors:
        print(error)
    print(f"Documentation: {pages} pages, {links_count} local links, {len(errors)} errors")
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
