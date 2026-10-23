"""Publish verified Windows installers to SourceForge using OpenSSH and rsync.

Only the publisher-owned windows/{stable,prerelease} tree is managed. Credentials
are passed by the release job and are never included in commands or diagnostics.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

HOST = "frs.sourceforge.net"
# SourceForge's published FRS ED25519 host fingerprint, checked 2026-10-23.
# https://sourceforge.net/p/forge/documentation/SSH%20Key%20Fingerprints/
HOST_FINGERPRINT = "209BDmH3jsRyO9UeGPPgLWPSegKmYCBIya0nR/AWWCY"
VERSION = re.compile(r"[0-9]+(?:\.[0-9]+){2,3}(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?\Z")
NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*\Z")
FORMAT = "sourceforge-release-v1"


class ReleaseError(Exception):
    """A diagnostic that is safe to display in public CI logs."""


def sha256(file: Path) -> str:
    with file.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def version_key(value: str) -> tuple:
    if not VERSION.fullmatch(value):
        raise ReleaseError("Invalid release version.")
    numbers, _, suffix = value.partition("-")
    parts = tuple(int(n) for n in numbers.split("."))
    parts += (0,) * (4 - len(parts))
    tokens = tuple((0, int(s)) if s.isdigit() else (1, s)
                   for s in re.split(r"[.-]", suffix)) if suffix else ()
    return parts, not bool(suffix), tokens


def validate_manifest(value: dict, channel: str) -> None:
    if value.get("format") != FORMAT or value.get("channel") != channel:
        raise ReleaseError("Remote release manifest has an unknown format or channel.")
    version_key(value.get("version", ""))
    if ("-" in value["version"]) != (channel == "prerelease"):
        raise ReleaseError("Remote release version does not match its channel.")
    files = value.get("files", [])
    if not files or len(files) > 8:
        raise ReleaseError("Invalid remote release file list.")
    for item in files:
        if not NAME.fullmatch(item.get("name", "")) or not re.fullmatch(r"[a-f0-9]{64}", item.get("sha256", "")):
            raise ReleaseError("Unsafe remote release file entry.")
        if not isinstance(item.get("bytes"), int) or item["bytes"] < 1:
            raise ReleaseError("Invalid remote release file size.")
    if len({item["name"] for item in files}) != len(files):
        raise ReleaseError("Duplicate remote release file entry.")
    if value.get("state") not in ("verified", "published"):
        raise ReleaseError("Invalid remote release state.")


def local_manifest(artifact: Path, notes: Path, version: str, source: str) -> tuple[dict, list[Path]]:
    version_key(version)
    if not re.fullmatch(r"[a-f0-9]{40}", source):
        raise ReleaseError("A full source commit SHA is required.")
    checksum = Path(str(artifact) + ".sha256")
    if not artifact.is_file() or artifact.suffix.lower() != ".exe" or not checksum.is_file() or not notes.is_file():
        raise ReleaseError("The installer, SHA-256 file, and release notes are required.")
    if artifact.stat().st_size > 4100000000:
        raise ReleaseError("Installer exceeds the single-EXE release size budget.")
    expected = re.fullmatch(r"([a-fA-F0-9]{64}) [ *](.+)\n?", checksum.read_text(encoding="utf-8-sig").replace("\r\n", "\n"))
    if not expected or expected[2] != artifact.name or sha256(artifact) != expected[1].lower():
        raise ReleaseError("Installer checksum verification failed.")
    paths = [artifact.resolve(), checksum.resolve(), notes.resolve()]
    files = [{"name": p.name, "bytes": p.stat().st_size, "sha256": sha256(p)} for p in paths]
    result = {"format": FORMAT, "version": version, "source": source,
              "channel": "prerelease" if "-" in version else "stable",
              "state": "verified", "files": files}
    validate_manifest(result, result["channel"])
    return result, paths


def download_url(project: str, channel: str, version: str, name: str) -> str:
    path = "/".join(urllib.parse.quote(s, safe="") for s in
                    (project, "windows", channel, version, name))
    return "https://downloads.sourceforge.net/project/" + path


def verify_download(url: str, expected: dict, attempts: int = 20) -> None:
    print("Verifying public mirror copy: " + expected["name"], flush=True)
    for attempt in range(attempts):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "ForkReleasePublisher/1.0"})
            with urllib.request.urlopen(request, timeout=120) as response:
                digest = hashlib.sha256()
                size = 0
                while block := response.read(1024 * 1024):
                    size += len(block)
                    if size > expected["bytes"]:
                        raise ReleaseError("Mirror returned an oversized release file.")
                    digest.update(block)
            if size == expected["bytes"] and digest.hexdigest() == expected["sha256"]:
                return
        except (OSError, urllib.error.URLError, ReleaseError):
            pass
        if attempt + 1 < attempts:
            print(f"Waiting for a complete mirror copy (attempt {attempt + 1}/{attempts}).", flush=True)
            time.sleep(min(30, 2 ** min(attempt, 5)))
    raise ReleaseError("SourceForge mirror verification failed; the previous default was preserved.")


def set_windows_default(project: str, channel: str, version: str, filename: str, api_key: str) -> None:
    path = "/".join(urllib.parse.quote(s, safe="") for s in
                    (project, "files", "windows", channel, version, filename))
    data = urllib.parse.urlencode({"default": "windows", "api_key": api_key}).encode()
    request = urllib.request.Request("https://sourceforge.net/projects/" + path,
                                     data=data, method="PUT", headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            result = json.load(response)
    except (OSError, ValueError, urllib.error.URLError):
        raise ReleaseError("SourceForge default-download API failed; older installers were preserved.") from None
    defaults = str(result.get("result", {}).get("x_sf", {}).get("default", ""))
    if result.get("error") or "windows" not in [part.strip() for part in defaults.split(",")]:
        raise ReleaseError("SourceForge did not confirm the Windows default; older installers were preserved.")


class OpenSshRemote:
    def __init__(self, project: str, username: str, key: str, directory: Path):
        self.root = f"/home/frs/project/{project}/windows"
        self.address = f"{username}@{HOST}"
        self.directory = directory
        self.key = directory / "identity"
        self.key.write_text(key.replace("\r\n", "\n").rstrip() + "\n", encoding="utf-8")
        self.key.chmod(0o600)
        scan = subprocess.run(["ssh-keyscan", "-T", "20", "-t", "ed25519", HOST], capture_output=True, text=True)
        accepted = []
        for line in scan.stdout.splitlines():
            parts = line.split()
            if len(parts) != 3 or parts[:2] != [HOST, "ssh-ed25519"]:
                continue
            try:
                fingerprint = base64.b64encode(hashlib.sha256(base64.b64decode(parts[2], validate=True)).digest()).decode().rstrip("=")
            except ValueError:
                continue
            if fingerprint == HOST_FINGERPRINT:
                accepted.append(line)
        if not accepted:
            raise ReleaseError("SourceForge SSH host identity did not match its published fingerprint.")
        known = directory / "known_hosts"
        known.write_text("\n".join(accepted) + "\n", encoding="utf-8")
        self.ssh = ["ssh", "-F", "/dev/null", "-i", str(self.key), "-o", "IdentitiesOnly=yes",
                    "-o", "IdentityAgent=none", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
                    "-o", f"UserKnownHostsFile={known}", "-o", "ConnectTimeout=30",
                    "-o", "ServerAliveInterval=30", "-o", "ServerAliveCountMax=10"]

    def batch(self, commands: list[str], missing_ok: bool = False) -> bool:
        result = subprocess.run(["sftp", *self.ssh[1:], "-b", "-", self.address],
                                input="\n".join(commands) + "\n", capture_output=True, text=True)
        if result.returncode:
            # OpenSSH get uses a different diagnostic from rm/rmdir when the
            # first publication has no manifest yet. Other failures stay fatal.
            failure = result.stderr.lower()
            absent = "no such file" in failure or re.search(r'\bfile "[^"\r\n]+" not found\.', failure)
            if missing_ok and absent and "permission denied" not in failure:
                return False
            raise ReleaseError("SourceForge SFTP operation failed. Check account permissions and SSH configuration.")
        return True

    def read_manifest(self, channel: str) -> dict | None:
        local = self.directory / f"{channel}.json"
        if not self.batch([f'get "{self.root}/{channel}/latest.json" "{local}"'], missing_ok=True):
            return None
        if local.stat().st_size > 128 * 1024:
            raise ReleaseError("Remote release manifest is too large.")
        try:
            return json.loads(local.read_text(encoding="utf-8"))
        except (ValueError, OSError):
            raise ReleaseError("Remote release manifest is unreadable; publication was stopped.") from None

    def upload(self, manifest: dict, files: list[Path]) -> None:
        channel, version = manifest["channel"], manifest["version"]
        target = f"{self.root}/{channel}/{version}"
        self.batch([f'-mkdir "{self.root}"', f'-mkdir "{self.root}/{channel}"', f'-mkdir "{target}"'])
        result = subprocess.run(["rsync", "--checksum", "--partial", "--partial-dir=.rsync-partial",
                                 "--delay-updates", "--chmod=F644,D755", "-e", shlex.join(self.ssh),
                                 "--", *(str(f) for f in files), f"{self.address}:{target}/"], capture_output=True)
        if result.returncode:
            raise ReleaseError("SourceForge upload failed; the previous default was preserved. Retry the release job.")

    def write_manifest(self, manifest: dict) -> None:
        channel = manifest["channel"]
        local = self.directory / "next.json"
        local.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        temporary = f"{self.root}/{channel}/latest-{manifest['version']}.tmp"
        self.batch([f'put "{local}" "{temporary}"', f'rename "{temporary}" "{self.root}/{channel}/latest.json"'])

    def prune(self, previous: dict) -> None:
        target = f"{self.root}/{previous['channel']}/{previous['version']}"
        # No recursive deletion, globs, shell commands, or unrelated project paths.
        for item in previous["files"]:
            self.batch([f'rm "{target}/{item["name"]}"'], missing_ok=True)
        self.batch([f'rmdir "{target}"'], missing_ok=True)


def publish(project: str, manifest: dict, files: list[Path], remote,
            verify=verify_download, promote=set_windows_default, api_key: str = "") -> str:
    channel = manifest["channel"]
    previous = remote.read_manifest(channel)
    older = []
    if previous is not None:
        validate_manifest(previous, channel)
        older = previous.get("previous", [])
        if not isinstance(older, list) or len(older) > 100:
            raise ReleaseError("Invalid pending-cleanup list.")
        for item in older:
            validate_manifest(item, channel)
            if version_key(item["version"]) >= version_key(previous["version"]):
                raise ReleaseError("Unsafe pending-cleanup version.")
        if version_key(previous["version"]) > version_key(manifest["version"]):
            raise ReleaseError("A newer version is already published; refusing to downgrade it.")
        if version_key(previous["version"]) == version_key(manifest["version"]) and previous["version"] != manifest["version"]:
            raise ReleaseError("An equivalent version already uses another spelling. Use a new version tag.")
        if previous["version"] == manifest["version"]:
            if previous["files"] != manifest["files"] or previous.get("source") != manifest["source"]:
                raise ReleaseError("This version already has different release bytes. Use a new version tag.")
        else:
            older = older + [{k: v for k, v in previous.items() if k != "previous"}]
    manifest = {**manifest, "previous": older}
    print("Uploading release files over pinned-host SSH.", flush=True)
    remote.upload(manifest, files)
    for item in manifest["files"]:
        verify(download_url(project, channel, manifest["version"], item["name"]), item)
    # Record the verified newer version before promotion, so an API interruption
    # cannot let a subsequently queued older tag replace it.
    remote.write_manifest(manifest)
    filename = next(item["name"] for item in manifest["files"] if item["name"].endswith(".exe"))
    if channel == "stable":
        print("Selecting the verified stable Windows installer.", flush=True)
        promote(project, channel, manifest["version"], filename, api_key)
    manifest["state"] = "published"
    remote.write_manifest(manifest)
    for item in older:
        print("Removing superseded publisher-owned files: " + item["version"], flush=True)
        remote.prune(item)
    manifest["previous"] = []
    remote.write_manifest(manifest)
    return download_url(project, channel, manifest["version"], filename)


def check_config(environment) -> tuple[str, str]:
    required = ["SOURCEFORGE_PROJECT", "SOURCEFORGE_USERNAME", "SOURCEFORGE_SSH_KEY", "SOURCEFORGE_API_KEY"]
    missing = [name for name in required if not environment.get(name)]
    if missing:
        raise ReleaseError("Configure the SourceForge release environment: " + ", ".join(missing))
    project, username = environment["SOURCEFORGE_PROJECT"], environment["SOURCEFORGE_USERNAME"]
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]{1,62}", project) or not NAME.fullmatch(username):
        raise ReleaseError("Invalid SourceForge project slug or username.")
    return project, username


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-config", action="store_true")
    parser.add_argument("--artifact", type=Path)
    parser.add_argument("--notes", type=Path)
    parser.add_argument("--version")
    parser.add_argument("--source")
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    try:
        project, username = check_config(os.environ)
        if args.check_config:
            print("SourceForge release configuration is present.")
            return 0
        if not all((args.artifact, args.notes, args.version, args.source)):
            raise ReleaseError("Artifact, notes, version, and source are required.")
        manifest, files = local_manifest(args.artifact, args.notes, args.version, args.source)
        key = os.environ.pop("SOURCEFORGE_SSH_KEY")
        api_key = os.environ.pop("SOURCEFORGE_API_KEY")
        with tempfile.TemporaryDirectory(prefix="sourceforge-release-") as directory:
            remote = OpenSshRemote(project, username, key, Path(directory))
            url = publish(project, manifest, files, remote, api_key=api_key)
        github_notes = args.notes.with_name("release-notes-sourceforge.md")
        github_notes.write_text(args.notes.read_text(encoding="utf-8") +
                                f"\nDownload this complete offline installer from [SourceForge]({url}).\n\n"
                                "Only the newest installer in each channel is retained. "
                                f"[Latest stable installer](https://sourceforge.net/projects/{project}/files/latest/download).\n", encoding="utf-8")
        if args.github_output:
            with args.github_output.open("a", encoding="utf-8") as output:
                output.write(f"download_url={url}\n")
        print("SourceForge installer published and verified: " + url)
        return 0
    except ReleaseError as error:
        print("Release stopped: " + str(error))
        return 1
    except Exception:
        # Libraries and child processes must not reflect authentication data.
        print("Release stopped by an unexpected transport error; no credential diagnostics were emitted.")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
