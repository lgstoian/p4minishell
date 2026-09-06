import sys

sys.path.insert(0, "tools")
from shell_session import Shell

sh = Shell()
CASES = [
    ("calc deg(30.1530)", ["30.25"], 15),
    ("calc deg(45.30)", ["45.5"], 15),
    ("draw box 2 2 20 8 double T", [], 10),
    ("draw line 1 5 80 5 single", [], 10),
    ("draw clear", [], 10),
    ("color", [], 10),
    ("tui status", ["80", "25"], 15),
    ("dialog Hi There /t:3", [], 12),
    ("beep", [], 10),
    ("tone 440 200", [], 10),
    ("audio status", [], 10),
    ("gpio status", [], 15),
    ("adc status", [], 15),
    ("i2c scan", [], 30),
    ("battery", [], 15),
    ("rgb status", [], 10),
    ("dir", [], 20),
    ("sd info", [], 15),
]
fails = 0
for cmd, wants, to in CASES:
    try:
        out = sh.run(cmd, timeout=to)
        missing = [w for w in wants if w not in out]
        status = "OK " if not missing else "MISSING %s" % missing
        if missing:
            fails += 1
        print("[%s] %s" % (status, cmd))
    except RuntimeError as e:
        fails += 1
        print("[PANIC] %s: %s" % (cmd, e))
        break
print("fails:", fails)
sh.close()
