import os

checks = [
    ('File browser surface', 'components/modal/modal_surf.c', 'filebrowser_surface'),
    ('File browser API', 'components/modal/modal_surf.h', 'modal_filebrowser_run'),
    ('File browser cmd', 'components/command/command.c', 'shell_command_browse'),
    ('Browse dispatch', 'components/command/command.c', 'shell_text_equals_ignore_case(argv[0], "browse")'),
    ('Modal runtime', 'components/modal/modal.c', 'modal_surface_run'),
    ('Dialog/List/Ask', 'components/modal/modal_surf.c', 'dialog_surface'),
]

for name, file_path, search in checks:
    full_path = os.path.join('D:\\p4minishell', file_path)
    if os.path.exists(full_path):
        with open(full_path, 'r', encoding='latin-1') as f:
            content = f.read()
        if search in content:
            print('OK:', name)
        else:
            print('MISSING:', name, '(' + search + ')')
    else:
        print('FILE NOT FOUND:', file_path)