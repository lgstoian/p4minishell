# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Batch suite: the DOS batch language end to end.

A generated ``P4BTEST.BAT`` (plus a tiny ``P4LIB.BAT`` routine library) is
pushed to the SD card once, then run with arguments. The script writes
well-defined ``P4BATCH: <name> OK`` markers that the suite asserts, covering
set / set /a / set /p, calc, every ``if`` form, ``for`` (literal + wildcard),
``for /f`` (skip / eol / delims / tokens / file + command + pipe sources),
goto / goto :eof, call (whole file + ``file::routine`` + ``:label``), the newer
BASIC-flavoured gosub / return / on, alias, shift, setlocal / endlocal, pipes,
redirection, chaining, line continuation, the dynamic pseudo-variables, proc,
pause / choice and exit /b. It also pins the DOS parity rules for a missing
label: ``goto`` aborts the batch, while ``call``/``gosub`` set ERRORLEVEL 1 and
continue.

The batch files and every scratch data file they create are deleted at the
end.
"""
import re
import time

from p4test.asserts import Checklist

NAME = "batch"
TAGS = ["core", "batch"]

BATCH = "P4BTEST.BAT"
LIB = "P4LIB.BAT"


P4BTEST = r"""@echo off
echo P4BATCH: start %~nx0
echo P4BATCH: arg1 %1 arg2 %2
echo P4BATCH: all %*
set P4V=hello
echo P4BATCH: set %P4V%
set /a P4N=6*7
echo P4BATCH: seta %P4N%
set /a P4C=100/7
echo P4BATCH: seta-div %P4C%
write P4BTIN.TXT P4INPUT
set /p P4P=< P4BTIN.TXT
echo P4BATCH: setp %P4P%
calc C1=7 MOD 3
echo P4BATCH: calc-mod %C1%
calc C2=len('hello')
echo P4BATCH: calc-len %C2%
calc 10/4
if 5 GTR 3 echo P4BATCH: if-num OK
if "abc"=="abc" echo P4BATCH: if-str OK
if not "abc"=="xyz" echo P4BATCH: if-not OK
if exist P4BTEST.BAT echo P4BATCH: if-exist OK
set P4DEF=1
if defined P4DEF echo P4BATCH: if-defined OK
delay 1500
choice /C:YN /T:Y,1
echo P4BATCH: el %ERRORLEVEL%
if errorlevel 1 echo P4BATCH: if-errorlevel OK
for %%i in (alpha beta gamma) do echo P4BATCH: for-lit %%i
write P4BTF1.TMP one
write P4BTF2.TMP two
for %%f in (P4BTF?.TMP) do echo P4BATCH: for-file %%f
write P4BTF.DAT alpha:1
append P4BTF.DAT beta:2
for /f "tokens=1,2 delims=:" %%a in (P4BTF.DAT) do echo P4BATCH: forf-tok %%a=%%b
for /f "skip=1" %%l in (P4BTF.DAT) do echo P4BATCH: forf-skip %%l
write P4BTEOL.DAT ;skipme
append P4BTEOL.DAT P4EOLKEEP
for /f "eol=;" %%l in (P4BTEOL.DAT) do echo P4BATCH: forf-eol %%l
for /f "tokens=*" %%v in ('echo P4BATCHCMD') do echo P4BATCH: forf-cmd %%v
type P4BTF.DAT | for /f "tokens=1 delims=:" %%k in () do echo P4BATCH: forf-pipe %%k
goto P4SKIP
echo P4BATCH: goto-fail
:P4SKIP
echo P4BATCH: goto OK
call :P4SUB
call P4LIB.BAT
if errorlevel 5 echo P4BATCH: call-errcode OK
call P4LIB.BAT::p4add 3 4
call P4LIB.BAT::p4shout hello
call :P4SHIFT one two
setlocal
set P4LOCAL=inside
endlocal
if not defined P4LOCAL echo P4BATCH: setlocal OK
echo P4BATCHPIPE | findstr P4BATCHPIPE
echo P4REDIR-TOKEN > P4BTRED.TXT
echo P4REDIR-TWO >> P4BTRED.TXT
type P4BTRED.TXT
echo P4BATCH: chain-amp OK & echo P4BATCH: chain-amp2 OK
echo P4BATCH: chain-and OK && echo P4BATCH: chain-and2 OK
p4badcmd0987 || echo P4BATCH: chain-or OK
echo P4BATCH: linecont ^
CONTINUED
rem P4BATCH: rem-comment | echo P4BATCH: rem-leaked
rem P4BATCH: rem-redir > P4BREM.TXT
echo P4BATCH: rem-after OK
echo P4BATCH: cd %CD%
echo P4BATCH: date %DATE%
echo P4BATCH: time %TIME%
echo P4BATCH: random %RANDOM%
set P4ON=2
on %P4ON% goto :P4ONA,:P4ONB,:P4ONC
echo P4BATCH: on-miss
:P4ONA
echo P4BATCH: on-a WRONG
goto :P4ONEND
:P4ONB
echo P4BATCH: on-b OK
goto :P4ONEND
:P4ONC
echo P4BATCH: on-c WRONG
:P4ONEND
echo P4BATCH: gosub-begin
gosub :P4GSUB
echo P4BATCH: gosub-back %ERRORLEVEL%
on 1 gosub :P4ONSUB
echo P4BATCH: on-returned
on %P4ON%*2-2 gosub :P4ONSUB,:P4ONSUB
echo P4BATCH: on-gosub-back
on 1 call :P4ONSUB
echo P4BATCH: on-call-returned
on 9 goto :P4ONA,:P4ONB
echo P4BATCH: on-fallthrough OK
gosub P4LIB.BAT::p4doub 21
echo P4BATCH: gosub-lib %ERRORLEVEL%
proc /name
proc /args
pause
echo P4BATCH: pause OK
exit /b 0

:P4SUB
echo P4BATCH: call-local OK
goto :eof

:P4SHIFT
shift
echo P4BATCH: shift %1
goto :eof

:P4GSUB
echo P4BATCH: gosub-local OK
return 7

:P4ONSUB
echo P4BATCH: on-gosub OK
return
"""


P4LIB = r"""@echo off
echo P4BATCH: call-file OK
exit /b 5

:p4add
set /a P4SUM=%1+%2
echo P4BATCH: lib-add %P4SUM%
goto :eof

:p4shout
echo P4BATCH: lib-shout %*
goto :eof

:p4doub
set /a P4DBL=%1*2
echo P4BATCH: lib-doub %P4DBL%
return 4
"""


# DOS parity: `goto` to a missing label aborts the batch; `call`/`gosub` to a
# missing label set ERRORLEVEL 1 and continue. `return` at the top level ends
# the frame like `goto :eof`.
MISS_ABORT = "P4MAB.BAT"
MISS_CONT = "P4MCT.BAT"

P4MAB = r"""@echo off
echo P4MISS: before
goto :nope
echo P4MISS: after
"""

P4MCT = r"""@echo off
call :nope
echo P4MISS: call-continued %ERRORLEVEL%
return
echo P4MISS: return-after
"""


def run(dev, ctx):
    c = Checklist(NAME)
    stamp = "%05d" % (int(time.time()) % 100000)

    # Work from the SD root so the bare batch name resolves through PATH.
    dev.run("cd sd:/")

    # Best-effort cleanup of a previous aborted run.
    dev.run("del /p %s" % BATCH)
    dev.run("del /p %s" % LIB)
    dev.run("del /p P4BT*.*")
    dev.run("del /p P4BREM.TXT")

    dev.push_file(BATCH, P4BTEST.encode())
    dev.push_file(LIB, P4LIB.encode())
    dev.push_file(MISS_ABORT, P4MAB.encode())
    dev.push_file(MISS_CONT, P4MCT.encode())

    # ---- alias (prompt-only expansion) -------------------------------
    alias = "p4mark%s" % stamp
    dev.run("alias %s=echo P4ALIAS OK" % alias)
    out = dev.run(alias)
    c.expect("alias expands at prompt", "P4ALIAS OK", out)
    dev.run("unalias %s" % alias)

    # ---- run the whole batch once ------------------------------------
    out = dev.run("%s argone argtwo" % BATCH, timeout=180)
    c.expect("batch start marker", "P4BATCH: start %s" % BATCH, out)
    c.expect("batch %1 argument", "P4BATCH: arg1 argone", out)
    c.expect("batch %* all arguments", "P4BATCH: all argone argtwo", out)

    # ---- set / set /a / set /p ---------------------------------------
    c.expect("set stores value", "P4BATCH: set hello", out)
    c.expect("set /a multiplies", "P4BATCH: seta 42", out)
    c.expect("set /a integer divide", "P4BATCH: seta-div 14", out)
    c.expect("set /p reads redirection", "P4BATCH: setp P4INPUT", out)

    # ---- calc --------------------------------------------------------
    c.expect("calc MOD", "P4BATCH: calc-mod 1", out)
    c.expect("calc string len", "P4BATCH: calc-len 5", out)
    c.expect("calc float divide", "2.5", out)

    # ---- if forms ----------------------------------------------------
    c.expect("if numeric", "P4BATCH: if-num OK", out)
    c.expect("if string equals", "P4BATCH: if-str OK", out)
    c.expect("if not", "P4BATCH: if-not OK", out)
    c.expect("if exist", "P4BATCH: if-exist OK", out)
    c.expect("if defined", "P4BATCH: if-defined OK", out)
    c.expect("if errorlevel", "P4BATCH: if-errorlevel OK", out)
    c.expect("%ERRORLEVEL% expands", "P4BATCH: el 1", out)

    # ---- for literal + wildcard --------------------------------------
    c.expect("for literal token", "P4BATCH: for-lit beta", out)
    c.expect("for wildcard file", "P4BATCH: for-file P4BTF1.TMP", out)

    # ---- for /f sources ----------------------------------------------
    c.expect("for /f delims+tokens", "P4BATCH: forf-tok alpha=1", out)
    c.expect("for /f skip", "P4BATCH: forf-skip beta:2", out)
    c.expect("for /f eol", "P4BATCH: forf-eol P4EOLKEEP", out)
    c.expect("for /f command source", "P4BATCH: forf-cmd P4BATCHCMD", out)
    c.expect("for /f pipe source", "P4BATCH: forf-pipe alpha", out)

    # ---- goto --------------------------------------------------------
    c.expect("goto lands on label", "P4BATCH: goto OK", out)
    c.expect("goto skipped bad line", "P4BATCH: goto-fail", out, want=False)

    # ---- call --------------------------------------------------------
    c.expect("call :label subroutine", "P4BATCH: call-local OK", out)
    c.expect("call whole file", "P4BATCH: call-file OK", out)
    c.expect("call propagates errorlevel", "P4BATCH: call-errcode OK", out)
    c.expect("call file::routine", "P4BATCH: lib-add 7", out)
    c.expect("call file::routine %*", "P4BATCH: lib-shout hello", out)
    c.expect("shift moves arguments", "P4BATCH: shift two", out)

    # ---- setlocal / endlocal -----------------------------------------
    c.expect("setlocal isolates scope", "P4BATCH: setlocal OK", out)

    # ---- pipes / redirection -----------------------------------------
    c.expect("pipe into findstr", "P4BATCHPIPE", out)
    c.expect("redirection > writes", "P4REDIR-TOKEN", out)
    c.expect("redirection >> appends", "P4REDIR-TWO", out)

    # ---- chaining ----------------------------------------------------
    c.expect("chain & runs both", "P4BATCH: chain-amp2 OK", out)
    c.expect("chain && on success", "P4BATCH: chain-and2 OK", out)
    c.expect("chain || on failure", "P4BATCH: chain-or OK", out)

    # ---- line continuation -------------------------------------------
    c.expect("caret joins lines", "P4BATCH: linecont CONTINUED", out)

    # ---- rem is opaque to end of line (DOS parity) -------------------
    c.expect("rem swallows a pipe", "P4BATCH: rem-leaked", out, want=False)
    c.expect("rem is not a redirect target", "P4BATCH: rem-after OK", out)
    rem_file = dev.run("if exist P4BREM.TXT echo P4REMFILE")
    c.expect("rem comment created no file", "P4REMFILE", rem_file, want=False)

    # ---- dynamic pseudo-variables ------------------------------------
    c.expect("%CD% expands", "P4BATCH: cd /sdcard", out)
    c.check("no literal %CD% left", "P4BATCH: cd %CD%" not in out, "pseudo-variable not expanded")
    c.check("no literal %DATE% left", "%DATE%" not in out, "date not expanded")
    c.check("no literal %TIME% left", "%TIME%" not in out, "time not expanded")
    c.check("%DATE% looks like a date",
            re.search(r"P4BATCH: date \d\d-\d\d-\d\d\d\d", out) is not None, "no MM-DD-YYYY")
    c.check("%TIME% looks like a time",
            re.search(r"P4BATCH: time \d\d:\d\d:\d\d", out) is not None, "no HH:MM:SS")
    m = re.search(r"P4BATCH: random (\d+)", out)
    c.check("%RANDOM% is numeric", m is not None and 0 <= int(m.group(1)) <= 32767,
            "random=%r" % (m.group(1) if m else None,))

    # ---- proc --------------------------------------------------------
    c.expect("proc reports name", "proc.name", out)
    c.expect("proc reports args", "proc.args", out)

    # ---- pause (bounded key-wait; never hangs) -----------------------
    c.expect("pause continues after timeout", "P4BATCH: pause OK", out)

    # ---- new verbs: gosub / return / on ------------------------------
    c.expect("gosub runs a local subroutine", "P4BATCH: gosub-local OK", out)
    c.expect("gosub return sets ERRORLEVEL", "P4BATCH: gosub-back 7", out)
    c.expect("on goto selects the target", "P4BATCH: on-b OK", out)
    c.expect("on goto skips other targets", "P4BATCH: on-a WRONG", out, want=False)
    c.expect("on goto does not fall through", "P4BATCH: on-miss", out, want=False)
    c.expect("on gosub returns to caller", "P4BATCH: on-returned", out)
    c.expect("on arithmetic expression dispatches", "P4BATCH: on-gosub OK", out)
    c.expect("on gosub continues after return", "P4BATCH: on-gosub-back", out)
    c.expect("on call aliases gosub", "P4BATCH: on-call-returned", out)
    c.expect("on out-of-range falls through", "P4BATCH: on-fallthrough OK", out)
    c.expect("gosub file::routine runs", "P4BATCH: lib-doub 42", out)
    c.expect("gosub file::routine return code", "P4BATCH: gosub-lib 4", out)

    # ---- DOS parity: missing label -----------------------------------
    ab = dev.run(MISS_ABORT, timeout=60)
    c.expect("goto :missing runs up to the jump", "P4MISS: before", ab)
    c.expect("goto :missing aborts the batch", "P4MISS: after", ab, want=False)
    c.check("goto :missing reports the label",
            "cannot find the batch label" in ab.lower(), "no label error")

    mc = dev.run(MISS_CONT, timeout=60)
    c.expect("call :missing continues", "P4MISS: call-continued 1", mc)
    c.expect("return at top level ends the batch", "P4MISS: return-after", mc, want=False)

    # ---- cleanup -----------------------------------------------------
    dev.run("del /p %s" % BATCH)
    dev.run("del /p %s" % LIB)
    dev.run("del /p %s" % MISS_ABORT)
    dev.run("del /p %s" % MISS_CONT)
    dev.run("del /p P4BT*.*")
    dev.run("del /p P4M*.*")
    dev.run("del /p P4BREM.TXT")
    out = dev.run("if exist %s echo P4BATCHLEFT" % BATCH)
    c.expect("batch file removed", "P4BATCHLEFT", out, want=False)

    return c
