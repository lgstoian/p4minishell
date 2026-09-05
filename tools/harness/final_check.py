import os

checks = [
    # Phase 1: ANSI/VT escape sequences
    ('components/ansi/ansi.c', 'Truecolor (38;2;r;g;b)', '38;2;'),
    ('components/ansi/ansi.c', '256-color (38;5;n)', '38;5;'),
    ('components/ansi/ansi.c', '@POS:', '@POS:'),
    ('components/ansi/ansi.c', '@CLEAR', '@CLEAR'),
    ('components/ansi/ansi.c', '@SAVE/@RESTORE', '@SAVE'),
    ('components/ansi/ansi.c', '@ALTON/@ALTOFF', '@ALTON'),
    ('components/ansi/ansi.c', '@CURSON/@CURSOFF', '@CURSON'),
    ('components/ansi/ansi.c', '@SCROLL:', '@SCROLL:'),
    
    # Phase 2: draw command
    ('components/command/command.c', 'draw box', 'shell_command_draw'),
    ('components/command/command.c', 'draw text', 'draw text'),
    ('components/command/command.c', 'draw line', 'draw line'),
    ('components/command/command.c', 'draw fill', 'draw fill'),
    ('components/command/command.c', 'draw clear', 'draw clear'),
    ('components/command/command.c', 'draw save/restore', 'draw save'),
    ('components/command/command.c', 'draw cursor', 'draw cursor'),
    ('components/command/command.c', 'draw alt-screen', 'draw alt-screen'),
    
    # Phase 2: anchor command
    ('components/command/command.c', 'anchor', 'shell_command_anchor'),
    ('components/command/command.c', 'anchor dispatch', 'shell_text_equals_ignore_case(argv[0], "anchor")'),
    
    # Phase 2: modal integration
    ('components/command/command.c', 'modal ops table', 'modal_is_active'),
    ('components/command/command.c', 'shell_launch_app', 'shell_launch_app'),
    
    # Phase 2: batch ansi @-specifiers
    ('components/batch/batch.c', 'batch @POS:', 'POS:'),
    ('components/batch/batch.c', 'batch @CLEAR', 'CLEAR'),
    ('components/batch/batch.c', 'batch @SAVE', 'SAVE'),
    ('components/batch/batch.c', 'batch @ALTON', 'ALTON'),
    ('components/batch/batch.c', 'batch @FG256', 'FG256:'),
    ('components/batch/batch.c', 'batch @FGRGB', 'FGRGB:'),
]

for file_path, name, search in checks:
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