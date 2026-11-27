############################################################################################################################
#  
#   ___      _ _    _
#  | _ )_  _(_) |__| |
#  | _ \ || | | / _` |
#  |___/\_,_|_|_\__,_|
#  
############################################################################################################################
# XA Discord Explorer Build v0.02
# Back up, configure, build and test the native Windows Release application.
# Core Features: colored timed stages, verified source/docs backups, preview, persistent logs and clear failures.
# Important Note: Close XA Discord Explorer before rebuilding; this script never stops running applications.
# Created by: XA
# Last Updated: 2026-10-16 14:03:01
# Release Notes: v0.02 - Match XA build colors and stages; add verified source and incremental docs backups.
# v0.01 - One-step native build with optional noninteractive execution.
########################################################################################################################

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import traceback
from datetime import datetime

from build_support.backup import create_project_backup
from build_support.console_status import plain_text, print_status

ROOT = Path(__file__).resolve().parent


def find_cmake():
    found = shutil.which('cmake')
    if found:
        return Path(found)
    candidates = [Path(os.environ.get('ProgramFiles', r'C:\Program Files')) / 'CMake/bin/cmake.exe']
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if vswhere.is_file():
        result = subprocess.run([str(vswhere), '-latest', '-products', '*', '-requires',
                                 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                                capture_output=True, text=True, check=True)
        location = result.stdout.strip()
        if location:
            candidates.append(Path(location) / 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe')
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise RuntimeError('CMake was not found. Install CMake 3.24+ and Visual Studio 2022 C++ Build Tools.')


def build(args, log=None):
    def report(message):
        print_status(message, flush=True)
        if log is not None:
            log.write(plain_text(message) + '\n')
            log.flush()

    stage_count = 6
    def stage(number, title, action):
        report(f'\n[{number}/{stage_count}] {title}')
        started = time.monotonic()
        action()
        report(f'  [TIME] {title}: {time.monotonic() - started:.2f}s')

    def run(command):
        command = [str(part) for part in command]
        report('\n> ' + subprocess.list2cmdline(command))
        if args.dry_run:
            return
        with subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, encoding='utf-8', errors='replace') as process:
            for line in process.stdout:
                report(line.rstrip('\n'))
            code = process.wait()
        if code:
            raise RuntimeError(f'Command failed with exit code {code}. See the output above. If the executable is locked, close XA Discord Explorer and retry.')

    started = time.monotonic()
    report('=' * 68)
    report('XA Discord Explorer - Backup -> Configure -> Build -> Test')
    report('=' * 68)
    report(f'Project: {ROOT}')
    report(f'Parallel jobs: {args.jobs}')

    cmake = ctest = None
    def preflight():
        nonlocal cmake, ctest
        if os.name != 'nt':
            raise RuntimeError('This project requires Windows and Visual Studio 2022 C++ Build Tools.')
        for required in ('CMakeLists.txt', 'native/app.cpp', 'third_party/sqlite3.c'):
            if not (ROOT / required).is_file():
                raise FileNotFoundError(f'Required build input missing: {required}')
        cmake = find_cmake()
        ctest = cmake.with_name('ctest.exe')
        if not args.skip_tests and not ctest.is_file():
            fallback = shutil.which('ctest')
            if not fallback:
                raise RuntimeError('CTest was not found. Repair the CMake installation.')
            ctest = Path(fallback)
        report(f'  [OK] CMake: {cmake}')
        if not args.skip_tests:
            report(f'  [OK] CTest: {ctest}')
    stage(1, 'Preflight', preflight)
    stage(2, 'Backup source + documentation', lambda: report('  Preview: verified source snapshot and incremental backups/docs/.')
          if args.dry_run else create_project_backup(ROOT, report))
    stage(3, 'Configure Release x64', lambda: run([cmake, '-S', ROOT, '-B', ROOT / 'build-xa-discord-explorer',
          '-G', 'Visual Studio 17 2022', '-A', 'x64', f'-DXA_DISCORD_EXPLORER_OUTPUT_DIRECTORY={ROOT.as_posix()}']))
    stage(4, 'Build', lambda: run([cmake, '--build', ROOT / 'build-xa-discord-explorer', '--config', 'Release', '--parallel', args.jobs]))
    stage(5, 'Native tests', lambda: report('  [WARN] Native tests skipped by --skip-tests.') if args.skip_tests
          else run([ctest, '--test-dir', ROOT / 'build-xa-discord-explorer', '-C', 'Release', '--output-on-failure']))
    output = ROOT / 'XA Discord Explorer.exe'
    def verify_output():
        if args.dry_run:
            report(f'  Planned output: {output}')
            return
        if not output.is_file() or output.stat().st_size == 0:
            raise RuntimeError('Build returned success but XA Discord Explorer.exe is missing or empty.')
        report(f'  [OK] Executable: {output} ({output.stat().st_size:,} bytes)')
    stage(6, 'Verify output', verify_output)
    report('\n' + '=' * 68)
    report('  DRY RUN COMPLETE - no backups or native build performed' if args.dry_run else '  BUILD SUCCESSFUL')
    report(f'  [TIME] Total: {time.monotonic() - started:.2f}s')
    report('=' * 68)


def main(argv=None):
    parser = argparse.ArgumentParser(description='Build XA Discord Explorer.exe and run its native tests')
    parser.add_argument('--no-pause', action='store_true', help='Exit immediately; suitable for terminals and automation')
    parser.add_argument('--skip-tests', action='store_true', help='Build without running native tests')
    parser.add_argument('--dry-run', action='store_true', help='Preview stages and commands without backups, logs or native build')
    parser.add_argument('--jobs', type=int, default=min(4, os.cpu_count() or 1), help='Parallel build jobs (default: up to 4)')
    args = parser.parse_args(argv)
    if not 1 <= args.jobs <= 64:
        parser.error('--jobs must be between 1 and 64')
    log_path = ROOT / 'logs' / f'build-{datetime.now():%Y-%m-%d_%H-%M-%S_%f}.log'
    result = 1
    try:
        if args.dry_run:
            build(args)
            result = 0
        else:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            with log_path.open('w', encoding='utf-8') as log:
                try:
                    build(args, log)
                    result = 0
                except KeyboardInterrupt:
                    print_status('\n[WARN] Build cancelled.')
                    log.write('\nBuild cancelled.\n')
                except Exception as exc:
                    print_status(f'\n[ERROR] BUILD FAILED: {exc}')
                    log.write(traceback.format_exc())
            print_status(f'Log: {log_path}')
    except KeyboardInterrupt:
        result = 1
        print_status('\n[WARN] Build cancelled.')
    except Exception as exc:
        result = 1
        print_status(f'[ERROR] Build could not complete: {exc}')
        if not args.dry_run:
            try:
                log_path.parent.mkdir(parents=True, exist_ok=True)
                with log_path.open('a', encoding='utf-8') as log:
                    log.write(traceback.format_exc())
                print_status(f'Log: {log_path}')
            except OSError:
                traceback.print_exc()
    finally:
        if not args.no_pause:
            try:
                input('\nPress Enter to close...')
            except (EOFError, KeyboardInterrupt):
                pass
    return result


if __name__ == '__main__':
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(errors='replace')
    sys.exit(main())
