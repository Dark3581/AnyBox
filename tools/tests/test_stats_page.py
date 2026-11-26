import unittest

import helpers  # noqa: F401  (puts tools/ on the path)

import stats_page

README = """# AnyBox

## Status

Loads into the world in about 15 minutes, at 3 to 4 fps. Details: [Bring-up status](docs/dev/GAME_TESTING.md#bring-up-status).

<!-- docs:start -->
<details>generated</details>
<!-- docs:end -->

## Stats

Measured on one PC.

| | Value | Run, date |
|---|---|---|
| Frame rate | 3 to 4 fps | run 76 |
| GPU time | 457 ms <script> | run 76 |

The runs set `d3d12_x.indirect_groups_max = 200000`.

## Libraries

| Library | Serves |
|---|---|
"""


class StatsPageTest(unittest.TestCase):
    def test_sections(self):
        self.assertTrue(stats_page.section(README, "Stats").startswith("Measured on one PC."))
        self.assertNotIn("Library", stats_page.section(README, "Stats"))
        self.assertIsNone(stats_page.section(README, "Missing"))

    def test_page(self):
        text = stats_page.page(README)
        self.assertIn("<td>3 to 4 fps</td>", text)
        self.assertIn("457 ms &lt;script&gt;", text)
        self.assertIn("<code>d3d12_x.indirect_groups_max = 200000</code>", text)
        self.assertIn('href="https://github.com/Dark3581/AnyBox/blob/main/docs/dev/GAME_TESTING.md#bring-up-status"', text)
        self.assertNotIn("generated", text)
        self.assertNotIn("Serves", text)
        self.assertIn('href="progress.html"', text)

    def test_banner(self):
        other = "<html><head></head><body><h1>Needs</h1></body></html>"
        text = stats_page.add_banner(other, README)
        self.assertLess(text.index('class="stats-banner"'), text.index("<h1>Needs</h1>"))
        self.assertIn("<td>3 to 4 fps</td>", text)
        self.assertIn('href="index.html"', text)
        self.assertEqual(stats_page.add_banner(text, README), text)
        self.assertEqual(stats_page.add_banner("no body here", README), "no body here")

    def test_no_sections(self):
        with self.assertRaises(ValueError):
            stats_page.page("# AnyBox\n\nnothing\n")


if __name__ == "__main__":
    unittest.main()
