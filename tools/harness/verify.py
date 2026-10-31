# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def read(*parts):
    with open(os.path.join(ROOT, *parts), 'r', encoding='latin-1') as f:
        return f.read()


command_c = read('components', 'command', 'command.c')
tui_commands_c = read('components', 'command', 'tui_commands.c')

checks = [
    ('modal_is_active in ops', command_c, 'modal_is_active'),
    ('modal_handle_usb_key in ops', command_c, 'modal_handle_usb_key'),
    ('modal_handle_serial_line in ops', command_c, 'modal_handle_serial_line'),
    ('shell_launch_app', command_c, 'shell_launch_app'),
    ('shell_command_draw impl', tui_commands_c, 'shell_command_draw'),
    ('shell_command_anchor impl', tui_commands_c, 'shell_command_anchor'),
    ('draw dispatch', command_c, 'shell_text_equals_ignore_case(argv[0], "draw")'),
    ('anchor dispatch', command_c, 'shell_text_equals_ignore_case(argv[0], "anchor")'),
]

for name, content, search in checks:
    if search in content:
        print('OK:', name)
    else:
        print('MISSING:', name)

# The host-only gates: machine-readable comments, the breadcrumb map, and the
# false-green guard. They are the authoritative checks; the symbol probes above
# are the legacy spot checks.
missing = sum(1 for _n, content, search in checks if search not in content)
gates = [
    ('comment index', 'comment_index.py'),
    ('breadcrumbs', 'breadcrumbs.py'),
    ('green guard', 'green_guard.py'),
]
failed = 0
for label, script in gates:
    rc = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', script)],
                        cwd=ROOT).returncode
    print('GATE %s: %s' % (label, 'OK' if rc == 0 else 'FAIL'))
    if rc != 0:
        failed += 1

# Harness marker self-check: a prompt, the transcript echo, and the tag output
# may share a line (the serial console and the UI render prompts independently),
# and _marked() must still find the tag.
try:
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    from p4test.session import DeviceSession
    tag = 'P4TAG1X1'
    cases = [
        ('PS \\> echo %s' % tag, False),
        ('PS /sdcard> PS \\> echo %s' % tag, False),
        ('PS \\> echo %s%s' % (tag, tag), True),
        ('PS /sdcard> %s' % tag, True),
        (tag, True),
    ]
    bad = [text for text, want in cases if DeviceSession._marked(text, tag) != want]
    print('GATE harness marker: %s' % ('OK' if not bad else 'FAIL %r' % bad))
    if bad:
        failed += 1
except Exception as exc:  # noqa: BLE001
    print('GATE harness marker: FAIL (%s)' % exc)
    failed += 1

print('RESULT %s' % ('OK' if missing == 0 and failed == 0 else 'FAIL'))
sys.exit(1 if missing or failed else 0)