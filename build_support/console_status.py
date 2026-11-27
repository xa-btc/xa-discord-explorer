"""XA build colors: blue stages, green success, orange warnings, red errors."""
import builtins
import os
import re
import sys

COLORS = {'error': '\x1b[91m', 'warning': '\x1b[38;2;255;165;0m',
          'success': '\x1b[92m', 'progress': '\x1b[94m'}
RESET = '\x1b[0m'
ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')


def plain_text(text):
    return ANSI.sub('', text)


def status_kind(text):
    line = plain_text(text).strip()
    if re.fullmatch(r'0 (?:Warning|Error)\(s\)', line, re.IGNORECASE):
        return None
    if re.search(r'\bFAILED\b|\bFAIL:|\[ERROR\]|\[CRITICAL ERROR\]|^error\b|: error\b|^Traceback', line, re.IGNORECASE):
        # CTest's successful summary includes "0 tests failed".
        if not re.search(r'100% tests passed, 0 tests failed', line, re.IGNORECASE):
            return 'error'
    if re.search(r'\bWARNING\b|^warn:|\[WARN\]|\[RETRY\]|\b[1-9][0-9]* Warning\(s\)', line, re.IGNORECASE):
        return 'warning'
    if re.search(r'\bSUCCESSFUL\b|\[OK\]|^PASS:|^Build succeeded|100% tests passed', line, re.IGNORECASE):
        return 'success'
    if re.match(r'\[\d+/\d+\]|\[TIME\]|[=-]{5,}|XA Discord Explorer -', line):
        return 'progress'
    return None


def color_enabled(stream):
    if 'NO_COLOR' in os.environ or os.environ.get('TERM') == 'dumb' or not getattr(stream, 'isatty', lambda: False)():
        return False
    if os.name != 'nt':
        return True
    try:
        import ctypes
        import msvcrt
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.GetConsoleMode.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
        kernel.GetConsoleMode.restype = wintypes.BOOL
        kernel.SetConsoleMode.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        kernel.SetConsoleMode.restype = wintypes.BOOL
        handle = msvcrt.get_osfhandle(stream.fileno())
        mode = wintypes.DWORD()
        return bool(kernel.GetConsoleMode(handle, ctypes.byref(mode)) and kernel.SetConsoleMode(handle, mode.value | 0x0004))
    except (AttributeError, OSError, ValueError):
        return False


def print_status(*values, sep=' ', end='\n', file=None, flush=False):
    stream = sys.stdout if file is None else file
    text = (' ' if sep is None else sep).join(str(value) for value in values)
    if not color_enabled(stream):
        return builtins.print(plain_text(text), end=end, file=stream, flush=flush)
    lines = []
    for line in text.splitlines(keepends=True):
        kind = status_kind(line)
        if kind and '\x1b[' not in line:
            body = line.rstrip('\r\n')
            line = COLORS[kind] + body + RESET + line[len(body):]
        lines.append(line)
    builtins.print(''.join(lines), end=end, file=stream, flush=flush)
