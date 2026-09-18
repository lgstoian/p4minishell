#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""newapp.py - scaffold a native applib app component from the whoami sample.

Usage: python tools/newapp.py <name>

Creates samples/<name>/<name>.c + <name>.h + CMakeLists.txt + README.md by
copying samples/whoami/ and renaming its tokens, then prints the two wiring
steps (EXTRA_COMPONENT_DIRS entry + register call). Names must be
`[a-z][a-z0-9_]*` (a shell command name) and must not shadow a built-in or a
batch app — native apps dispatch last, so a collision would silently lose.
"""
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATE = os.path.join(ROOT, "samples", "whoami")
NAME_RE = re.compile(r"^[a-z][a-z0-9_]{0,30}$")


def main():
    if len(sys.argv) != 2 or not NAME_RE.match(sys.argv[1]):
        print("Usage: python tools/newapp.py <name>  ([a-z][a-z0-9_]*)")
        return 2
    name = sys.argv[1]
    upper = name.upper()
    dest = os.path.join(ROOT, "samples", name)
    if os.path.exists(dest):
        print("newapp: samples/%s already exists" % name)
        return 1
    if not os.path.isdir(TEMPLATE):
        print("newapp: template samples/whoami missing")
        return 1

    renames = {"whoami.c": name + ".c", "whoami.h": name + ".h"}
    os.makedirs(dest)
    for entry in sorted(os.listdir(TEMPLATE)):
        src = os.path.join(TEMPLATE, entry)
        if not os.path.isfile(src):
            continue
        with open(src, "r", encoding="utf-8") as fh:
            text = fh.read()
        text = text.replace("whoami", name).replace("WHOAMI", upper)
        with open(os.path.join(dest, renames.get(entry, entry)),
                  "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
    print("created samples/%s/ (%s.c, %s.h, CMakeLists.txt, README.md)"
          % (name, name, name))
    print("wire it up:")
    print("  1. root CMakeLists.txt EXTRA_COMPONENT_DIRS += samples/%s" % name)
    print("  2. main/CMakeLists.txt REQUIRES += %s" % name)
    print('  3. #include "%s.h" in main/native_apps.c and call %s_register()'
          " from native_apps_register()" % (name, name))
    print("  4. idf.py build (zero warnings), flash, run `%s` on the board"
          % name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
