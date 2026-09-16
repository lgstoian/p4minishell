# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

checks = [
    ('File browser surface', 'components/modal/modal_surf.c', 'fb_surface'),
    ('File browser API', 'components/modal/modal_surf.h', 'modal_filebrowser_run'),
    ('File browser cmd', 'components/command/tui_commands.c', 'shell_command_browse'),
    ('Browse dispatch', 'components/command/command.c', 'shell_text_equals_ignore_case(argv[0], "browse")'),
    ('Modal runtime', 'components/modal/modal.c', 'modal_surface_run'),
    ('Dialog/List/Ask', 'components/modal/modal_surf.c', 'dialog_surface'),
]

for name, file_path, search in checks:
    full_path = os.path.join(ROOT, file_path)
    if os.path.exists(full_path):
        with open(full_path, 'r', encoding='latin-1') as f:
            content = f.read()
        if search in content:
            print('OK:', name)
        else:
            print('MISSING:', name, '(' + search + ')')
    else:
        print('FILE NOT FOUND:', file_path)