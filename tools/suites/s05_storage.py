# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Storage suite: the DOS-style filesystem surface on the SD card.

Exercises write/type/append, copy/move/ren, the recycle bin (del / undelete /
trash), directory navigation and the `dir` switches, tree, xcopy /s, the text
tools (find / findstr / fc / comp / sort / more), attributes, label, chkdsk,
`sd info`, `disk detail`, and a recursive cleanup.

Everything runs under a scratch directory ``sd:/P4TEST`` using unique temp
file names so a run can never collide with existing user files. The scratch
tree is removed at the end with ``rd /s /p`` plus the interactive ``YES``
confirmation (recursive ``rd`` has no ``/q`` switch: it always confirms).
"""
import re
import time

from p4test.asserts import Checklist

NAME = "storage"
TAGS = ["core", "sd"]

D = "sd:/P4TEST"


def _confirm_destructive(dev, cmd, timeout=90.0):
    """Run a command that asks ``Type YES to continue:`` and answer it.

    Recursive ``del``/``rd`` (and ``format``/``disk``) gate on the exact
    confirmation word. There is no non-interactive switch, so we drive the
    prompt deliberately: send the command, wait for the prompt, answer YES,
    then marker-sync the worker back to idle.
    """
    session = dev.session
    session.reset_input()
    session.write_line(cmd)
    time.sleep(0.6)
    head = session.read_until(b"to continue:", 10.0)
    if b"to continue:" in head:
        session.reset_input()
        session.write_line("YES")
        time.sleep(0.5)
    session.read_for(2.0)
    return dev.run("echo P4STORAGE-SYNC", timeout=timeout)


def run(dev, ctx):
    c = Checklist(NAME)
    stamp = "%05d" % (int(time.time()) % 100000)

    # ---- scratch directory -------------------------------------------
    out = dev.run("md %s" % D)
    if "Created directory" not in out:
        # A previous aborted run left the scratch tree behind.
        _confirm_destructive(dev, "rd /s /p %s" % D)
        out = dev.run("md %s" % D)
    c.expect("md creates scratch dir", "Created directory", out)

    # ---- write / append / type ---------------------------------------
    fw = "%s/P4WR%s.TXT" % (D, stamp)
    out = dev.run('write %s "P4WRITE-%s"' % (fw, stamp))
    c.expect("write reports path", "Wrote", out)
    out = dev.run("type %s" % fw)
    c.expect("type shows content", "P4WRITE-%s" % stamp, out)
    out = dev.run('append %s "P4APPEND-%s"' % (fw, stamp))
    c.expect("append reports path", "Appended", out)
    out = dev.run("type %s" % fw)
    c.expect("append grew file", "P4APPEND-%s" % stamp, out)

    fc = "%s/P4COPY%s.TXT" % (D, stamp)
    out = dev.run("copy %s %s" % (fw, fc))
    c.expect("copy reports copied", "copied", out)
    out = dev.run("type %s" % fc)
    c.expect("copied content present", "P4WRITE-%s" % stamp, out)

    fm = "%s/P4MOV%s.TXT" % (D, stamp)
    out = dev.run("move %s %s" % (fc, fm))
    c.expect("move reports moved", "Moved", out)
    out = dev.run("if exist %s echo P4MOVESRC" % fc)
    c.expect("move removed source", "P4MOVESRC", out, want=False)

    fr = "%s/P4REN%s.TXT" % (D, stamp)
    out = dev.run("ren %s %s" % (fm, fr))
    c.expect("ren reports rename", "->", out)
    out = dev.run("if exist %s echo P4RENOK" % fr)
    c.expect("ren target exists", "P4RENOK", out)

    # ---- del -> trash -> undelete / restore --------------------------
    ft = "%s/P4TRASH%s.TXT" % (D, stamp)
    dev.run('write %s "P4TRASHLINE-%s"' % (ft, stamp))
    out = dev.run("del %s" % ft)
    c.expect("del moves to trash", "moved to", out)
    out = dev.run("trash list")
    c.expect("trash list shows entry", "P4TRASH%s.TXT" % stamp, out)
    out = dev.run("trash info")
    c.expect("trash info counts entries", "entries:", out)
    out = dev.run("undelete P4TRASH%s.TXT" % stamp)
    c.expect("undelete restores", "restored", out)
    out = dev.run("type %s" % ft)
    c.expect("restored content", "P4TRASHLINE-%s" % stamp, out)
    out = dev.run("del /p %s" % ft)
    c.expect("del /p permanent", "deleted", out)
    out = dev.run("if exist %s echo P4TRASHLEFT" % ft)
    c.expect("del /p removed file", "P4TRASHLEFT", out, want=False)

    # ---- md / rd (non-recursive) -------------------------------------
    sub = "%s/P4SUB" % D
    out = dev.run("md %s" % sub)
    c.expect("md subdir", "Created directory", out)
    out = dev.run("rd %s" % sub)
    c.expect("rd removes empty dir", "Removed directory", out)

    # rebuild the subdir with a file for tree / dir /s / xcopy
    dev.run("md %s" % sub)
    dev.run('write %s/NEST%s.TXT "P4NEST-%s"' % (sub, stamp, stamp))

    # ---- cd + dir switches -------------------------------------------
    out = dev.run("cd %s" % D)
    c.expect("cd reports path", "P4TEST", out)
    out = dev.run("cd")
    c.expect("cd with no arg shows cwd", "P4TEST", out)
    dev.run("cd sd:/")

    out = dev.run("dir %s /b" % D)
    c.expect("dir /b names", "P4WR%s.TXT" % stamp, out)
    out = dev.run("dir %s /w" % D)
    c.expect("dir /w marks dirs", "[P4SUB]", out)
    out = dev.run("dir %s /s" % D)
    c.expect("dir /s recurses", "NEST%s.TXT" % stamp, out)
    c.expect("dir /s grand total", "Total files listed:", out)
    out = dev.run("dir %s" % D)
    c.expect("dir default volume line", "Volume in drive", out)

    # ---- tree --------------------------------------------------------
    out = dev.run("tree %s /F" % D)
    c.expect("tree lists subdir file", "NEST%s.TXT" % stamp, out)
    c.expect("tree summary", "file(s)", out)

    # ---- xcopy /s ----------------------------------------------------
    xd = "%s/P4XCOPY" % D
    out = dev.run("xcopy %s %s /s /i" % (sub, xd), timeout=40)
    c.expect("xcopy reports copied", "file(s) copied", out)
    out = dev.run("if exist %s/NEST%s.TXT echo P4XCOPYOK" % (xd, stamp))
    c.expect("xcopy copied nested file", "P4XCOPYOK", out)

    # ---- find: text search + discovery ------------------------------
    fn = "%s/P4NEEDLE%s.TXT" % (D, stamp)
    dev.run('write %s "needle-P4FIND-%s"' % (fn, stamp))
    out = dev.run("find P4FIND-%s %s /C" % (stamp, fn))
    c.expect("find text count", "find: 1 line(s)", out)
    out = dev.run("find P4FIND-%s %s /N" % (stamp, fn))
    c.expect("find /n numbers line", "[1]", out)
    out = dev.run("find %s /NAME:*.TXT /B" % D)
    c.expect("find discovery bare path", "P4NEEDLE%s.TXT" % stamp, out)
    out = dev.run("find %s /NAME:*.TXT" % D)
    c.expect("find discovery summary", "find:", out)

    # ---- findstr -----------------------------------------------------
    out = dev.run("findstr P4FIND-%s %s && echo P4FINDSTROK" % (stamp, fn))
    c.expect("findstr match ok", "P4FINDSTROK", out)
    out = dev.run('findstr /C:"no-such-token-%s" %s || echo P4FINDSTRMISS' % (stamp, fn))
    c.expect("findstr miss sets errorlevel", "P4FINDSTRMISS", out)

    # ---- fc ----------------------------------------------------------
    fdiff = "%s/P4DIFF%s.TXT" % (D, stamp)
    dev.run('write %s "P4DIFF-%s"' % (fdiff, stamp))
    out = dev.run("fc %s %s" % (fn, fdiff))
    c.expect("fc reports difference", "difference(s)", out)
    out = dev.run("fc %s %s" % (fn, fn))
    c.expect("fc identical", "FC: no differences encountered", out)

    # ---- comp --------------------------------------------------------
    # A user/shipped alias (`ALIASES.BAT` maps `COMP=COMPANION.BAT`) would expand
    # at the prompt and launch the Companion instead of the built-in compare, so
    # drop it for this check (DOSKEY macros legitimately shadow internal names).
    dev.run("unalias comp")
    out = dev.run("comp %s %s" % (fn, fdiff))
    c.expect("comp reports mismatch", "mismatch", out)
    out = dev.run("comp %s %s" % (fn, fn))
    c.expect("comp identical", "Files compare OK", out)

    # ---- sort --------------------------------------------------------
    fs = "%s/P4SORT%s.TXT" % (D, stamp)
    dev.run('write %s P4SORTC' % fs)
    dev.run('append %s P4SORTA' % fs)
    dev.run('append %s P4SORTB' % fs)
    out = dev.run("sort %s" % fs)
    ia, ib, ic = (out.find("P4SORTA"), out.find("P4SORTB"), out.find("P4SORTC"))
    c.check("sort orders lines", 0 <= ia < ib < ic, "indices a=%d b=%d c=%d" % (ia, ib, ic))
    out = dev.run("sort %s /U" % fs)
    c.expect("sort /u keeps unique lines", "P4SORTA", out)

    # ---- more (short file, no paging) --------------------------------
    out = dev.run("more %s" % fs)
    c.expect("more prints content", "P4SORTB", out)

    # ---- touch / attrib ----------------------------------------------
    ftouch = "%s/P4TOUCH%s.TXT" % (D, stamp)
    out = dev.run("touch %s" % ftouch)
    c.expect("touch creates file", "Touched", out)
    out = dev.run("attrib +R %s" % ftouch)
    c.expect("attrib sets +R", "+R set for", out)
    out = dev.run("attrib %s" % ftouch)
    c.expect("attrib listing shows R", "R--", out)
    out = dev.run("attrib -R %s" % ftouch)
    c.expect("attrib clears R", "-R set for", out)

    # ---- label (show, set, restore) ----------------------------------
    out = dev.run("label")
    c.expect("label shows volume", "label:", out)
    match = re.search(r"label:\s*([^\r\n]+)", out)
    original_label = match.group(1).strip() if match else ""
    out = dev.run("label P4REGRESS")
    c.expect("label sets volume", 'volume label set to "P4REGRESS"', out)
    if original_label and original_label != "(no volume label)":
        out = dev.run("label %s" % original_label)
        c.expect("label restored", "volume label set to", out)
    else:
        c.note("volume had no label; left P4REGRESS in place (no clear form)")

    # ---- chkdsk ------------------------------------------------------
    out = dev.run("chkdsk", timeout=60)
    c.expect("chkdsk reports total", "total disk space", out)
    c.expect("chkdsk reports free", "available", out)
    out = dev.run("chkdsk /F", timeout=90)
    c.expect("chkdsk /f scans", "file(s)", out)

    # ---- sd info / disk detail ---------------------------------------
    out = dev.run("sd info", timeout=40)
    c.expect("sd info mount point", "sd.mount_point:", out)
    c.expect("sd info free space", "sd.fs_free:", out)
    out = dev.run("disk detail", timeout=40)
    c.expect("disk detail number", "disk.number:", out)
    c.expect("disk detail capacity", "disk.capacity:", out)

    # ---- cleanup: recursive remove with confirmation -----------------
    _confirm_destructive(dev, "rd /s /p %s" % D)
    out = dev.run("if exist %s echo P4DIRSTILL" % D)
    c.expect("scratch dir removed", "P4DIRSTILL", out, want=False)

    return c
