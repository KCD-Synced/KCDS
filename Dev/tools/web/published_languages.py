"""The resource languages and features a release publishes, and the public docs cut to them.

A language not in PUBLISHED is left out of the docs bundle (build_docs_bundle.py: its guides lua.md,
csharp.md, its code blocks and the reference's examples and signatures in it) and out of the Types
zip (Dev/tools/release/package.py: the Lua annotations, the C# SDK). Its code and tests stay.

KCDS-DOCS marks the text about a language that is not always published:

    <!-- only lua cs -->        a block, up to <!-- end -->, kept when one of them is published
    text <!-- only lua cs -->   that line kept only then
    text <!-- without lua cs --> that line kept only when none of them is published

and the markers themselves never reach the bundle.

Features take the same markers, and a feature's guide is left out like a language's:

    studio   the Asset/Animation Studio: asset-studio.md and the text about it in other guides
    sealed   the text about sealed resource files in client-resources.md
    examples the example resources (Dev/examples/resources, the Tabletop guide's chess game)
    apichanges  api-changes.md, the old names and their new ones, and the links to it

To publish one, add its name to PUBLISHED.
"""

import re

PUBLISHED = frozenset({"js", "lua", "cs", "studio", "sealed", "examples", "apichanges"})

# The guides of a language or a feature, by its code: left out while it is not published.
LANGUAGE_GUIDES = {
    "lua": "lua",
    "cs": "csharp",
    "studio": "asset-studio",
    "apichanges": "api-changes",
}

FENCE = re.compile(r"^```([A-Za-z#]*)[^\n]*\n.*?^```[ \t]*\n?", re.M | re.S)
BLOCK = re.compile(r"^<!-- only ([a-z ]+) -->[ \t]*\n(.*?)^<!-- end -->[ \t]*\n?", re.M | re.S)
LINE = re.compile(r"^(.*?)[ \t]*<!-- (only|without) ([a-z ]+) -->[ \t]*$", re.M)
FENCE_LANGUAGES = {"lua": "lua", "cs": "cs", "csharp": "cs", "c#": "cs"}


def published(names, languages=PUBLISHED):
    """Whether any of the languages named (`lua cs`) is published."""
    return any(name in languages for name in names.split())


def cut_markdown(text, languages=PUBLISHED):
    """A KCDS-DOCS page with only what the published languages need."""

    def block(match):
        return match.group(2) if published(match.group(1), languages) else ""

    def line(match):
        keep = published(match.group(3), languages)
        if match.group(2) == "without":
            keep = not keep
        return match.group(1) if keep else "\x00"

    def fence(match):
        language = FENCE_LANGUAGES.get(match.group(1).lower())
        return "" if language and language not in languages else match.group(0)

    text = text.replace("\r\n", "\n")
    text = BLOCK.sub(block, text)
    text = LINE.sub(line, text)
    text = re.sub(r"^\x00\n", "", text, flags=re.M)
    text = FENCE.sub(fence, text)
    # Blocks taken out leave runs of blank lines.
    return re.sub(r"\n{3,}", "\n\n", text)


def guide_published(stem, languages=PUBLISHED):
    """Whether a KCDS-DOCS page (its file name without .md) is published."""
    for code, guide in LANGUAGE_GUIDES.items():
        if stem.lower() == guide and code not in languages:
            return False
    return True


def cut_api(api, languages=PUBLISHED):
    """The reference without the examples and signatures of languages not published."""
    for api_type in api.get("types", []):
        for member in api_type.get("members", []):
            member["examples"] = [
                example
                for example in member.get("examples", [])
                if FENCE_LANGUAGES.get(example.get("lang", "js"), "js") in languages
            ]
            signatures = member.get("signatures")
            if signatures:
                member["signatures"] = {
                    code: text for code, text in signatures.items() if code in languages
                }
                if not member["signatures"]:
                    del member["signatures"]
    return api
