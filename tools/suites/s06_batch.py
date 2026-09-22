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
calc C3=upper$('hi')
echo P4BATCH: calc-upper %C3%
calc C4=instr('hello','l')
echo P4BATCH: calc-instr %C4%
calc 10/4
set P4ARR[0]=10
set P4ARR[1]=20
echo P4BATCH: array %P4ARR[0]%+%P4ARR[1]%
set /a P4ARR[0]+=5
echo P4BATCH: array-add %P4ARR[0]%
for /A %%k in (P4ARR) do echo P4BATCH: fora %%k=%%l
set P4SUB=hello
echo P4BATCH: substr %P4SUB:~1,3%
echo P4BATCH: replace %P4SUB:l=L%
setlocal enabledelayedexpansion
echo P4BATCH: delayed !P4SUB! !P4SUB:~0,2!
endlocal
echo P4BATCH: undelayed !P4SUB!
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
for /L %%i in (1,1,3) do echo P4BATCH: forl %%i
for /L %%i in (6,-2,0) do echo P4BATCH: forl-down %%i
set /a P4W=0
while P4W LSS 3 do set /a P4W=P4W+1
echo P4BATCH: while %P4W%
while 0 do echo P4BATCH: while-ran
echo P4BATCH: while-el %ERRORLEVEL%
set P4SW=diag
switch %P4SW% setup:P4SWC diag:P4SWD /d:P4SWH
echo P4BATCH: switch-fallthrough WRONG
goto :P4SWEND
:P4SWC
echo P4BATCH: switch WRONG cfg
goto :P4SWEND
:P4SWD
echo P4BATCH: switch-dg OK
goto :P4SWEND
:P4SWH
echo P4BATCH: switch WRONG help
:P4SWEND
set P4SW=zzz
switch %P4SW% setup:P4SWC diag:P4SWD /d:P4SWH2
echo P4BATCH: switch WRONG fallthrough
goto :P4SWEND2
:P4SWH2
echo P4BATCH: switch-default OK
:P4SWEND2
switch %P4SW% setup:P4SWC diag:P4SWD
echo P4BATCH: switch-nodefault %ERRORLEVEL%
md P4BDIRA
md P4BDIRB
write P4BFILE.TXT x
for /D %%v in (P4BDIR*) do echo P4BATCH: ford %%v
md P4BREC
md P4BREC/SUB
write P4BREC/TOP.TXT x
write P4BREC/SUB/DEEP.TXT y
for /R P4BREC %%v in (*.TXT) do echo P4BATCH: forr %%v
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


# Declarative screens (`screen run|info`): one `.FRM` file per screen, flat
# KEY=VALUE like APPINFO. Timeouts of 1s keep the suite non-interactive.
SCR_DLG = "P4SCR_D.FRM"
SCR_FRM = "P4SCR_F.FRM"

P4SCR_D = """title=Hello
type=dialog
message=Hi there.
buttons=Yes|No
timeout=1
"""

P4SCR_F = """title=Setup
type=form
timeout=1
field0=SSID=text:SSID
field1=Password=password:PASS
"""

# Declarative multi-screen flows (`screen flow`): a `.FLOW` graph over `.FRM`
# nodes. Both dialogs time out in 1 s, so the flow is fully non-interactive.
SCR_FA = "P4FA.FRM"
SCR_FB = "P4FB.FRM"
FLOW_OK = "P4FLOW.FLOW"
FLOW_BAD = "P4FBAD.FLOW"

P4SCR_FA = """title=FlowA
type=dialog
message=First step.
timeout=1
"""

P4SCR_FB = """title=FlowB
type=dialog
message=Second step.
timeout=1
"""

# a times out -> err:255 routes to b; b ends. start= is the entry node.
P4FLOW_OK = """; flow test
flow.start=a
flow.title=Flow test
a.screen=P4FA.FRM
a.var=FLOWRESULT
a.err:255=b
a.*=end
b.screen=P4FB.FRM
b.*=end
"""

# A broken flow: the entry node has no screen.
P4FLOW_BAD = """flow.start=missing
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
    dev.run("del /p P4BFILE.TXT")
    dev.run("rd P4BDIRA")
    dev.run("rd P4BDIRB")
    dev.run("rd P4BREC/SUB")
    dev.run("rd P4BREC")

    dev.push_file(BATCH, P4BTEST.encode())
    dev.push_file(LIB, P4LIB.encode())
    dev.push_file(MISS_ABORT, P4MAB.encode())
    dev.push_file(MISS_CONT, P4MCT.encode())
    dev.push_file(SCR_DLG, P4SCR_D.encode())
    dev.push_file(SCR_FRM, P4SCR_F.encode())
    dev.push_file(SCR_FA, P4SCR_FA.encode())
    dev.push_file(SCR_FB, P4SCR_FB.encode())
    dev.push_file(FLOW_OK, P4FLOW_OK.encode())
    dev.push_file(FLOW_BAD, P4FLOW_BAD.encode())

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
    c.expect("calc upper$", "P4BATCH: calc-upper HI", out)
    c.expect("calc instr", "P4BATCH: calc-instr 3", out)
    c.expect("calc float divide", "2.5", out)

    # ---- arrays + DOS string forms ----------------------------------
    c.expect("array slots read", "P4BATCH: array 10+20", out)
    c.expect("set /a on array slot", "P4BATCH: array-add 15", out)
    c.expect("substring slice", "P4BATCH: substr ell", out)
    c.expect("replacement", "P4BATCH: replace heLLo", out)

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

    # ---- for /L + while --------------------------------------------
    c.expect("for /L counts up", "P4BATCH: forl 2", out)
    c.expect("for /L counts down", "P4BATCH: forl-down 2", out)
    c.expect("while loops to 3", "P4BATCH: while 3", out)
    c.expect("while skips a false body", "P4BATCH: while-ran", out, want=False)
    c.expect("while sets ERRORLEVEL", "P4BATCH: while-el 0", out)

    # ---- for /A + delayed expansion -------------------------------
    c.expect("for /A binds index", "P4BATCH: fora 0=15", out)
    c.expect("for /A binds second slot", "P4BATCH: fora 1=20", out)
    c.expect("delayed expansion in scope", "P4BATCH: delayed hello he", out)
    c.expect("delayed expansion ends with scope", "P4BATCH: undelayed !P4SUB!", out)

    # ---- switch ----------------------------------------------------
    c.expect("switch jumps to match", "P4BATCH: switch-dg OK", out)
    c.expect("switch skips fallthrough", "P4BATCH: switch-fallthrough", out, want=False)
    c.expect("switch jumps to default", "P4BATCH: switch-default OK", out)
    c.expect("switch no-match falls through", "P4BATCH: switch-nodefault 1", out)

    # ---- for /D + for /R -------------------------------------------
    c.expect("for /D lists dir A", "P4BATCH: ford P4BDIRA", out)
    c.expect("for /D lists dir B", "P4BATCH: ford P4BDIRB", out)
    c.expect("for /R finds top file", "TOP.TXT", out)
    c.expect("for /R finds deep file", "DEEP.TXT", out)

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

    # ---- declarative screens ---------------------------------------
    info = dev.run("screen info %s" % SCR_DLG)
    c.expect("screen info reports type", "dialog", info)
    c.expect("screen info reports title", "Hello", info)
    # The runner queues its marker echo 0.5s after the command; a modal open
    # at that moment eats the marker as serial input and desyncs the run. The
    # screens time out after 1s, so settle the marker behind the modal close.
    c.expect("screen dialog times out like dialog", "P4SCREEN-DLG-TIMEOUT",
             dev.run("screen run %s & if errorlevel 255 echo P4SCREEN-DLG-TIMEOUT" % SCR_DLG,
                     timeout=60, settle=4))
    c.expect("screen form times out like form", "P4SCREEN-FRM-TIMEOUT",
             dev.run("screen run %s & if errorlevel 255 echo P4SCREEN-FRM-TIMEOUT" % SCR_FRM,
                     timeout=60, settle=4))
    c.expect("screen rejects a missing file",
             "Usage", dev.run("screen run P4NOSUCH.FRM"))
    dev.run("del /p %s" % SCR_DLG)
    dev.run("del /p %s" % SCR_FRM)

    # ---- declarative flows -----------------------------------------
    finfo = dev.run("screen info %s" % FLOW_OK)
    c.expect("flow info reports start", "start: a", finfo)
    c.expect("flow info lists a step", "a -> P4FA.FRM", finfo)
    c.expect("flow info lists a rule", "a.err:255 -> b", finfo)
    # Two 1s-timeout dialogs: settle the runner marker behind both closes.
    fout = dev.run("screen flow %s & echo P4FLOW-EL=%%ERRORLEVEL%%" % FLOW_OK,
                   timeout=60, settle=6)
    c.expect("flow visits first step", "flow: a", fout)
    c.expect("flow routes err:255 to second step", "flow: b", fout)
    c.expect("flow returns the last screen errorlevel", "P4FLOW-EL=255", fout)
    c.expect("flow run refuses via `screen run`", "is a flow",
             dev.run("screen run %s" % FLOW_OK))
    c.expect("broken flow refuses", "no .screen=",
             dev.run("screen flow %s" % FLOW_BAD))
    c.expect("broken flow sets errorlevel 1", "P4BAD=1",
             dev.run("screen flow %s & echo P4BAD=%%ERRORLEVEL%%" % FLOW_BAD))
    dev.run("del /p %s" % SCR_FA)
    dev.run("del /p %s" % SCR_FB)
    dev.run("del /p %s" % FLOW_OK)
    dev.run("del /p %s" % FLOW_BAD)

    # ---- cleanup -----------------------------------------------------
    dev.run("del /p %s" % BATCH)
    dev.run("del /p %s" % LIB)
    dev.run("del /p %s" % MISS_ABORT)
    dev.run("del /p %s" % MISS_CONT)
    dev.run("del /p P4BT*.*")
    dev.run("del /p P4BREM.TXT")
    dev.run("del /p P4BFILE.TXT")
    dev.run("del /p P4BREC/TOP.TXT")
    dev.run("del /p P4BREC/SUB/DEEP.TXT")
    dev.run("rd P4BDIRA")
    dev.run("rd P4BDIRB")
    dev.run("rd P4BREC/SUB")
    dev.run("rd P4BREC")
    out = dev.run("if exist %s echo P4BATCHLEFT" % BATCH)
    c.expect("batch file removed", "P4BATCHLEFT", out, want=False)

    return c
