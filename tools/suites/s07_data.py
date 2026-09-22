# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Data suite: structured stores, persistence and interchange.

Covers the ``db`` record store (create/info/categories/add/get/find/field/
sort/del/purge/export/import), the ``csv`` grid verbs with ``=EXPR``
formulas, portable ``export`` / ``import`` for databases and alarms, ``archive``
create/list/verify/extract, the ``crypt`` lock/unlock round-trip, the
persistent-state verbs (``ini``, ``appconfig``, ``temp``), the ``alarm`` /
``cal`` calendar store, ``json validate|pretty``, ``markdown`` and ``gfind``.

A scratch database name and a scratch ``sd:/P4DATA`` directory are used and
both are cleaned up. ``alarm purge`` is only reached after deleting exactly the
events this suite created.
"""
import re
import time

from p4test.asserts import Checklist

NAME = "data"
TAGS = ["core", "sd"]

DATA = "sd:/P4DATA"


def _confirm_destructive(dev, cmd, timeout=120.0):
    """Answer a ``Type YES to continue:`` prompt, then marker-sync idle."""
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
    return dev.run("echo P4DATA-SYNC", timeout=timeout)


def _ids_for(text, title):
    """Extract the leading record id from ``id|...|title|...`` bare rows."""
    ids = []
    for line in text.splitlines():
        if title in line and "|" in line:
            digits = re.findall(r"\d+", line.split("|", 1)[0])
            if digits:
                ids.append(digits[-1])
    return ids


def run(dev, ctx):
    c = Checklist(NAME)
    stamp = "%05d" % (int(time.time()) % 100000)
    dbname = "P4R%s" % stamp
    dbname2 = "P4R2%s" % stamp
    app = "p4app%s" % stamp
    title = "P4ALARM%s" % stamp
    imp_title = "P4IMP%s" % stamp
    close_ids = []

    dev.run("cd sd:/")
    out = dev.run("md %s" % DATA)
    if "Created directory" not in out:
        _confirm_destructive(dev, "rd /s /p %s" % DATA)
        out = dev.run("md %s" % DATA)
    c.expect("scratch dir created", "Created directory", out)

    # ==================================================================
    # db - record store
    # ==================================================================
    out = dev.run("db create %s /cr:APP /tp:P4RG /vr:1" % dbname)
    c.expect("db create", "db: created", out)
    out = dev.run("db info %s" % dbname)
    c.expect("db info name", "db.name", out)
    c.expect("db info records", "db.records", out)

    out = dev.run('db add %s /cat:1 /key:alice name=Alice;city=Paris' % dbname)
    c.expect("db add first id", "db.added 1", out)
    dev.run('db add %s /cat:1 /key:bob name=Bob;city=Rome' % dbname)
    out = dev.run('db add %s /cat:2 /secret p4secret-%s' % (dbname, stamp))
    c.expect("db add secret id", "db.added 3", out)

    out = dev.run("db categories %s set 7 Family" % dbname)
    c.expect("db category label", "category 7 = Family", out)
    out = dev.run("db categories %s list" % dbname)
    c.expect("db categories list", "Family", out)

    out = dev.run("db get %s 1" % dbname)
    c.expect("db get key", "alice", out)
    c.expect("db get payload", "Alice", out)
    out = dev.run("db get %s 1 /field:name /b" % dbname)
    c.expect("db get single field bare", "Alice", out)
    out = dev.run("db get %s 3" % dbname)
    c.expect("db secret redacted", "secret", out)
    c.expect("db secret hidden", "p4secret-%s" % stamp, out, want=False)
    out = dev.run("db get %s 3 /reveal" % dbname)
    c.expect("db reveal secret", "p4secret-%s" % stamp, out)

    out = dev.run("db find %s /field:city=Rome /sort:name /b" % dbname)
    c.expect("db find field filter", "bob", out)
    c.expect("db find field excludes", "alice", out, want=False)
    out = dev.run("db find %s /sort:name /b" % dbname)
    c.expect("db find sort all", "alice", out)
    out = dev.run("db find %s /key:alice /b" % dbname)
    c.expect("db find by key", "alice", out)
    out = dev.run("db count %s" % dbname)
    c.expect("db count", "db.count", out)

    out = dev.run("db set %s 1 /key:alice2 name=Alice;city=Rome" % dbname)
    c.expect("db set updates", "db: updated", out)

    out = dev.run("db del %s 3" % dbname)
    c.expect("db soft delete", "soft-deleted", out)
    out = dev.run("db purge %s" % dbname)
    c.expect("db purge", "purged", out)
    out = dev.run("db get %s 3 || echo P4DBGONE" % dbname)
    c.expect("db purge removed record", "P4DBGONE", out)

    # native export / import
    out = dev.run("db export %s %s/db_native.txt" % (dbname, DATA))
    c.expect("db native export", "exported", out)
    dev.run("db create %s" % dbname2)
    out = dev.run("db import %s %s/db_native.txt" % (dbname2, DATA))
    c.expect("db native import", "imported", out)

    # portable export / import
    out = dev.run("export db %s csv %s/db.csv" % (dbname, DATA))
    c.expect("export db csv", "row(s)", out)
    out = dev.run("export db %s json %s/db.json" % (dbname, DATA))
    c.expect("export db json", "row(s)", out)
    out = dev.run("export db %s txt %s/db.txt" % (dbname, DATA))
    c.expect("export db txt", "row(s)", out)
    out = dev.run("import db %s csv %s/db.csv" % (dbname2, DATA))
    c.expect("import db csv", "record(s)", out)

    # ==================================================================
    # csv - grid + formulas
    # ==================================================================
    csvf = "%s/sheet.csv" % DATA
    dev.run('write %s "item,qty,price"' % csvf)
    dev.run('append %s "apple,3,0.5"' % csvf)
    dev.run('append %s "berry,2,=R3C2*10"' % csvf)
    out = dev.run("csv rows %s" % csvf)
    c.expect("csv rows", "rows: 3", out)
    out = dev.run("csv cols %s" % csvf)
    c.expect("csv cols", "cols: 3", out)
    out = dev.run("csv cell %s 2 1 /v:CSVCELL" % csvf)
    c.expect("csv cell", "R2C1", out)
    out = dev.run("echo CSVCELL=%CSVCELL%")
    c.expect("csv /v stores field", "CSVCELL=apple", out)
    out = dev.run("csv eval %s /b" % csvf)
    c.expect("csv eval formula", "20", out)
    out = dev.run("csv set %s 3 3 19.95" % csvf)
    c.expect("csv set reports cell", "R3C3 set", out)
    out = dev.run("csv cell %s 3 3 /b" % csvf)
    c.expect("csv set round-trip", "19.95", out)

    # ==================================================================
    # json / markdown
    # ==================================================================
    dev.push_file("%s/obj.json" % DATA, b'{"a": 1, "b": [1, 2, 3]}')
    out = dev.run("json validate %s/obj.json" % DATA)
    c.expect("json validate good", "json: valid", out)
    out = dev.run("json pretty %s/obj.json" % DATA)
    c.expect("json pretty key", '"b"', out)
    dev.push_file("%s/bad.json" % DATA, b'{"a": }')
    out = dev.run("json validate %s/bad.json" % DATA)
    c.expect("json validate bad", "invalid", out)

    dev.push_file("%s/doc.md" % DATA, ("# P4MD%s\n\nHello markdown.\n" % stamp).encode())
    out = dev.run("markdown %s/doc.md" % DATA)
    c.expect("markdown renders file", "P4MD%s" % stamp, out)
    out = dev.run('markdown -e "# P4MDINLINE%s"' % stamp)
    c.expect("markdown -e renders text", "P4MDINLINE%s" % stamp, out)

    # ==================================================================
    # ini / appconfig / temp
    # ==================================================================
    inif = "%s/state.ini" % DATA
    dev.run("ini set %s theme dark" % inif)
    out = dev.run("ini get %s theme" % inif)
    c.expect("ini get", "dark", out)
    out = dev.run("ini list %s" % inif)
    c.expect("ini list", "theme=dark", out)
    dev.run("ini set %s volume 70" % inif)
    out = dev.run("ini get %s volume" % inif)
    c.expect("ini update", "70", out)
    dev.run("ini del %s theme" % inif)
    out = dev.run("ini get %s theme || echo P4INIDELOK" % inif)
    c.expect("ini del removes key", "P4INIDELOK", out)

    dev.run("set P4INIVAL=hello-%s" % stamp)
    dev.run("ini save %s/env.ini" % DATA)
    dev.run("set P4INIVAL=")
    dev.run("ini load %s/env.ini" % DATA)
    out = dev.run("echo P4INICHECK %P4INIVAL%")
    c.expect("ini save/load round-trips env", "hello-%s" % stamp, out)
    dev.run("set P4INIVAL=")

    dev.run("appconfig %s set theme ocean" % app)
    out = dev.run("appconfig %s get theme" % app)
    c.expect("appconfig get", "ocean", out)
    out = dev.run("appconfig %s" % app)
    c.expect("appconfig list", "theme=ocean", out)
    out = dev.run("appconfig %s path" % app)
    c.expect("appconfig path", "appconfig.path", out)
    dev.run("del /p sd:/APPS/%s.INI" % app)

    out = dev.run("temp")
    c.expect("temp dir", "temp.dir=", out)
    out = dev.run("temp new csv")
    c.expect("temp new path", "temp.path=", out)
    out = dev.run("temp clean")
    c.expect("temp clean", "cleaned", out)

    # ==================================================================
    # alarm / cal
    # ==================================================================
    out = dev.run("alarm add 2035-01-01 09:00 %s /msg:p4msg /beep" % title)
    c.expect("alarm add schedules", "scheduled", out)
    out = dev.run("alarm list /b")
    c.expect("alarm list bare", title, out)
    aids = _ids_for(out, title)
    c.check("alarm id parsed", bool(aids), "no id in:\n%s" % out[-400:])
    if aids:
        close_ids.append(aids[0])

    out = dev.run("alarm status")
    c.expect("alarm status count", "alarm.count", out)
    c.expect("alarm status checker", "alarm.checker", out)
    if aids:
        out = dev.run("alarm disable %s" % aids[0])
        c.expect("alarm disable", "disabled", out)
        out = dev.run("alarm enable %s" % aids[0])
        c.expect("alarm enable", "enabled", out)
        out = dev.run("alarm snooze %s 5" % aids[0])
        c.expect("alarm snooze", "snoozed", out)

    out = dev.run("export alarms csv %s/alarms.csv" % DATA)
    c.expect("export alarms csv", "row(s)", out)
    out = dev.run("export alarms json %s/alarms.json" % DATA)
    c.expect("export alarms json", "row(s)", out)
    out = dev.run("export alarms txt %s/alarms.txt" % DATA)
    c.expect("export alarms txt", "row(s)", out)
    out = dev.run("export alarms ics %s/alarms.ics" % DATA)
    c.expect("export alarms ics", "row(s)", out)

    dev.push_file("%s/imp.csv" % DATA,
                  ("id,when,title,msg,recur,flags\n0,2035-02-02 10:00,%s,p4import,0,0\n"
                   % imp_title).encode())
    out = dev.run("import alarms csv %s/imp.csv" % DATA)
    c.expect("import alarms csv", "event(s)", out)
    out = dev.run("alarm list /b")
    c.expect("imported alarm listed", imp_title, out)
    close_ids.extend(_ids_for(out, imp_title))

    out = dev.run("cal today")
    c.expect("cal today", "cal.today", out)
    out = dev.run("cal next")
    c.expect("cal next", "cal.next", out)
    out = dev.run("cal 2035-01")
    c.expect("cal month grid", "2035-01", out)

    # ==================================================================
    # gfind (db + alarms)
    # ==================================================================
    out = dev.run("gfind Alice /b")
    c.expect("gfind db match", "DB|", out)
    out = dev.run("gfind Alice /count")
    c.expect("gfind total count", "gfind.total", out)

    # ==================================================================
    # gfind /files (opt-in text-file scope; shared with findstr /S)
    # ==================================================================
    fdir = "%s/files" % DATA
    token = "P4NEEDLE%s" % stamp
    dev.run("md %s" % fdir)
    dev.run('write %s/a.txt "line one %s"' % (fdir, token))
    dev.run('write %s/b.md "markdown %s"' % (fdir, token))
    # An image and an unknown-kind file must be skipped by the default filter.
    dev.run('write %s/c.bmp "binary %s"' % (fdir, token))
    dev.run('write %s/d.dat "opaque %s"' % (fdir, token))

    out = dev.run("gfind %s /files /root:%s /b" % (token, fdir))
    c.expect("gfind file txt match", "FILE|", out)
    c.expect("gfind file txt path", "a.txt", out)
    c.expect("gfind file md match", "b.md", out)
    c.expect("gfind skips images", "c.bmp", out, want=False)
    c.expect("gfind skips unknown kinds", "d.dat", out, want=False)

    out = dev.run("gfind %s /files /root:%s /count" % (token, fdir))
    c.expect("gfind files count", "gfind.files=2", out)
    c.expect("gfind total includes files", "gfind.total=2", out)

    out = dev.run("gfind %s /files /root:%s /ext:.md /b" % (token, fdir))
    c.expect("gfind ext filter keeps md", "b.md", out)
    c.expect("gfind ext filter drops txt", "a.txt", out, want=False)

    out = dev.run("gfind %s /files /root:%s /filesonly" % (token, fdir))
    c.expect("gfind filesonly lists a file", "a.txt", out)

    # Opt-in proof: without /files the text tree is never scanned.
    out = dev.run("gfind %s /b" % token)
    c.expect("gfind default does not scan files", "FILE|", out, want=False)

    # ==================================================================
    # archive
    # ==================================================================
    # Archive a source subdirectory so the output .p4a/.tmp never lands inside
    # the tree being walked (the writer would otherwise see its own temp file).
    src = "%s/src" % DATA
    arc = "%s/p4.p4a" % DATA
    dev.run("md %s" % src)
    dev.run("write %s/member.txt P4ARCHIVE-MEMBER-%s" % (src, stamp))
    out = dev.run("archive create %s %s" % (arc, src), timeout=240)
    c.expect("archive create", "file(s)", out)
    out = dev.run("archive list %s /b" % arc, timeout=90)
    c.expect("archive list member", "member.txt", out)
    out = dev.run("archive verify %s" % arc, timeout=240)
    c.expect("archive verify crc", "verified", out)
    out = dev.run("archive extract %s %s/out" % (arc, DATA), timeout=240)
    c.expect("archive extract", "extract", out)

    # ==================================================================
    # crypt
    # ==================================================================
    pw = "p4pw%s" % stamp
    plain = "%s/secret.txt" % DATA
    lock = "%s/secret.lock" % DATA
    unlocked = "%s/secret.out" % DATA
    dev.run("write %s P4CRYPT-%s" % (plain, stamp))
    # crypt now carries a software AES-256-GCM fallback for a fragmented
    # Tab5 DMA heap (was bugs.md F23: hardware descriptors failed after a
    # busy session). No skip: lock/unlock must pass on every board.
    out = dev.run("crypt lock %s %s /p:%s" % (plain, lock, pw), timeout=120)
    c.expect("crypt lock", "locked", out)
    out = dev.run("crypt unlock %s %s /p:%s" % (lock, unlocked, pw), timeout=120)
    c.expect("crypt unlock", "unlocked", out)
    out = dev.run("type %s" % unlocked)
    c.expect("crypt round-trip content", "P4CRYPT-%s" % stamp, out)
    out = dev.run("crypt unlock %s %s/bad /p:wrong-%s" % (lock, DATA, stamp), timeout=120)
    c.expect("crypt wrong password rejected", "wrong password or corrupt file", out)
    out = dev.run("crypt unlock %s %s /p:%s" % (lock, unlocked, pw), timeout=120)
    c.check("crypt password masked", pw not in out, "password leaked to transcript")

    # ==================================================================
    # cleanup
    # ==================================================================
    for cid in close_ids:
        dev.run("alarm del %s" % cid)
    out = dev.run("alarm purge")
    c.expect("alarm purge", "purged", out)

    dev.run("db drop %s" % dbname)
    dev.run("db drop %s" % dbname2)
    dev.run("del /p sd:/APPS/%s.INI" % app)
    _confirm_destructive(dev, "rd /s /p %s" % DATA)
    out = dev.run("if exist %s echo P4DATALEFT" % DATA)
    c.expect("scratch dir removed", "P4DATALEFT", out, want=False)

    return c
