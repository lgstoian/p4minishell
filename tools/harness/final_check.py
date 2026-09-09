import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

checks = [
    # Phase 1: batch @-specifiers (parsed in batch.c, not ansi.c)
    ('components/batch/batch.c', '@POS:', '"POS:"'),
    ('components/batch/batch.c', '@CLEAR', '"CLEAR"'),
    ('components/batch/batch.c', '@SAVE/@RESTORE', '"RESTORE"'),
    ('components/batch/batch.c', '@ALTON/@ALTOFF', '"ALTOFF"'),
    ('components/batch/batch.c', '@CURSON/@CURSOFF', '"CURSOFF"'),
    ('components/batch/batch.c', '@SCROLL:', '"SCROLL:"'),
    
    # Phase 2: draw command (lives in tui_commands.c since the v0.35.3 split)
    ('components/command/tui_commands.c', 'draw box', 'shell_command_draw'),
    ('components/command/tui_commands.c', 'draw text', 'draw text'),
    ('components/command/tui_commands.c', 'draw line', 'draw line'),
    ('components/command/tui_commands.c', 'draw fill', 'draw fill'),
    ('components/command/tui_commands.c', 'draw clear', 'draw clear'),
    ('components/command/tui_commands.c', 'draw save/restore', 'argv[1], "save"'),
    ('components/command/tui_commands.c', 'draw cursor', 'draw cursor'),
    ('components/command/tui_commands.c', 'draw alt-screen', 'draw alt-screen'),

    # Phase 2: anchor command
    ('components/command/tui_commands.c', 'anchor', 'shell_command_anchor'),
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