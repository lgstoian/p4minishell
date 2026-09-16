# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
import sys

sys.path.insert(0, "tools")
from shell_session import Shell

sh = Shell()
fails = 0

out = sh.run("list Pick a b c", timeout=6)
print(out[-400:])
# The list modal stays open until it gets a serial selection (1-based),
# a label, or q — close it before moving on, or every later line would be
# swallowed by the still-open modal.
out = sh.run("1", timeout=10)
ok = True
print("[%s] list (serial-select)" % ("OK " if ok else "CHECK"))
print(out[-400:])

out = sh.run("ask Name?", timeout=20)
print("[sent ask, check ASK_RESULT]")
out2 = sh.run("myname", timeout=10)
print(out2[-300:])
out3 = sh.run("echo %ASK_RESULT%", timeout=10)
ok = "myname" in out3
print("[%s] ask serial" % ("OK " if ok else "FAIL"))
if not ok:
    fails += 1
    print(out3[-500:])

for cmd in [
    "write swptest.txt hello-hardening-world",
    "type swptest.txt",
    "copy swptest.txt swptest2.txt",
    "dir *.txt",
    "del swptest.txt",
    "del swptest2.txt",
]:
    try:
        out = sh.run(cmd, timeout=20)
        print("[OK ]", cmd)
    except RuntimeError as e:
        fails += 1
        print("[PANIC]", cmd, e)
        break
print("fails:", fails)
sh.close()
