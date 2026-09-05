with open('components/command/command.c', 'r', encoding='latin-1') as f:
    content = f.read()

checks = [
    ('modal_is_active in ops', 'modal_is_active'),
    ('modal_handle_usb_key in ops', 'modal_handle_usb_key'),
    ('modal_handle_serial_line in ops', 'modal_handle_serial_line'),
    ('shell_launch_app', 'shell_launch_app'),
    ('shell_command_draw impl', 'static bool shell_command_draw'),
    ('shell_command_anchor impl', 'static bool shell_command_anchor'),
    ('draw dispatch', 'shell_text_equals_ignore_case(argv[0], "draw")'),
    ('anchor dispatch', 'shell_text_equals_ignore_case(argv[0], "anchor")'),
]

for name, search in checks:
    if search in content:
        print('OK:', name)
    else:
        print('MISSING:', name)