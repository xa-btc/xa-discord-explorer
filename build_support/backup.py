"""Verified source snapshots and incremental documentation backups for this app."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import time
from datetime import datetime
from uuid import uuid4

EXCLUDED_DIRS = {'backups', 'build', 'data', 'downloads', 'inbox', 'logs', 'reports',
                 '.auth', '.research', '.git', '.vs', '.vscode', '.codex', '.claude',
                 '.claude-octopus', '__pycache__', '.venv', 'venv', 'bin', 'obj'}
RUNTIME_FILES = {'config.json', 'chat-options.json', 'chat-dm-options.json', 'scan-options.json'}
GENERATED_SUFFIXES = {'.exe', '.dll', '.pdb', '.obj', '.ilk', '.log', '.pyc', '.cache',
                      '.db', '.db-wal', '.db-shm', '.sqlite', '.sqlite-wal', '.sqlite-shm',
                      '.sqlite3', '.sqlite3-wal', '.sqlite3-shm'}


def reject_link(path):
    """Reject Windows junctions/reparse points as well as symlinks on Python 3.10+."""
    try:
        info = path.lstat()
    except FileNotFoundError:
        return
    if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
        raise ValueError(f'Backup path must not be a link or reparse point: {path}')


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def copy_verified(source, destination, report):
    before = None
    for attempt in range(5):
        try:
            reject_link(source)
            reject_link(destination)
            if before is None:
                before = sha256(source)
            shutil.copy2(source, destination)
            if sha256(destination) != before or sha256(source) != before:
                raise ValueError(f'Source changed or backup verification failed; build refused: {source}')
            return before
        except PermissionError:
            if attempt == 4:
                raise
            delay = 0.25 * 2 ** attempt
            report(f'  [RETRY] Backup access denied for {source.name}; retrying in {delay:g}s ({attempt + 2}/5).')
            time.sleep(delay)
    raise AssertionError('Backup retry loop did not return or raise')


def backup_files(root):
    def walk_error(error):
        raise error
    for directory, dirs, files in os.walk(root, followlinks=False, onerror=walk_error):
        relative_dir = Path(directory).relative_to(root)
        private_inputs = bool(relative_dir.parts and relative_dir.parts[0].casefold() == 'xapr')
        dirs[:] = sorted(name for name in dirs if private_inputs or (
            name.casefold() not in EXCLUDED_DIRS and not name.casefold().startswith('build-')))
        for name in dirs:
            reject_link(Path(directory) / name)
        for name in sorted(files):
            source = Path(directory) / name
            if not private_inputs and (source.suffix.casefold() in GENERATED_SUFFIXES
                    or (relative_dir == Path('.') and name.casefold() in RUNTIME_FILES)
                    or (name.casefold().startswith('.env') and not name.casefold().endswith('.example'))):
                continue
            reject_link(source)
            yield source


def is_document(relative):
    return relative.parts[0].casefold() != 'xapr' and (
        relative.parts[0].casefold() == 'docs' or relative.suffix.casefold() in {'.md', '.markdown'}
        or relative.as_posix().casefold() == 'changelog.txt')


def safe_parent(base, relative):
    reject_link(base)
    base.mkdir(exist_ok=True)
    parent = base
    for part in relative.parent.parts:
        parent /= part
        reject_link(parent)
        parent.mkdir(exist_ok=True)
    return parent


def update_docs_backup(root, backup_root, sources, report):
    target = backup_root / 'docs'
    updated = unchanged = 0
    for source in sources:
        relative = source.relative_to(root)
        if not is_document(relative):
            continue
        parent = safe_parent(target, relative)
        destination = target / relative
        reject_link(destination)
        if destination.is_file() and sha256(source) == sha256(destination):
            unchanged += 1
            continue
        temporary = parent / f'.docs-{uuid4().hex}.tmp'
        try:
            copy_verified(source, temporary, report)
            reject_link(destination)
            os.replace(temporary, destination)
            updated += 1
        finally:
            temporary.unlink(missing_ok=True)
    report(f'  [OK] Docs backup: backups/docs/ ({updated} new/updated, {unchanged} unchanged; removed documents retained).')


def finalize_source_backup(pending, destination, report):
    """Retry transient Windows directory locks without replacing an existing backup."""
    for attempt in range(8):
        reject_link(pending.parent)
        reject_link(pending)
        reject_link(destination)
        if destination.exists():
            raise FileExistsError(f'Backup destination already exists: {destination}')
        try:
            pending.rename(destination)
            return
        except PermissionError as exc:
            if attempt == 7:
                raise PermissionError(
                    f'Windows denied finalizing the source backup after 8 attempts. '
                    f'The copied files are retained at {pending}. '
                    'Release any folder lock or check backup-folder permissions, then retry the build.'
                ) from exc
            delay = min(0.25 * 2 ** attempt, 2.0)
            report(f'  [RETRY] Backup folder rename denied; retrying in {delay:g}s ({attempt + 2}/8).')
            time.sleep(delay)


def create_project_backup(root, report):
    root = root.resolve(strict=True)
    backup_root = root / 'backups'
    reject_link(backup_root)
    backup_root.mkdir(exist_ok=True)
    sources = list(backup_files(root))
    update_docs_backup(root, backup_root, sources, report)
    stem = f'XA Discord Explorer source - {datetime.now():%Y-%m-%d_%H-%M-%S}'
    destination = backup_root / stem
    while destination.exists() or destination.with_name(destination.name + '.partial').exists():
        destination = backup_root / (stem + '-' + uuid4().hex[:8])
    pending = destination.with_name(destination.name + '.partial')
    pending.mkdir()
    inventory = {}
    try:
        for source in sources:
            relative = source.relative_to(root)
            if is_document(relative):
                continue
            copied = pending / relative
            safe_parent(pending, relative)
            inventory[relative.as_posix()] = copy_verified(source, copied, report)
        if not inventory:
            raise ValueError('No source files found; build refused')
        manifest = {'created_at': datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
                    'documentation': '../docs/', 'files': inventory}
        (pending / 'backup-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        finalize_source_backup(pending, destination, report)
    except BaseException:
        report(f'  [WARN] Incomplete source backup retained at {pending}; build refused.')
        raise
    report(f'  [OK] Source backup: {destination.relative_to(root).as_posix()}/ ({len(inventory)} verified files).')
    return destination
