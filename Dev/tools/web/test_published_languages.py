"""The public docs cut to the resource languages a release publishes (published_languages.py)."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from published_languages import cut_api, cut_markdown, guide_published  # noqa: E402

PAGE = """# Events

Intro.

<!-- only lua cs -->
## Between languages

Lua and C# share events.
<!-- end -->

| Key | Meaning |
| --- | ------- |
| `a` | in CommonJS, Lua or C# | <!-- only lua cs -->
| `a` | in CommonJS | <!-- without lua cs -->

```js
a();
```

```lua
a()
```

```cs
A();
```

The end.
"""


class CutMarkdownTest(unittest.TestCase):
    def test_javascript_only(self):
        cut = cut_markdown(PAGE, frozenset({"js"}))
        self.assertNotIn("Between languages", cut)
        self.assertNotIn("Lua", cut)
        self.assertIn("| `a` | in CommonJS |", cut)
        self.assertIn("```js\na();\n```", cut)
        self.assertNotIn("```lua", cut)
        self.assertNotIn("```cs", cut)
        self.assertNotIn("<!--", cut)
        self.assertNotIn("\n\n\n", cut)

    def test_every_language(self):
        cut = cut_markdown(PAGE, frozenset({"js", "lua", "cs"}))
        self.assertIn("## Between languages", cut)
        self.assertIn("| `a` | in CommonJS, Lua or C# |", cut)
        self.assertNotIn("| `a` | in CommonJS |", cut)
        self.assertIn("```lua", cut)
        self.assertIn("```cs", cut)
        self.assertNotIn("<!--", cut)

    def test_windows_line_ends(self):
        cut = cut_markdown(PAGE.replace("\n", "\r\n"), frozenset({"js"}))
        self.assertNotIn("Between languages", cut)
        self.assertNotIn("<!--", cut)

    def test_guides(self):
        self.assertFalse(guide_published("lua", frozenset({"js"})))
        self.assertFalse(guide_published("csharp", frozenset({"js"})))
        self.assertTrue(guide_published("events", frozenset({"js"})))
        self.assertTrue(guide_published("lua", frozenset({"js", "lua"})))


class CutApiTest(unittest.TestCase):
    def test_examples_and_signatures(self):
        api = {
            "types": [
                {
                    "members": [
                        {
                            "examples": [
                                {"lang": "js", "code": "a()"},
                                {"lang": "lua", "code": "a()"},
                                {"lang": "cs", "code": "A();"},
                            ],
                            "signatures": {"lua": "a()", "cs": "void A()"},
                        }
                    ]
                }
            ]
        }
        member = cut_api(api, frozenset({"js"}))["types"][0]["members"][0]
        self.assertEqual([example["lang"] for example in member["examples"]], ["js"])
        self.assertNotIn("signatures", member)

    def test_features_not_announced_are_left_out(self):
        text = (
            "Files go to players.\n"
            "<!-- only sealed -->\n"
            "They are sealed.\n"
            "<!-- end -->\n"
            "See [Siege](siege.md). <!-- without studio -->\n"
            "See the studio. <!-- only studio -->\n"
        )
        cut = cut_markdown(text, frozenset({"js"}))
        self.assertEqual(cut, "Files go to players.\nSee [Siege](siege.md).\n")
        self.assertFalse(guide_published("asset-studio", frozenset({"js"})))
        self.assertTrue(guide_published("asset-studio", frozenset({"js", "studio"})))


if __name__ == "__main__":
    unittest.main()
