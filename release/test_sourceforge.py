import copy
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
import urllib.error
from types import SimpleNamespace
from unittest.mock import patch

import sourceforge as sf


class MemoryRemote:
    def __init__(self, current=None):
        self.current = copy.deepcopy(current)
        self.events = []

    def read_manifest(self, channel):
        return copy.deepcopy(self.current)

    def upload(self, manifest, files):
        self.events.append("upload")

    def write_manifest(self, manifest):
        self.current = copy.deepcopy(manifest)
        self.events.append("write:" + manifest["state"])

    def prune(self, previous):
        self.events.append("prune:" + previous["version"])


class PublishingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.exe = self.root / "application-1.2.3-setup.exe"
        self.exe.write_bytes(b"installer fixture")
        self.notes = self.root / "release-notes.md"
        self.notes.write_text("Release notes\n", encoding="utf-8")
        Path(str(self.exe) + ".sha256").write_text(sf.sha256(self.exe) + "  " + self.exe.name + "\n", encoding="utf-8")
        self.manifest, self.files = sf.local_manifest(self.exe, self.notes, "1.2.3", "a" * 40)

    def older(self, version="1.2.2"):
        return {**copy.deepcopy(self.manifest), "version": version, "state": "published"}

    def run_publish(self, remote, **kwargs):
        def verify(url, item):
            remote.events.append("verify:" + item["name"])
        def promote(*args):
            remote.events.append("promote")
        return sf.publish("test-project", self.manifest, self.files, remote,
                          verify=kwargs.get("verify", verify), promote=kwargs.get("promote", promote))

    def test_uploads_and_verifies_before_promotion_then_prunes(self):
        remote = MemoryRemote(self.older())
        url = self.run_publish(remote)
        self.assertIn("/windows/stable/1.2.3/", url)
        self.assertLess(remote.events.index("verify:release-notes.md"), remote.events.index("promote"))
        self.assertLess(remote.events.index("promote"), remote.events.index("prune:1.2.2"))
        self.assertEqual(remote.current["previous"], [])
        self.assertEqual(remote.current["state"], "published")

    def test_failed_mirror_does_not_promote_or_remove_old_files(self):
        remote = MemoryRemote(self.older())
        def fail(*args):
            raise sf.ReleaseError("verification failed")
        with self.assertRaises(sf.ReleaseError):
            self.run_publish(remote, verify=fail)
        self.assertFalse(any(e == "promote" or e.startswith("prune:") for e in remote.events))
        self.assertEqual(remote.current["version"], "1.2.2")

    def test_failed_promotion_preserves_older_files_and_blocks_downgrade(self):
        remote = MemoryRemote(self.older())
        def fail(*args):
            raise sf.ReleaseError("API unavailable")
        with self.assertRaises(sf.ReleaseError):
            self.run_publish(remote, promote=fail)
        self.assertEqual(remote.current["state"], "verified")
        self.assertFalse(any(e.startswith("prune:") for e in remote.events))
        with self.assertRaises(sf.ReleaseError):
            sf.publish("test-project", self.older(), self.files, remote)

    def test_retry_finishes_pending_cleanup(self):
        previous = {**self.older("1.2.3"), "state": "verified", "previous": [self.older()]}
        remote = MemoryRemote(previous)
        self.run_publish(remote)
        self.assertIn("prune:1.2.2", remote.events)

    def test_old_tag_cannot_replace_newer_release(self):
        remote = MemoryRemote(self.older("1.3.0"))
        with self.assertRaises(sf.ReleaseError):
            self.run_publish(remote)
        self.assertEqual(remote.events, [])

    def test_same_version_cannot_change_published_bytes(self):
        previous = self.older("1.2.3")
        previous["files"][0]["sha256"] = "b" * 64
        remote = MemoryRemote(previous)
        with self.assertRaises(sf.ReleaseError):
            self.run_publish(remote)
        self.assertEqual(remote.events, [])

    def test_prerelease_never_changes_stable_default(self):
        self.manifest["channel"] = "prerelease"
        self.manifest["version"] = "1.3.0-rc.1"
        remote = MemoryRemote()
        self.run_publish(remote)
        self.assertNotIn("promote", remote.events)

    def test_cleanup_rejects_remote_path_traversal(self):
        previous = self.older()
        previous["previous"] = [self.older("1.2.1")]
        previous["previous"][0]["files"][0]["name"] = "../../other-project/file.exe"
        remote = MemoryRemote(previous)
        with self.assertRaises(sf.ReleaseError):
            self.run_publish(remote)
        self.assertEqual(remote.events, [])

    def test_cleanup_rejects_active_or_newer_version(self):
        previous = self.older()
        previous["previous"] = [self.older("1.2.3")]
        with self.assertRaises(sf.ReleaseError):
            self.run_publish(MemoryRemote(previous))

    def test_rejects_corrupt_local_installer(self):
        self.exe.write_bytes(b"changed bytes")
        with self.assertRaises(sf.ReleaseError):
            sf.local_manifest(self.exe, self.notes, "1.2.3", "a" * 40)

    def test_supports_gnu_binary_checksum_and_crlf(self):
        Path(str(self.exe) + ".sha256").write_bytes((sf.sha256(self.exe) + " *" + self.exe.name + "\r\n").encode())
        sf.local_manifest(self.exe, self.notes, "1.2.3.4", "a" * 40)

    def test_rejects_shell_metacharacters_and_invalid_versions(self):
        for version in ("../1.2.3", "1.2.3;echo", "1.2.3-rc..1", "v1.2.3", "1.2.3\n"):
            with self.subTest(version=version), self.assertRaises(sf.ReleaseError):
                sf.version_key(version)

    def test_version_ordering(self):
        self.assertLess(sf.version_key("1.2.9"), sf.version_key("1.2.10"))
        self.assertLess(sf.version_key("1.3.0-rc.2"), sf.version_key("1.3.0-rc.10"))
        self.assertLess(sf.version_key("1.3.0-rc.10"), sf.version_key("1.3.0"))

    def test_equivalent_version_spelling_cannot_replace_release(self):
        with self.assertRaises(sf.ReleaseError):
            self.run_publish(MemoryRemote(self.older("1.2.3.0")))

    def test_unknown_ssh_host_is_rejected_before_authentication(self):
        scan = SimpleNamespace(stdout='frs.sourceforge.net ssh-ed25519 ' + base64.b64encode(b'unknown host').decode(),returncode=0)
        with patch.object(sf.subprocess,'run',return_value=scan) as run:
            with self.assertRaisesRegex(sf.ReleaseError,'SSH host identity'):
                sf.OpenSshRemote('test-project','fixture','synthetic fixture',self.root)
        self.assertEqual(run.call_count,1)
        self.assertEqual(run.call_args.args[0][0],'ssh-keyscan')

    def test_transport_pins_host_and_disables_agent_and_password_fallback(self):
        public = b'synthetic host fixture'
        pin=base64.b64encode(hashlib.sha256(public).digest()).decode().rstrip('=')
        scan=SimpleNamespace(stdout='frs.sourceforge.net ssh-ed25519 '+base64.b64encode(public).decode(),returncode=0)
        with patch.object(sf.subprocess,'run',return_value=scan), patch.object(sf,'HOST_FINGERPRINT',pin):
            remote=sf.OpenSshRemote('test-project','fixture','synthetic fixture',self.root)
        self.assertIn('StrictHostKeyChecking=yes',remote.ssh)
        self.assertIn('IdentityAgent=none',remote.ssh)
        self.assertIn('BatchMode=yes',remote.ssh)
        self.assertIn('IdentitiesOnly=yes',remote.ssh)

    def test_first_publication_accepts_openssh_missing_manifest_diagnostics(self):
        remote = object.__new__(sf.OpenSshRemote)
        remote.ssh, remote.address = ['ssh'], 'fixture@host'
        remote.directory, remote.root = self.root, '/home/frs/project/test-project/windows'
        for error in ('File "/home/frs/project/test-project/windows/stable/latest.json" not found.',
                      'stat remote: No such file or directory'):
            with self.subTest(error=error), patch.object(sf.subprocess, 'run', return_value=SimpleNamespace(returncode=1, stderr=error)):
                self.assertIsNone(remote.read_manifest('stable'))

    def test_manifest_permission_and_connection_errors_are_not_absence(self):
        remote = object.__new__(sf.OpenSshRemote)
        remote.ssh, remote.address = ['ssh'], 'fixture@host'
        for error in ('Permission denied', 'Connection closed', 'Permission denied\nFile "latest.json" not found.'):
            with self.subTest(error=error), patch.object(sf.subprocess, 'run', return_value=SimpleNamespace(returncode=1, stderr=error)):
                with self.assertRaises(sf.ReleaseError):
                    remote.batch(['get "latest.json"'], missing_ok=True)

    def test_bad_public_mirror_bytes_are_retried_and_rejected(self):
        expected={'name':'fixture.exe','bytes':4,'sha256':hashlib.sha256(b'good').hexdigest()}
        with patch.object(sf.urllib.request,'urlopen',side_effect=lambda *a,**k:io.BytesIO(b'bad!')), patch.object(sf.time,'sleep'):
            with self.assertRaisesRegex(sf.ReleaseError,'mirror verification'):
                sf.verify_download('https://example.invalid/fixture.exe',expected,attempts=2)

    def test_api_key_is_sent_in_https_body_and_never_in_diagnostics(self):
        result=io.BytesIO(json.dumps({'result':{'x_sf':{'default':'windows'}}}).encode())
        with patch.object(sf.urllib.request,'urlopen',return_value=result) as request:
            sf.set_windows_default('test-project','stable','1.2.3','fixture.exe','API fixture')
        call=request.call_args.args[0]
        self.assertTrue(call.full_url.startswith('https://sourceforge.net/'))
        self.assertNotIn('API fixture',call.full_url)
        self.assertEqual(call.method,'PUT')
        with patch.object(sf.urllib.request,'urlopen',side_effect=OSError('API fixture')):
            with self.assertRaises(sf.ReleaseError) as error:
                sf.set_windows_default('test-project','stable','1.2.3','fixture.exe','API fixture')
        self.assertNotIn('API fixture',str(error.exception))

    def test_cleanup_failure_is_retained_for_safe_retry(self):
        remote=MemoryRemote(self.older())
        def fail(previous):
            raise sf.ReleaseError('cleanup failure')
        remote.prune=fail
        with self.assertRaises(sf.ReleaseError):
            self.run_publish(remote)
        self.assertEqual(remote.current['state'],'published')
        self.assertEqual(remote.current['previous'][0]['version'],'1.2.2')

    def test_config_error_never_reflects_credentials(self):
        environment = {"SOURCEFORGE_PROJECT": "invalid/project", "SOURCEFORGE_USERNAME": "fixture",
                       "SOURCEFORGE_SSH_KEY": "private fixture", "SOURCEFORGE_API_KEY": "API fixture"}
        with patch.dict(sf.os.environ, environment, clear=True), patch("sys.argv", ["sourceforge.py", "--check-config"]), patch("sys.stdout", new_callable=io.StringIO) as output:
            self.assertEqual(sf.main(), 1)
        self.assertNotIn("private fixture", output.getvalue())
        self.assertNotIn("API fixture", output.getvalue())

    def test_oversized_file_rejected_before_hashing(self):
        original = Path.stat
        def stat(path, *args, **kwargs):
            result = original(path, *args, **kwargs)
            if path == self.exe:
                values = list(result)
                values[6] = 4100000001
                return os.stat_result(values)
            return result
        with patch.object(Path, "stat", stat), patch.object(sf, "sha256", side_effect=AssertionError("must reject before hashing")):
            with self.assertRaisesRegex(sf.ReleaseError, "size budget"):
                sf.local_manifest(self.exe, self.notes, "1.2.3", "a" * 40)


if __name__ == "__main__":
    unittest.main()
