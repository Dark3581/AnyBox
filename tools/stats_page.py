#!/usr/bin/env python3
"""Write the Pages front page from the README's "Status" and "Stats" sections.

    python tools/stats_page.py README.md site/index.html
    python tools/stats_page.py README.md site/index.html --banner site/progress.html site/needs.html site/history.html

--banner also puts the links and the stats table at the top of the other pages.
"""

import argparse
import html
import re
import sys
from pathlib import Path

LINKS = (("index.html", "Status and stats"), ("progress.html", "Library progress"), ("needs.html", "What titles need"), ("history.html", "Progress history"))
REPOSITORY = "https://github.com/Dark3581/AnyBox"


def section(markdown, heading):
    """The text under the first `## heading` up to the next `## ` heading, or None."""
    match = re.search(rf"^## {re.escape(heading)}[ \t]*\n(.*?)(?=^## |\Z)", markdown, re.MULTILINE | re.DOTALL)
    return match.group(1).strip() if match else None


def inline(text):
    """Escape text and render `code`, **bold** and [label](url) links."""
    out = html.escape(text, quote=False)
    out = re.sub(r"`([^`]+)`", r"<code>\1</code>", out)
    out = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", out)

    def link(match):
        url = match.group(2)
        if not re.match(r"https?://", url):
            url = f"{REPOSITORY}/blob/main/{url}"
        return f'<a href="{html.escape(url)}">{match.group(1)}</a>'

    return re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, out)


def cells(row):
    return [cell.strip() for cell in row.strip().strip("|").split("|")]


def blocks(text):
    """Paragraphs and pipe tables of a Markdown section as HTML; other block types are kept as paragraphs."""
    parts = []
    for block in re.split(r"\n\s*\n", text.strip()):
        lines = block.strip().splitlines()
        if not lines:
            continue
        if len(lines) >= 2 and lines[0].lstrip().startswith("|") and re.fullmatch(r"\s*\|?[\s:|-]+\|?\s*", lines[1]):
            head = "".join(f"<th>{inline(cell)}</th>" for cell in cells(lines[0]))
            body = "".join("<tr>" + "".join(f"<td>{inline(cell)}</td>" for cell in cells(line)) + "</tr>" for line in lines[2:])
            parts.append(f"<table><thead><tr>{head}</tr></thead><tbody>{body}</tbody></table>")
        else:
            parts.append(f"<p>{inline(' '.join(line.strip() for line in lines))}</p>")
    return "\n".join(parts)


def page(markdown):
    status = section(markdown, "Status")
    stats = section(markdown, "Stats")
    if status is None and stats is None:
        raise ValueError('the README has neither a "## Status" nor a "## Stats" section')
    body = []
    if status is not None:
        body.append(f"<h2>Status</h2>\n{blocks(status.split('<!-- docs:start -->')[0])}")
    if stats is not None:
        body.append(f"<h2>Stats</h2>\n{blocks(stats)}")
    links = " &nbsp;·&nbsp; ".join(f'<a href="{href}">{label}</a>' for href, label in LINKS)
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AnyBox</title>
<style>
body{{font-family:system-ui,sans-serif;background:#0d1117;color:#e6edf3;margin:0 auto;padding:16px;max-width:960px;line-height:1.5}}
a{{color:#58a6ff}} code{{background:#161b22;padding:1px 4px;border-radius:4px;font-size:13px}}
table{{border-collapse:collapse;width:100%;font-size:14px;margin:8px 0}}
th,td{{border-bottom:1px solid #30363d;padding:6px;text-align:left;vertical-align:top}}
nav{{margin:8px 0 16px;color:#8b949e}}
</style></head><body>
<h1><a href="{REPOSITORY}">AnyBox</a></h1>
<nav>{links}</nav>
{chr(10).join(body)}
</body></html>
"""


def banner(markdown):
    """The links and the Stats section as a block for the top of another page."""
    stats = section(markdown, "Stats")
    links = " &nbsp;·&nbsp; ".join(f'<a href="{href}">{label}</a>' for href, label in LINKS)
    table = f"<details open><summary>Current stats</summary>\n{blocks(stats)}\n</details>" if stats is not None else ""
    return (f'<div class="stats-banner" style="border-bottom:1px solid #30363d;margin-bottom:16px;padding-bottom:8px">'
            f"<nav>{links}</nav>\n{table}</div>\n")


def add_banner(page_html, markdown):
    """page_html with the banner right after its <body> tag; unchanged when it already has one or has no <body>."""
    if 'class="stats-banner"' in page_html:
        return page_html
    match = re.search(r"<body[^>]*>", page_html)
    if match is None:
        return page_html
    return page_html[:match.end()] + "\n" + banner(markdown) + page_html[match.end():]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("readme", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--banner", type=Path, nargs="*", default=[], help="pages to put the links and stats at the top of")
    args = parser.parse_args(argv)
    markdown = args.readme.read_text(encoding="utf-8")
    try:
        text = page(markdown)
    except ValueError as error:
        print(f"stats_page: {error}", file=sys.stderr)
        return 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    for other in args.banner:
        if other.is_file():
            other.write_text(add_banner(other.read_text(encoding="utf-8"), markdown), encoding="utf-8")
        else:
            print(f"stats_page: {other} does not exist, no banner added", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
