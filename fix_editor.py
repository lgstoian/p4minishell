import re

with open(r'D:\Code\new\p4minishell\editor.md', 'r', encoding='utf-8') as f:
    content = f.read()

old = "4. **Symbols** - every printable ASCII character including the shell-critical\n   `|`, `^`, `~`, and `` ` ``; its `Nav` button switches to the Nav page."

new = "4. **Symbols** - every printable ASCII character including the shell-critical\n   `|`, `^`, `~`, and `` ` ``; its `Nav` button switches to the Nav page.\n   When `P4_CONFIG_GFX_FONT_EXTENDED=1` (default), the full CP437 range\n   0x80-0xFF is also available: box-drawing (┌┐└┘├┤┬┴┼), accented letters\n   (ÇüéâäàåçêëèïîìÄÅÉæÆôöòûùÿÖÜ¢£¥₧ƒ), and symbols (±×÷≈°∙√ⁿ²■)."

content = content.replace(old, new)

with open(r'D:\Code\new\p4minishell\editor.md', 'w', encoding='utf-8') as f:
    f.write(content)

print('Done')