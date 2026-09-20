"""Print actual checkout identities from the current west workspace (read-only)."""

import subprocess
from pathlib import Path

import yaml


def output(*command: str) -> str:
    return subprocess.check_output(command, text=True).strip()


root = Path(output("west", "topdir"))
manifest = yaml.safe_load(output("west", "manifest", "--resolve"))["manifest"]
print("name\trevision\tcheckout\tdirty")
for project in manifest["projects"]:
    path = root / project.get("path", project["name"])
    if (path / ".git").exists():
        sha = output("git", "-C", str(path), "rev-parse", "HEAD")
        dirty = bool(output("git", "-C", str(path), "status", "--porcelain", "-uno"))
        print(f"{project['name']}\t{project['revision']}\t{sha}\t{dirty}")
    else:
        print(f"{project['name']}\t{project['revision']}\tNOT_CHECKED_OUT\t-")
