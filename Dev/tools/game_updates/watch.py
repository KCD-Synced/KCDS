"""Kingdom Come: Deliverance II update watcher.

The scheduled workflow .github/workflows/game-update-watch.yml runs this every 10 minutes. It reads
the game's public Steam build anonymously (steamcmd app_info_print), and when a new build is seen
on two runs in a row it opens a game-update issue with the follow-up checklist and posts once to
the team's Discord channel. Every store ships the same game version, so a Steam update stands for
an update on all of them.

  watch.py read --app 1771300 --steamcmd-output info.txt     -> {"build": "...", "depots": {...}}
  watch.py issue --build B --previous A --seen <iso time>      -> the issue's title and body (JSON)
  watch.py discord --build B --previous A --issue-url <url>    -> posts to $DISCORD_WEBHOOK
  watch.py unreachable --last-success <iso time> --now <iso>   -> posts once after an hour
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import sys
import urllib.request

KCD2_APP = "1771300"
# Steam being unreachable for this long is reported (once).
UNREACHABLE_ALERT_AFTER = datetime.timedelta(hours=1)
# The workflow's interval: the alert goes out on the first run past the hour only.
RUN_INTERVAL = datetime.timedelta(minutes=10)


class VdfError(ValueError):
    pass


def _tokens(text: str):
    i = 0
    while i < len(text):
        c = text[i]
        if c.isspace():
            i += 1
        elif c in "{}":
            yield c
            i += 1
        elif c == '"':
            value = []
            i += 1
            while i < len(text) and text[i] != '"':
                if text[i] == "\\" and i + 1 < len(text):
                    i += 1
                value.append(text[i])
                i += 1
            if i >= len(text):
                raise VdfError("unterminated string")
            i += 1
            yield "".join(value)
        elif text.startswith("//", i):
            while i < len(text) and text[i] != "\n":
                i += 1
        else:
            raise VdfError(f"unexpected character {c!r}")


def parse_vdf(text: str) -> dict:
    """Valve's KeyValues text (quoted keys and values, braces for nested blocks)."""
    root: dict = {}
    stack = [root]
    key = None
    for token in _tokens(text):
        if token == "{":
            if key is None:
                raise VdfError("block without a key")
            block: dict = {}
            stack[-1][key] = block
            stack.append(block)
            key = None
        elif token == "}":
            if len(stack) == 1 or key is not None:
                raise VdfError("unbalanced block")
            stack.pop()
        elif key is None:
            key = token
        else:
            stack[-1][key] = token
            key = None
    if len(stack) != 1 or key is not None:
        raise VdfError("unterminated block")
    return root


def app_info(steamcmd_output: str, app: str) -> dict:
    """The app's block in steamcmd's app_info_print output (which has log lines around it)."""
    marker = f'"{app}"'
    start = -1
    for line_start, line in _lines_with_offsets(steamcmd_output):
        if line.strip() == marker:
            start = line_start
            break
    if start < 0:
        raise VdfError(f"app {app} not in the steamcmd output")
    depth = 0
    end = -1
    in_string = False
    for i in range(start, len(steamcmd_output)):
        c = steamcmd_output[i]
        if c == '"' and (i == 0 or steamcmd_output[i - 1] != "\\"):
            in_string = not in_string
        elif not in_string and c == "{":
            depth += 1
        elif not in_string and c == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    if end < 0:
        raise VdfError(f"app {app}'s block is not closed")
    return parse_vdf(steamcmd_output[start:end])[app]


def _lines_with_offsets(text: str):
    offset = 0
    for line in text.splitlines(keepends=True):
        yield offset, line
        offset += len(line)


def public_build(info: dict) -> dict:
    """The public branch's build id and each depot's public manifest id."""
    depots = info.get("depots", {})
    build = depots.get("branches", {}).get("public", {}).get("buildid", "")
    if not build.isdigit():
        raise VdfError("no public build id")
    manifests = {}
    for depot, entry in depots.items():
        if not depot.isdigit() or not isinstance(entry, dict):
            continue
        public = entry.get("manifests", {}).get("public")
        # Newer app info nests the manifest id as {"gid": ...}; older has it directly.
        gid = public.get("gid") if isinstance(public, dict) else public
        if isinstance(gid, str) and gid.isdigit():
            manifests[depot] = gid
    return {"build": build, "depots": manifests}


def issue_text(build: str, previous: str, seen: str) -> dict:
    title = f"Kingdom Come: Deliverance II updated: Steam build {build}"
    body = "\n".join(
        [
            f"Steam's public build of Kingdom Come: Deliverance II changed from {previous or 'unknown'} "
            f"to **{build}** (first seen {seen}, confirmed on the next run).",
            "",
            "Every store ships the same game version, so this stands for an update on Steam, the Epic "
            "Games Store, GOG and the Microsoft Store.",
            "",
            "- [ ] The client's game functions still resolve on the new build (signature audit)",
            "- [ ] Compatibility tested in a game: join, combat, riding",
            "- [ ] Known builds updated: GOG build list on the Master (`-gog-builds`)",
            "- [ ] Epic, GOG and Microsoft Store builds checked once they publish the same version",
            "- [ ] Players told what works (status, Discord)",
            "- [ ] KCDS release needed?",
        ]
    )
    return {"title": title, "body": body}


def discord_text(build: str, previous: str, issue_url: str) -> str:
    return (
        f"**Kingdom Come: Deliverance II was updated** on Steam: build {previous or 'unknown'} "
        f"-> {build}. KCDS compatibility has to be checked: {issue_url}"
    )


def unreachable_alert_due(last_success: datetime.datetime, now: datetime.datetime) -> bool:
    """True on the one run that first finds Steam unreachable for more than an hour."""
    down = now - last_success
    return UNREACHABLE_ALERT_AFTER < down <= UNREACHABLE_ALERT_AFTER + RUN_INTERVAL


def post_discord(webhook: str, text: str) -> None:
    request = urllib.request.Request(
        webhook,
        data=json.dumps({"content": text, "allowed_mentions": {"parse": []}}).encode(),
        headers={"Content-Type": "application/json", "User-Agent": "kcds-game-update-watch"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=20) as answer:
        if answer.status >= 300:
            raise RuntimeError(f"Discord answered {answer.status}")


def _time(text: str) -> datetime.datetime:
    return datetime.datetime.fromisoformat(text.replace("Z", "+00:00"))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    read = commands.add_parser("read")
    read.add_argument("--app", default=KCD2_APP)
    read.add_argument("--steamcmd-output", required=True)
    issue = commands.add_parser("issue")
    issue.add_argument("--build", required=True)
    issue.add_argument("--previous", default="")
    issue.add_argument("--seen", required=True)
    discord = commands.add_parser("discord")
    discord.add_argument("--build", required=True)
    discord.add_argument("--previous", default="")
    discord.add_argument("--issue-url", required=True)
    unreachable = commands.add_parser("unreachable")
    unreachable.add_argument("--last-success", required=True)
    unreachable.add_argument("--now", required=True)
    options = parser.parse_args(argv)

    if options.command == "read":
        with open(options.steamcmd_output, encoding="utf-8", errors="replace") as file:
            print(json.dumps(public_build(app_info(file.read(), options.app))))
    elif options.command == "issue":
        print(json.dumps(issue_text(options.build, options.previous, options.seen)))
    elif options.command == "discord":
        webhook = os.environ.get("DISCORD_WEBHOOK", "")
        if webhook:
            post_discord(webhook, discord_text(options.build, options.previous, options.issue_url))
        else:
            print("DISCORD_WEBHOOK is not set: no Discord post", file=sys.stderr)
    elif options.command == "unreachable":
        if unreachable_alert_due(_time(options.last_success), _time(options.now)):
            webhook = os.environ.get("DISCORD_WEBHOOK", "")
            text = (
                "**KCDS game update watcher:** Steam's app info has been unreachable for over an "
                "hour; game updates are not being noticed."
            )
            if webhook:
                post_discord(webhook, text)
            print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
