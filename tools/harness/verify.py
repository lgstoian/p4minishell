# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
import os

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
