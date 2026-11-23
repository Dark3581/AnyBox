import argparse
import posixpath
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
README = ROOT / "README.md"
START = "<!-- docs:start -->"
END = "<!-- docs:end -->"
PAGES = [
    ("Usage", "docs/user/USAGE.md"),
    ("Build", "docs/dev/BUILD.md"),
    ("Architecture", "docs/dev/ARCHITECTURE.md"),
    ("Game testing", "docs/dev/GAME_TESTING.md"),
    ("Contributing", "CONTRIBUTING.md"),
    ("Conventions", "docs/dev/CONVENTIONS.md"),
    ("Technical debt", "docs/dev/TechnicalDebt.md"),
    ("Progress", "docs/dev/PROGRESS.md"),
]

MARKDOWN_LINK = re.compile(r'(\]\()([^)\s]+)((?:\s+"[^"]*")?\))')
HTML_LINK = re.compile(r'((?:href|src)=")([^"]+)(")')
TAB_BAR = re.compile(r'^\s*<p align="center">.*?</p>\s*\n', re.S)


def rewrite(target, page):
    if re.match(r'^[a-z][a-z0-9+.-]*:', target, re.I) or target.startswith('/'):
        return target
    if target.startswith('#'):
        return page + target
    path, _, anchor = target.partition('#')
    resolved = posixpath.normpath(posixpath.join(posixpath.dirname(page), path))
    return resolved + ('#' + anchor if anchor else '')


def body(text, page):
    lines = text.split('\n')
    if lines and lines[0].startswith('# '):
        lines = lines[1:]
    content = TAB_BAR.sub('', '\n'.join(lines).lstrip('\n'), count=1)
    out = []
    fenced = False
    for line in content.split('\n'):
        if line.lstrip().startswith('```'):
            fenced = not fenced
        if not fenced:
            line = MARKDOWN_LINK.sub(lambda m: m.group(1) + rewrite(m.group(2), page) + m.group(3), line)
            line = HTML_LINK.sub(lambda m: m.group(1) + rewrite(m.group(2), page) + m.group(3), line)
        out.append(line)
    return '\n'.join(out).strip('\n')


def section(root=ROOT):
    blocks = []
    for title, page in PAGES:
        text = (root / page).read_text(encoding='utf-8')
        blocks.append(f'<details>\n<summary><b>{title}</b> (<a href="{page}">{page}</a>)</summary>\n\n{body(text, page)}\n\n</details>')
    return START + '\n' + '\n\n'.join(blocks) + '\n' + END


def updated(readme_text, root=ROOT):
    if START not in readme_text or END not in readme_text:
        raise ValueError(f"README.md has no {START} ... {END} block")
    before, rest = readme_text.split(START, 1)
    _, after = rest.split(END, 1)
    return before + section(root) + after


def main():
    parser = argparse.ArgumentParser(description="Embed the documentation pages in README.md as collapsible sections.")
    parser.add_argument("--check", action="store_true", help="fail if README.md is not up to date instead of writing it")
    args = parser.parse_args()
    current = README.read_text(encoding='utf-8')
    try:
        fresh = updated(current)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    if args.check:
        if fresh != current:
            print("README.md is out of date: run python tools/readme_docs.py", file=sys.stderr)
            return 1
        print("readme docs: ok")
        return 0
    if fresh != current:
        README.write_text(fresh, encoding='utf-8', newline='\n')
        print("README.md updated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
