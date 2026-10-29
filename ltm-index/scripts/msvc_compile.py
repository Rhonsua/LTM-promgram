#!/usr/bin/env python3
"""Normalize MSVC include diagnostics to UTF-8 for Ninja dependency tracking."""
import os
import re
import subprocess
import sys


def main():
    env = dict(os.environ)
    env["VSLANG"] = "1033"
    # cl uses the console output code page, not necessarily the ANSI code page.
    import ctypes
    codepage = ctypes.windll.kernel32.GetConsoleOutputCP()
    encoding = "cp" + str(codepage or ctypes.windll.kernel32.GetACP())
    prefix = re.compile(r"^(?:Note: including file:|\u6ce8\u610f: \u5305\u542b\u6587\u4ef6:)\s*")
    proc = subprocess.Popen(sys.argv[1:], env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    for raw in proc.stdout:
        line = raw.decode(encoding, errors="replace")
        match = prefix.match(line)
        if match:
            line = "Note: including file: " + line[match.end():]
        sys.stdout.buffer.write(line.encode("utf-8"))
        sys.stdout.buffer.flush()
    return proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
