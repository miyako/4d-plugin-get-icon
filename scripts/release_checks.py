"""Fail-closed checks for Developer ID distribution metadata."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess


def keychain_search_list(snapshot, keychain=None):
    if keychain is not None:
        original = subprocess.check_output(
            ["security", "list-keychains", "-d", "user"], text=True
        )
        snapshot.write_text(original, encoding="utf-8")
        paths = [str(keychain), *shlex.split(original)]
    else:
        paths = shlex.split(snapshot.read_text(encoding="utf-8"))
    subprocess.run(["security", "list-keychains", "-d", "user", "-s", *paths], check=True)


def signature(details, team, runtime=False):
    if not re.fullmatch(r"[A-Z0-9]{10}", team):
        raise ValueError("Expected a 10-character Apple team ID")
    lines = details.splitlines()
    if f"TeamIdentifier={team}" not in lines:
        raise ValueError("Signing team does not match NOTARYTOOL_TEAM_ID")
    if not any(line.startswith("Authority=Developer ID Application: ") for line in lines):
        raise ValueError("A Developer ID Application signature is required")
    if not any(line.startswith("Timestamp=") and line[10:].strip().lower() not in ("", "none")
               for line in lines):
        raise ValueError("A secure signing timestamp is required")
    if any("adhoc" in line.lower() or line.startswith("Signed Time=") for line in lines):
        raise ValueError("Ad-hoc signatures and local signing times are not distribution signatures")
    if runtime and not any(re.search(r"\bflags=0x[0-9a-fA-F]+\(.*\bruntime\b", line)
                           for line in lines):
        raise ValueError("The plugin must use the hardened runtime")


def notarization(report):
    if report.get("status") != "Accepted" or not report.get("id"):
        raise ValueError(f"Notarization was not Accepted: {report.get('status', 'missing status')}")


def checksums(directory):
    files = sorted(path for path in directory.iterdir() if path.name != "SHA256SUMS")
    if not files or any(path.is_symlink() or not path.is_file() for path in files):
        raise ValueError("Distribution must contain regular artifact files only")
    lines = []
    for path in files:
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        lines.append(f"{digest}  {path.name}\n")
    (directory / "SHA256SUMS").write_text("".join(lines), encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    check_signature = commands.add_parser("signature")
    check_signature.add_argument("--details", type=Path, required=True)
    check_signature.add_argument("--team", required=True)
    check_signature.add_argument("--runtime", action="store_true")
    check_notary = commands.add_parser("notarization")
    check_notary.add_argument("--report", type=Path, required=True)
    sums = commands.add_parser("checksums")
    sums.add_argument("--directory", type=Path, required=True)
    keychains = commands.add_parser("keychain-search-list")
    keychains.add_argument("--snapshot", type=Path, required=True)
    keychains.add_argument("--add", type=Path)
    args = parser.parse_args()
    if args.command == "signature":
        signature(args.details.read_text(encoding="utf-8"), args.team, args.runtime)
    elif args.command == "notarization":
        notarization(json.loads(args.report.read_text(encoding="utf-8")))
    elif args.command == "checksums":
        checksums(args.directory)
    else:
        keychain_search_list(args.snapshot, args.add)


if __name__ == "__main__":
    main()
