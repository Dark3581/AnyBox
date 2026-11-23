import tempfile
import unittest
from pathlib import Path

import helpers

import readme_docs


class ReadmeDocsTest(unittest.TestCase):
    def test_readme_is_up_to_date(self):
        text = readme_docs.README.read_text(encoding="utf-8")
        self.assertEqual(readme_docs.updated(text), text, "README.md is out of date: run python tools/readme_docs.py")

    def test_links_are_rewritten_relative_to_the_readme(self):
        page = "docs/dev/BUILD.md"
        self.assertEqual(readme_docs.rewrite("ARCHITECTURE.md#graphics", page), "docs/dev/ARCHITECTURE.md#graphics")
        self.assertEqual(readme_docs.rewrite("../../core/relinker", page), "core/relinker")
        self.assertEqual(readme_docs.rewrite("#requirements", page), "docs/dev/BUILD.md#requirements")
        self.assertEqual(readme_docs.rewrite("https://example.com/a", page), "https://example.com/a")
        self.assertEqual(readme_docs.rewrite("mailto:a@b", page), "mailto:a@b")

    def test_body_drops_title_and_tab_bar_and_keeps_code(self):
        text = '# Build\n\n<p align="center">\n  <a href="../../README.md">Home</a>\n</p>\n\nSee [usage](../user/USAGE.md) and <a href="CONVENTIONS.md">rules</a>.\n\n```\n[not a link](x.md)\n```\n'
        out = readme_docs.body(text, "docs/dev/BUILD.md")
        self.assertNotIn("# Build", out)
        self.assertNotIn("Home", out)
        self.assertIn("[usage](docs/user/USAGE.md)", out)
        self.assertIn('<a href="docs/dev/CONVENTIONS.md">rules</a>', out)
        self.assertIn("[not a link](x.md)", out)

    def test_markers_are_required(self):
        with self.assertRaises(ValueError):
            readme_docs.updated("# AnyBox\n")

    def test_section_from_a_tree(self):
        with tempfile.TemporaryDirectory() as root:
            for _, page in readme_docs.PAGES:
                path = Path(root) / page
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(f"# Title\n\nText of {page}.\n", encoding="utf-8")
            text = readme_docs.updated("# AnyBox\n\n<!-- docs:start -->\nold\n<!-- docs:end -->\n\nRest.\n", Path(root))
        self.assertEqual(text.count("<details>"), len(readme_docs.PAGES))
        self.assertIn("Text of docs/dev/BUILD.md.", text)
        self.assertTrue(text.endswith("<!-- docs:end -->\n\nRest.\n"))
        self.assertNotIn("old", text)


if __name__ == "__main__":
    unittest.main()
