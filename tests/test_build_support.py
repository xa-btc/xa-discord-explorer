"""Offline build-helper checks; all project data lives in temporary directories."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT))
from build_support import backup, console_status

SPEC = importlib.util.spec_from_file_location('sml_build_launcher', PROJECT / '1.build.py')
launcher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(launcher)


class TemporaryProject(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='sml-build-test-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / 'project'
        self.root.mkdir()
        self.messages = []

    def write(self, name, contents='fixture'):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding='utf-8')
        return path

    def snapshot(self):
        return backup.create_project_backup(self.root, self.messages.append)


class BackupTests(TemporaryProject):
    def test_snapshot_verifies_source_and_excludes_runtime_data(self):
        retained = {
            'native/app.cpp': 'int main() {}',
            'third_party/sqlite3.c': 'vendor source',
            'config.json.example': '{"example": true}',
            'build_support/backup.py': '# source',
            'xapr/config.json': '{"private": true}',
            'xapr/inputs/library.dll': 'private binary input',
            'xapr/bin/Obfuscar.dll': 'private tool input',
            'xapr/build-profile/profile.xml': '<profile />',
        }
        excluded = (
            'config.json', 'chat-options.json', 'chat-dm-options.json', 'scan-options.json', '.env',
            '.auth/token.json', 'data/members.db', 'reports/messages.json',
            'logs/build.log', 'backups/older.cpp', 'build/generated.cpp',
            'build-debug/generated.cpp', '__pycache__/example.pyc',
            'native/temporary.obj', 'XA Discord Explorer.exe',
        )
        for name, content in retained.items():
            self.write(name, content)
        for name in excluded:
            self.write(name, 'runtime data must not be copied')
        self.write('README.md', '# Documentation')
        destination = self.snapshot()
        manifest = json.loads((destination / 'backup-manifest.json').read_text(encoding='utf-8'))
        self.assertEqual(set(manifest['files']), set(retained))
        self.assertEqual(manifest['documentation'], '../docs/')
        for name, contents in retained.items():
            with self.subTest(name=name):
                actual = (destination / name).read_bytes()
                self.assertEqual(actual, contents.encode('utf-8'))
                self.assertEqual(manifest['files'][name], hashlib.sha256(actual).hexdigest())
        self.assertEqual((self.root / 'backups/docs/README.md').read_text(encoding='utf-8'), '# Documentation')

    def test_docs_preserve_paths_timestamps_and_removed_documents(self):
        self.write('native/app.cpp')
        documents = ('README.md', 'CHANGELOG.md', 'changelog.txt', 'docs/nested/guide.md', 'docs/assets/example.json')
        for name in documents:
            self.write(name, f'original {name}')
        self.snapshot()
        targets = {name: self.root / 'backups/docs' / name for name in documents}
        for destination in targets.values():
            os.utime(destination, ns=(1_600_000_000_000_000_000, 1_600_000_000_000_000_000))
        before = {name: path.stat().st_mtime_ns for name, path in targets.items()}
        self.snapshot()
        self.assertEqual({name: path.stat().st_mtime_ns for name, path in targets.items()}, before)
        changed = self.write('docs/nested/guide.md', 'updated guide')
        os.utime(changed, ns=(1_700_000_000_000_000_000, 1_700_000_000_000_000_000))
        (self.root / 'changelog.txt').unlink()
        self.snapshot()
        self.assertEqual(targets['docs/nested/guide.md'].read_text(encoding='utf-8'), 'updated guide')
        self.assertEqual(targets['docs/nested/guide.md'].stat().st_mtime_ns, changed.stat().st_mtime_ns)
        self.assertEqual(targets['README.md'].stat().st_mtime_ns, before['README.md'])
        self.assertEqual(targets['changelog.txt'].read_text(encoding='utf-8'), 'original changelog.txt')
        self.assertEqual(targets['changelog.txt'].stat().st_mtime_ns, before['changelog.txt'])

    def test_source_change_or_corrupt_copy_refuses_completed_snapshot(self):
        source = self.write('native/app.cpp', 'original source')
        original_copy = backup.shutil.copy2
        for change_source in (False, True):
            with self.subTest(change_source=change_source):
                source.write_text('original source', encoding='utf-8')

                def corrupt_copy(src, dest):
                    result = original_copy(src, dest)
                    Path(src if change_source else dest).write_text('changed bytes', encoding='utf-8')
                    return result

                with mock.patch.object(backup.shutil, 'copy2', side_effect=corrupt_copy):
                    with self.assertRaisesRegex(ValueError, 'verification failed|Source changed'):
                        self.snapshot()
                snapshots = list((self.root / 'backups').glob('XA Discord Explorer source -*'))
                self.assertTrue(snapshots)
                self.assertTrue(all(path.name.endswith('.partial') for path in snapshots))
                self.assertTrue(all(not (path / 'backup-manifest.json').exists() for path in snapshots))

    def test_docs_failed_copy_keeps_previous_target(self):
        self.write('native/app.cpp')
        source = self.write('README.md', 'previous documentation')
        self.snapshot()
        target = self.root / 'backups/docs/README.md'
        previous_mtime = target.stat().st_mtime_ns
        source.write_text('new documentation', encoding='utf-8')

        def failed_copy(src, dest):
            Path(dest).write_text('incomplete bytes', encoding='utf-8')
            raise OSError('simulated disk failure')

        with mock.patch.object(backup.shutil, 'copy2', side_effect=failed_copy):
            with self.assertRaisesRegex(OSError, 'simulated disk failure'):
                backup.update_docs_backup(self.root, self.root / 'backups', [source], self.messages.append)
        self.assertEqual(target.read_text(encoding='utf-8'), 'previous documentation')
        self.assertEqual(target.stat().st_mtime_ns, previous_mtime)
        self.assertFalse(list(target.parent.glob('.docs-*.tmp')))

    def test_permission_errors_are_bounded_and_can_recover(self):
        source = self.write('native/app.cpp', 'source bytes')
        target = self.base / 'copy.cpp'
        with mock.patch.object(backup.shutil, 'copy2', side_effect=PermissionError('locked')) as copy:
            with mock.patch.object(backup.time, 'sleep') as sleep:
                with self.assertRaises(PermissionError):
                    backup.copy_verified(source, target, self.messages.append)
                self.assertEqual(copy.call_count, 5)
                self.assertEqual(sleep.call_count, 4)
        original_copy = backup.shutil.copy2
        attempts = 0

        def temporarily_locked(src, dest):
            nonlocal attempts
            attempts += 1
            if attempts <= 2:
                raise PermissionError('temporarily locked')
            return original_copy(src, dest)

        with mock.patch.object(backup.shutil, 'copy2', side_effect=temporarily_locked):
            with mock.patch.object(backup.time, 'sleep') as sleep:
                digest = backup.copy_verified(source, target, self.messages.append)
        self.assertEqual(attempts, 3)
        self.assertEqual(sleep.call_count, 2)
        self.assertEqual(target.read_bytes(), source.read_bytes())
        self.assertEqual(digest, hashlib.sha256(source.read_bytes()).hexdigest())

    def test_source_finalize_recovers_from_transient_folder_lock(self):
        self.write('native/app.cpp', 'verified source')
        original_rename = Path.rename
        attempts = 0

        def locked_then_available(source, destination):
            nonlocal attempts
            attempts += 1
            if attempts <= 2:
                raise PermissionError('simulated Windows folder lock')
            return original_rename(source, destination)

        with mock.patch.object(Path, 'rename', locked_then_available), mock.patch.object(backup.time, 'sleep') as sleep:
            destination = self.snapshot()
        self.assertEqual(attempts, 3)
        self.assertEqual(sleep.call_count, 2)
        self.assertEqual((destination / 'native/app.cpp').read_text(), 'verified source')
        manifest = json.loads((destination / 'backup-manifest.json').read_text())
        self.assertEqual(manifest['files']['native/app.cpp'], backup.sha256(destination / 'native/app.cpp'))
        self.assertFalse(list((self.root / 'backups').glob('*.partial')))

    def test_source_finalize_persistent_lock_retains_verified_partial(self):
        self.write('native/app.cpp', 'verified source')
        with mock.patch.object(Path, 'rename', side_effect=PermissionError('locked')) as rename, mock.patch.object(backup.time, 'sleep') as sleep:
            with self.assertRaisesRegex(PermissionError, 'after 8 attempts'):
                self.snapshot()
        self.assertEqual(rename.call_count, 8)
        self.assertEqual(sleep.call_count, 7)
        pending = list((self.root / 'backups').glob('*.partial'))
        self.assertEqual(len(pending), 1)
        self.assertEqual((pending[0] / 'native/app.cpp').read_text(), 'verified source')
        self.assertTrue((pending[0] / 'backup-manifest.json').is_file())
        self.assertTrue(any('build refused' in message for message in self.messages))

    def test_source_finalize_never_overwrites_existing_destination(self):
        pending = self.root / 'pending.partial'
        destination = self.root / 'complete'
        pending.mkdir()
        destination.mkdir()
        (destination / 'previous').write_text('keep')
        with mock.patch.object(Path, 'rename') as rename:
            with self.assertRaises(FileExistsError):
                backup.finalize_source_backup(pending, destination, self.messages.append)
        rename.assert_not_called()
        self.assertEqual((destination / 'previous').read_text(), 'keep')
        self.assertTrue(pending.is_dir())

    def test_symlink_source_is_rejected_when_supported(self):
        external = self.base / 'external.cpp'
        external.write_text('outside project', encoding='utf-8')
        try:
            (self.root / 'linked.cpp').symlink_to(external)
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f'Symlink creation unavailable: {exc}')
        with self.assertRaisesRegex(ValueError, 'link or reparse point'):
            self.snapshot()
        self.assertEqual(external.read_text(encoding='utf-8'), 'outside project')

    @unittest.skipUnless(os.name == 'nt', 'Windows junction check')
    def test_junction_backup_destination_is_rejected(self):
        import _winapi
        if not hasattr(_winapi, 'CreateJunction'):
            self.skipTest('Python has no junction creation helper')
        external = self.base / 'external'
        external.mkdir()
        self.write('native/app.cpp')
        try:
            _winapi.CreateJunction(str(external), str(self.root / 'backups'))
        except OSError as exc:
            self.skipTest(f'Junction creation unavailable: {exc}')
        with self.assertRaisesRegex(ValueError, 'link or reparse point'):
            self.snapshot()
        self.assertEqual(list(external.iterdir()), [])


@unittest.skipUnless(os.name == 'nt', 'Launcher requires Windows')
class LauncherTests(TemporaryProject):
    def setUp(self):
        super().setUp()
        for required in ('CMakeLists.txt', 'native/app.cpp', 'third_party/sqlite3.c'):
            self.write(required)
        self.cmake = self.base / 'cmake.exe'
        self.cmake.write_bytes(b'fixture, never executed')
        self.cmake.with_name('ctest.exe').write_bytes(b'fixture, never executed')
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(launcher, 'ROOT', self.root).start()
        mock.patch.object(launcher, 'find_cmake', return_value=self.cmake).start()
        self.process = mock.patch.object(launcher.subprocess, 'Popen', side_effect=AssertionError('Native execution forbidden')).start()

    def test_dry_run_writes_no_backups_or_logs_and_starts_no_native_process(self):
        before = {path.relative_to(self.root) for path in self.root.rglob('*')}
        with mock.patch.object(launcher, 'create_project_backup') as create_backup:
            with contextlib.redirect_stdout(io.StringIO()) as output:
                result = launcher.main(['--dry-run', '--no-pause'])
        self.assertEqual(result, 0, output.getvalue())
        create_backup.assert_not_called()
        self.process.assert_not_called()
        self.assertEqual({path.relative_to(self.root) for path in self.root.rglob('*')}, before)
        self.assertIn('DRY RUN COMPLETE', output.getvalue())
        self.assertIn('--output-on-failure', output.getvalue())

    def test_backup_failure_prevents_cmake_and_is_logged(self):
        with mock.patch.object(launcher, 'create_project_backup', side_effect=OSError('fixture backup failed')):
            with contextlib.redirect_stdout(io.StringIO()) as output:
                result = launcher.main(['--no-pause'])
        self.assertEqual(result, 1)
        self.process.assert_not_called()
        logs = list((self.root / 'logs').glob('build-*.log'))
        self.assertEqual(len(logs), 1)
        contents = logs[0].read_text(encoding='utf-8')
        self.assertIn('fixture backup failed', contents)
        self.assertNotIn('\x1b[', contents)
        self.assertIn('BUILD FAILED', output.getvalue())


class ConsoleTests(unittest.TestCase):
    class TtyStream(io.StringIO):
        def isatty(self):
            return True

    def test_redirected_output_is_plain_even_with_existing_ansi(self):
        stream = io.StringIO()
        with mock.patch.dict(os.environ, {}, clear=True):
            console_status.print_status('\x1b[92m[OK] success\x1b[0m', file=stream)
        self.assertEqual(stream.getvalue(), '[OK] success\n')

    def test_tty_status_colors_and_no_color_override(self):
        cases = (
            ('[2/6] Backup', 'progress'),
            ('[OK] Source backup verified', 'success'),
            ('[WARN] Tests skipped', 'warning'),
            ('[ERROR] BUILD FAILED', 'error'),
            ('100% tests passed, 0 tests failed out of 3', 'success'),
        )
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(console_status.os, 'name', 'posix'):
            for message, kind in cases:
                with self.subTest(message=message):
                    stream = self.TtyStream()
                    console_status.print_status(message, file=stream)
                    self.assertEqual(stream.getvalue(), console_status.COLORS[kind] + message + console_status.RESET + '\n')
            with mock.patch.dict(os.environ, {'NO_COLOR': ''}):
                stream = self.TtyStream()
                console_status.print_status('[ERROR] BUILD FAILED', file=stream)
                self.assertEqual(stream.getvalue(), '[ERROR] BUILD FAILED\n')
        self.assertIsNone(console_status.status_kind('0 Warning(s)'))
        self.assertIsNone(console_status.status_kind('0 Error(s)'))


if __name__ == '__main__':
    unittest.main()
