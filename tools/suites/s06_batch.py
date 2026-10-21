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
import zlib

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

# File-backed screens (`screen run` browse/view/hexview/image): one data
# file plus one `.FRM` per type. Timeouts of 1s keep the suite
# non-interactive (browse cancels -> EL 1; view/hexview/image close -> EL 0).
SCR_VTXT = "P4SCR_V.TXT"
SCR_VIEW = "P4SCR_V.FRM"
SCR_HEX = "P4SCR_H.FRM"
SCR_BRW = "P4SCR_B.FRM"
SCR_IMG = "P4SCR_I.FRM"
SCR_NOFILE = "P4SCR_N.FRM"
SCR_BOGUS = "P4SCR_X.FRM"
SCR_SHOT = "P4SCR_I.BMP"

P4SCR_V = """title=ProbeView
type=view
file=P4SCR_V.TXT
timeout=1
"""

P4SCR_H = """title=ProbeHex
type=hexview
file=P4SCR_V.TXT
timeout=1
"""

P4SCR_B = """title=ProbeBrowse
type=browse
start=sd:/
var=P4SCRV
timeout=1
"""

P4SCR_I = """title=ProbeImage
type=image
file=P4SCR_I.BMP
timeout=1
"""

P4SCR_N = """title=ProbeNoFile
type=view
timeout=1
"""

P4SCR_X = """title=ProbeBogus
type=bogus
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
    dev.push_file("P4GRP.BAT",
                  b"@echo off\r\nset n=0\r\n"
                  b"while n LSS 2 do set /a n+=1 & echo P4GRP-WIN\r\n"
                  b"echo P4GRP-WDONE\r\n")
    dev.push_file(MISS_ABORT, P4MAB.encode())
    dev.push_file(MISS_CONT, P4MCT.encode())
    dev.push_file(SCR_DLG, P4SCR_D.encode())
    dev.push_file(SCR_FRM, P4SCR_F.encode())
    dev.push_file(SCR_VTXT, b"hello-screen-view\nsecond line\n")
    dev.push_file(SCR_VIEW, P4SCR_V.encode())
    dev.push_file(SCR_HEX, P4SCR_H.encode())
    dev.push_file(SCR_BRW, P4SCR_B.encode())
    dev.push_file(SCR_IMG, P4SCR_I.encode())
    dev.push_file(SCR_NOFILE, P4SCR_N.encode())
    dev.push_file(SCR_BOGUS, P4SCR_X.encode())
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

    # ---- redirection devices, missing target, 2> merge, type stdin ----
    # Keep the whole-batch `out` intact for the checks below; this section
    # uses its own `red` variable.
    red = dev.run("echo hi > NUL")
    c.expect("NUL swallows redirected output", "hi", red, want=False)
    c.expect("NUL creates no file", "P4NULLEFT",
             dev.run("if exist NUL echo P4NULLEFT"), want=False)
    c.expect("CON passes output through", "hi", dev.run("echo hi > CON"))
    c.expect("missing target errors",
             "missing target", dev.run("echo hi >"))
    c.expect("missing input target errors",
             "missing target", dev.run("sort <"))
    dev.run("echo P4HANDLE > P4HANDLE.TXT")
    red = dev.run("echo hi 2> P4HANDLE.TXT & type P4HANDLE.TXT")
    c.expect("2> merges into the stdout file", "hi", red)
    c.expect("2> leaves no handle digit", "hi 2", red, want=False)
    dev.push_file("P4RDIR.TXT", b"redirstdin\n")
    c.expect("type reads the input slot", "redirstdin",
             dev.run("type < P4RDIR.TXT"))
    c.expect("pipe spools leave the card root", "_pipe",
             dev.run("echo abc | sort & dir sd:/ /b"), want=False)
    dev.run("del /p P4HANDLE.TXT")
    dev.run("del /p P4RDIR.TXT")

    # ---- chaining ----------------------------------------------------
    c.expect("chain & runs both", "P4BATCH: chain-amp2 OK", out)
    c.expect("chain && on success", "P4BATCH: chain-and2 OK", out)
    c.expect("chain || on failure", "P4BATCH: chain-or OK", out)

    # ---- parenthesized groups --------------------------------------
    c.expect("group chains as one link", "P4GRP-B",
             dev.run("(echo P4GRP-A & echo P4GRP-B)", timeout=30))
    gout = dev.run("(p4badcmd0987 || echo P4GRP-FB)", timeout=30)
    c.expect("group failure feeds ||", "P4GRP-FB", gout)
    # Echo preserves errorlevel by design, so `if errorlevel` (not bare
    # `&&`) is the deterministic probe for group success/failure.
    # DOS `if errorlevel N` means EL >= N; `if errorlevel 0` is ALWAYS true.
    # Exact EL check: `if errorlevel N & if not errorlevel N+1` or `if %errorlevel% EQU N`.
    c.expect("failed group leaves errorlevel >= 9009", "P4GRP-CAUGHT",
             dev.run("(p4badcmd0987) & if errorlevel 9009 echo P4GRP-CAUGHT",
                     timeout=30))
    c.check("if errorlevel 0 ALWAYS matches (DOS semantics)",
            "P4GRP-ALWAYS" in dev.run("(p4badcmd0987) & if errorlevel 0 echo P4GRP-ALWAYS",
                                      timeout=30), "if errorlevel 0 should ALWAYS run")
    c.expect("successful group feeds &&", "P4GRP-BOTH",
             dev.run("(echo P4GRP-C & echo P4GRP-D) && echo P4GRP-BOTH",
                     timeout=30))
    c.check("successful group skips || branch",
            "P4GRP-NEVER2" not in dev.run("(echo P4GRP-E) || echo P4GRP-NEVER2",
                                          timeout=30), "|| ran after success")
    c.expect("parens stay literal after echo", "(a & b)",
             dev.run("echo (a & b)", timeout=30))
    c.expect("if keeps group in body", "P4GRP-IF2",
             dev.run("if 1==1 (echo P4GRP-IF1 & echo P4GRP-IF2)", timeout=30))
    c.expect("group pipes inside", "P4GRP-PIPE",
             dev.run("(echo P4GRP-PIPE | sort)", timeout=30))
    c.expect("deep groups refused, not crashed", "refused",
             dev.run("(" * 10 + "echo deep" + ")" * 10, timeout=30))
    wout = dev.run("P4GRP", timeout=60)
    c.check("while body chains inside the loop", wout.count("P4GRP-WIN") == 2,
            "loop body ran %d/2 times" % wout.count("P4GRP-WIN"))
    c.expect("while falls through after", "P4GRP-WDONE", wout)
    dev.run("start /wait (echo P4GRP-BG1 > P4GRPBG.TXT & echo P4GRP-BG2 >> P4GRPBG.TXT)",
            timeout=60)
    c.expect("start backgrounds the whole group", "P4GRP-BG2",
             dev.run("type P4GRPBG.TXT", timeout=30))
    dev.run("del /p P4GRPBG.TXT", timeout=30)

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
             "no such file", dev.run("screen run P4NOSUCH.FRM"))
    dev.run("del /p %s" % SCR_DLG)
    dev.run("del /p %s" % SCR_FRM)

    # ---- file-backed screens (browse/view/hexview/image) -------------
    vinfo = dev.run("screen info %s" % SCR_VIEW)
    c.expect("screen view info reports file", "P4SCR_V.TXT", vinfo)
    c.expect("screen view info reports raw", "raw:", vinfo)
    binfo = dev.run("screen info %s" % SCR_BRW)
    c.expect("screen browse info reports start", "sd:/", binfo)
    c.expect("screen browse info reports var", "P4SCRV", binfo)
    c.expect("screen view times out like view",
             "P4SCREEN-VIEW-EL=0",
             dev.run("screen run %s & echo P4SCREEN-VIEW-EL=%%ERRORLEVEL%%" % SCR_VIEW,
                     timeout=60, settle=4))
    c.expect("screen hexview times out like hexview",
             "P4SCREEN-HEX-EL=0",
             dev.run("screen run %s & echo P4SCREEN-HEX-EL=%%ERRORLEVEL%%" % SCR_HEX,
                     timeout=60, settle=4))
    c.expect("screen browse cancels like browse", "P4SCREEN-BRW-CANCEL",
             dev.run("screen run %s & if errorlevel 1 echo P4SCREEN-BRW-CANCEL" % SCR_BRW,
                     timeout=60, settle=4))
    # Image screen: capture a BMP on-device first, then show it with a 1s
    # timeout (modal opens and auto-closes, EL 0). The Tab5 screenshot is
    # 1280x720 (~2.7 MB); the decode takes seconds, so settle well behind it.
    dev.run("screenshot %s" % SCR_SHOT, timeout=60)
    c.expect("screen image times out like image",
             "P4SCREEN-IMG-EL=0",
             dev.run("screen run %s & echo P4SCREEN-IMG-EL=%%ERRORLEVEL%%" % SCR_IMG,
                     timeout=90, settle=8))
    c.expect("screen view without file= refuses", "needs file=",
             dev.run("screen run %s" % SCR_NOFILE))
    c.expect("screen rejects an unknown type", "unknown type",
             dev.run("screen run %s" % SCR_BOGUS))
    dev.run("del /p %s" % SCR_VIEW)
    dev.run("del /p %s" % SCR_HEX)
    dev.run("del /p %s" % SCR_BRW)
    dev.run("del /p %s" % SCR_IMG)
    dev.run("del /p %s" % SCR_NOFILE)
    dev.run("del /p %s" % SCR_BOGUS)
    dev.run("del /p %s" % SCR_VTXT)
    dev.run("del /p %s" % SCR_SHOT)

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

    # ---- extended memory (himem: XMS-style handles over PSRAM) --------
    c.expect("himem bare reports the table", "handles 0/16 used",
             dev.run("himem"))
    c.expect("himem alloc reports bytes", "1024 bytes",
             dev.run("himem alloc 1K /v:P4HH"))
    dev.run("himem fill %P4HH% 65")
    c.expect("himem crc is deterministic", "B737FB1A",
             dev.run("himem crc %P4HH%"))
    c.expect("himem save stages bytes", "saved 1024 bytes",
             dev.run("himem save %P4HH% P4HIMEM.BIN"))
    dev.run("himem free %P4HH%")
    dev.run("himem alloc 2K /v:P4HH2")
    c.expect("himem load restores bytes", "loaded 1024 bytes",
             dev.run("himem load %P4HH2% P4HIMEM.BIN"))
    c.expect("himem crc matches after round-trip", "B737FB1A",
             dev.run("himem crc %P4HH2% 0 1024"))
    c.expect("himem lock guards free", "lock count 1",
             dev.run("himem lock %P4HH2%"))
    c.expect("himem locked free refuses", "cannot free",
             dev.run("himem free %P4HH2%"))
    dev.run("himem unlock %P4HH2%")
    dev.run("himem free %P4HH2%")
    c.expect("himem alloc without size is usage", "Usage",
             dev.run("himem alloc"))
    c.expect("himem free of a free handle fails", "cannot free",
             dev.run("himem free 5"))
    c.expect("himem over quota fails in a job", "alloc failed",
             dev.run("start /wait himem alloc 2M", timeout=60))
    dev.run("del /p P4HIMEM.BIN")
    dev.run("set P4HH=")
    dev.run("set P4HH2=")

    # ---- background jobs (start /wait, /t:, isolation) ----------------
    c.expect("bare start lists the pool", "bg0",
             dev.run("start"))
    c.expect("start /wait joins with EL", "finished: EL=0",
             dev.run("start /wait echo P4WAIT-OK", timeout=60))
    dev.run("set P4HISO=fg")
    dev.run("start /wait set P4HISO=bg", timeout=60)
    c.expect("bg set never leaks to the foreground", "[fg]",
             dev.run("echo [%P4HISO%]"))
    dev.run("set P4HISO=")
    c.expect("start /t: kills a long job", "finished: EL=1",
             dev.run("start /wait /t:1 delay 5000", timeout=60))
    c.expect("kill stops like taskkill", "stop requested",
             dev.run("start delay 30000 & kill bg0", timeout=60))
    dev.run("delay 500", timeout=30)

    # ---- task manager (suspend/resume/nice/start /b/tmgr_* events) -----
    # Victim runs an endless delay loop: `delay` clamps to 10 s, so a
    # plain `start delay N` would expire mid-flow and fake assertions.
    dev.push_file("P4VICT.BAT",
                  b"@echo off\r\n:loop\r\ndelay 10000\r\ngoto loop\r\n")
    tm = re.search(r"\[(bg\d+)\] started",
                   dev.run("start P4VICT.BAT", timeout=30))
    c.check("tmgr victim spawned", tm is not None, "")
    if tm is not None:
        job = tm.group(1)
        c.expect("suspend freezes the job", "suspended",
                 dev.run("suspend %s" % job, timeout=30))
        c.expect("pool shows suspended", "suspended",
                 dev.run("start", timeout=30))
        c.expect("ps shows the frozen task", "%s SUS" % job,
                 dev.run("ps /b", timeout=30))
        # Exact start priority varies: a suspend can freeze the job inside
        # an SD session (transient SD boost, wider window on slow SDIO),
        # so match the confirmed target, not the sampled start.
        c.expect("nice sets priority", "-> 3",
                 dev.run("nice %s 3" % job, timeout=30))
        c.expect("nice rejects out of range", "outside 1..3",
                 dev.run("nice %s 9" % job, timeout=30))
        c.expect("nice needs a value", "Usage",
                 dev.run("nice %s" % job, timeout=30))
        c.expect("suspend needs a job", "Usage", dev.run("suspend", timeout=30))
        c.expect("resume needs a job", "Usage", dev.run("resume", timeout=30))
        c.expect("suspend of unknown job fails", "no such job",
                 dev.run("suspend bg9", timeout=30))
        c.expect("resume thaws the job", "running again",
                 dev.run("resume %s" % job, timeout=30))
        c.expect("start /b has machine rows", "%s|running" % job,
                 dev.run("start /b", timeout=30))
        dev.run("wait tmgr_exit /t:1", timeout=30)
        c.expect("taskkill ends the job", "stop requested",
                 dev.run("taskkill %s" % job, timeout=30))
        c.expect("job exit fires tmgr_exit", "P4EXIT-EL=0",
                 dev.run("wait tmgr_exit /t:5 & echo P4EXIT-EL=%ERRORLEVEL%",
                         timeout=30))
    # Pool-full rendezvous: launch delay jobs until pool is full (3 running),
    # the next launch is refused and fires tmgr_pool_full.
    # First, clean any pre-existing jobs.
    for name in ("bg0", "bg1", "bg2"):
        dev.run("taskkill %s" % name, timeout=5)
    dev.run("delay 500", timeout=10)
    for name in ("bg0", "bg1", "bg2"):
        for _ in range(20):
            out = dev.run("start /b", timeout=5)
            if "%s|idle" % name in out or "%s|unavailable" % name in out:
                break
            dev.run("delay 200", timeout=5)
    # Count current running jobs, then launch until we have 3 total.
    out = dev.run("start /b", timeout=5)
    running_before = sum(1 for n in ("bg0", "bg1", "bg2") if "%s|running" % n in out)
    fill = []
    for _ in range(5):
        f = re.search(r"\[(bg\d+)\] started",
                      dev.run("start P4VICT.BAT", timeout=30))
        if f is not None:
            fill.append(f.group(1))
        else:
            break
    out = dev.run("start /b", timeout=5)
    running_after = sum(1 for n in ("bg0", "bg1", "bg2") if "%s|running" % n in out)
    c.check("pool filled to three running jobs", running_after == 3,
            "running_before=%d launched=%d running_after=%d" % (running_before, len(fill), running_after))
    if running_after == 3:
        # Launch 4th job - should be refused and fire tmgr_pool_full
        c.expect("fourth launch refused", "no background slots free",
                 dev.run("start P4VICT.BAT", timeout=30))
        c.expect("refusal fires tmgr_pool_full", "P4FULL-EL=0",
                 dev.run("wait tmgr_pool_full /t:5 & echo P4FULL-EL=%ERRORLEVEL%",
                         timeout=30))
        # Kill all running jobs
        out = dev.run("start /b", timeout=5)
        for n in ("bg0", "bg1", "bg2"):
            if "%s|running" % n in out:
                dev.run("taskkill %s" % n, timeout=30)
        dev.run("delay 500", timeout=30)
    dev.run("del /p P4VICT.BAT", timeout=30)

    # ---- file associations (assoc/ftype + open dispatch) ---------------
    dev.push_file("ASSOCAPP.BAT", b"@echo off\r\necho [M-ASSOCAPP] got=%1\r\n")
    dev.run("assoc .tst=", timeout=30)
    c.expect("assoc maps an extension", ".tst=ASSOCAPP",
             dev.run("assoc .tst=ASSOCAPP", timeout=30))
    c.expect("assoc shows one mapping", ".tst=ASSOCAPP",
             dev.run("assoc .tst", timeout=30))
    c.expect("assoc usage", "Usage", dev.run("assoc .tst=ASSOCAPP extra", timeout=30))
    c.expect("assoc rejects bad extensions", "Usage",
             dev.run("assoc no-dot-here!", timeout=30))
    c.expect("assoc warns on unknown apps", "no such app",
             dev.run("assoc .tst2=NO_SUCH_APP_XYZ", timeout=30))
    dev.run("assoc .tst2=", timeout=30)
    dev.push_file("P4ASSOC.TST", b"hello assoc\r\n")
    c.expect("open dispatches to the app", "[M-ASSOCAPP]",
             dev.run("open P4ASSOC.TST", timeout=60))
    c.expect("app receives the file", "got=P4ASSOC.TST",
             dev.run("open P4ASSOC.TST", timeout=60))
    c.expect("assoc lists mappings", ".tst=ASSOCAPP",
             dev.run("assoc", timeout=30))
    c.expect("ftype shows the mapping", "ASSOCAPP=.tst",
             dev.run("ftype ASSOCAPP", timeout=30))
    # ASSOCT needs a .BAT too: `ftype` bare iterates launch discovery,
    # and an APPINFO without its shim is not an installed app. The APPS
    # directory may not exist on a clean card (s14 deploys it later), so
    # create it before pushing the metadata.
    dev.run("md APPS", timeout=15)
    _appinfo = b"title=AssocT\r\ndescription=t\r\nversion=1.0\r\nhandles=.tst,.md\r\n"
    dev.push_file("ASSOCT.BAT", b"@echo off\r\necho ASSOCT here\r\n")
    dev.push_file("APPS/ASSOCT.APPINFO", _appinfo)
    # The ACK-paced push can lose the metadata under FATFS descriptor pressure
    # in a long suite; verify the file and retry once before grading ftype.
    if "handles=" not in dev.run("type APPS/ASSOCT.APPINFO", timeout=15):
        dev.run("delay 1000", timeout=15)
        dev.push_file("APPS/ASSOCT.APPINFO", _appinfo)
    c.expect("ftype shows declared handlers", ".tst,.md",
             dev.run("ftype ASSOCT", timeout=30))
    out = dev.run("ftype", timeout=30)
    if "ASSOCT=.tst,.md" not in out:
        dev.push_file("APPS/ASSOCT.APPINFO", _appinfo)
        out = dev.run("ftype", timeout=30)
    c.expect("ftype lists declared apps", "ASSOCT=.tst,.md", out)
    dev.run("assoc .tst=", timeout=30)
    c.expect("assoc clear drops the mapping", "no association",
             dev.run("assoc .tst", timeout=30))
    # Unmapped `open` falls back to the viewer modal (owns input: dismiss
    # with q — the marker-synced echo below proves the shell is back).
    dev.session.reset_input()
    dev.session.write_line("open P4ASSOC.TST")
    dev.session.read_for(4.0)
    dev.session.write_line("q")
    dev.session.read_for(2.0)
    dev.session.write_line("q")
    c.expect("open falls back to viewer (shell back)", "P4ASSOC-OK",
             dev.run("echo P4ASSOC-OK", timeout=30))
    c.expect("file intact after fallback", "hello assoc",
             dev.run("type P4ASSOC.TST", timeout=30))
    dev.run("del /p ASSOCAPP.BAT", timeout=30)
    dev.run("del /p ASSOCT.BAT", timeout=30)
    dev.run("del /p P4ASSOC.TST", timeout=30)
    dev.run("del /p APPS/ASSOCT.APPINFO", timeout=30)
    dev.run("del /p ASSOC.INI", timeout=30)

    # ---- pkg lifecycle (gates, outdated, remove/purge, setup) -----------
    def _tstapp_bundle(version, extra=b""):
        bat = (b"@echo off\r\necho [M-TSTAPP-%s]\r\n"
               b"if \"%%1\"==\"/setup\" echo [M-TSTAPP-SETUP]\r\n" % version.encode())
        info = (b"title=TstApp\r\ndescription=t\r\nversion=%s\r\nsetup=Setup demo\r\n"
                b"data=TSTAPP.SAV\r\nalias=tst\r\nautostart=true\r\n" % version.encode()) + extra
        man = b"TSTAPP.BAT=%08X\n" % (zlib.crc32(bat) & 0xFFFFFFFF)
        return bat, info, man

    dev.run("md PKGS", timeout=30)
    dev.run("md PKGS/TSTAPP", timeout=30)
    bat, info, man = _tstapp_bundle("1.0")
    dev.push_file("PKGS/TSTAPP/TSTAPP.BAT", bat)
    dev.push_file("PKGS/TSTAPP/TSTAPP.APPINFO", info)
    dev.push_file("PKGS/TSTAPP/TSTAPP.ASSETS", man)
    out = dev.run("pkg install TSTAPP", timeout=60)
    c.expect("pkg install works", "installed", out)
    c.expect("pkg install alias hint", 'alias tst="launch TSTAPP"', out)
    c.expect("pkg install autostart hint", "startup add TSTAPP", out)
    c.expect("pkg info setup hint", "launch TSTAPP /setup",
             dev.run("pkg info TSTAPP", timeout=30))
    c.expect("launch /setup reaches setup", "[M-TSTAPP-SETUP]",
             dev.run("launch TSTAPP /setup", timeout=60))
    # startup add/list/remove (boot execution itself runs off STARTUP.INI
    # after AUTOEXEC.BAT; the boot hook shares shell_launch_app).
    c.expect("startup add", "runs at boot",
             dev.run("startup add TSTAPP", timeout=30))
    c.expect("startup lists", "TSTAPP", dev.run("startup", timeout=30))
    c.expect("startup remove", "removed",
             dev.run("startup remove TSTAPP", timeout=30))
    c.expect("startup empty", "no startup apps",
             dev.run("startup", timeout=30))
    # remove keeps data with a warning; purge deletes it.
    dev.run("echo hello > TSTAPP.SAV", timeout=30)
    c.expect("pkg remove warns kept data", "kept",
             dev.run("pkg remove TSTAPP", timeout=60))
    # outdated: staged 2.0 over installed 1.0.
    dev.run("pkg install TSTAPP", timeout=60)
    bat, info, man = _tstapp_bundle("2.0")
    dev.push_file("PKGS/TSTAPP/TSTAPP.BAT", bat)
    dev.push_file("PKGS/TSTAPP/TSTAPP.APPINFO", info)
    dev.push_file("PKGS/TSTAPP/TSTAPP.ASSETS", man)
    c.expect("pkg outdated flags staged bundle", "UPDATE TSTAPP 1.0 -> 2.0",
             dev.run("pkg outdated", timeout=30))
    c.expect("pkg upgrade installs", "installed",
             dev.run("pkg install TSTAPP", timeout=60))
    # downgrade guard + /force.
    bat, info, man = _tstapp_bundle("1.0")
    dev.push_file("PKGS/TSTAPP/TSTAPP.BAT", bat)
    dev.push_file("PKGS/TSTAPP/TSTAPP.APPINFO", info)
    dev.push_file("PKGS/TSTAPP/TSTAPP.ASSETS", man)
    c.expect("pkg downgrade refused", "older",
             dev.run("pkg install TSTAPP", timeout=60))
    c.expect("pkg force downgrades", "installed",
             dev.run("pkg install TSTAPP /force", timeout=60))
    # min_fw gate.
    bat, info, man = _tstapp_bundle("1.0", b"min_fw=9.9.9\r\n")
    dev.push_file("PKGS/TSTAPP/TSTAPP.BAT", bat)
    dev.push_file("PKGS/TSTAPP/TSTAPP.APPINFO", info)
    dev.push_file("PKGS/TSTAPP/TSTAPP.ASSETS", man)
    c.expect("pkg min_fw refused", "needs firmware",
             dev.run("pkg install TSTAPP", timeout=60))
    c.expect("pkg purge deletes data", "purged",
             dev.run("pkg purge TSTAPP", timeout=60))
    c.expect("purged data gone", "STILL",
             dev.run("if exist TSTAPP.SAV echo STILL", timeout=30), want=False)
    # Machine rows for scripts: reinstall 2.0, then parse /b output.
    bat, info, man = _tstapp_bundle("2.0")
    dev.push_file("PKGS/TSTAPP/TSTAPP.BAT", bat)
    dev.push_file("PKGS/TSTAPP/TSTAPP.APPINFO", info)
    dev.push_file("PKGS/TSTAPP/TSTAPP.ASSETS", man)
    c.expect("pkg reinstalls", "installed",
             dev.run("pkg install TSTAPP", timeout=60))
    c.expect("pkg list /b row", "TSTAPP|",
             dev.run("pkg list /b", timeout=30))
    c.expect("pkg list /b version", "|2.0|",
             dev.run("pkg list /b", timeout=30))
    c.expect("pkg info /b version", "version=2.0",
             dev.run("pkg info TSTAPP /b", timeout=30))
    c.expect("pkg info /b setup", "setup=Setup demo",
             dev.run("pkg info TSTAPP /b", timeout=30))
    c.expect("pkg info /b file row", "file=TSTAPP.BAT|ok|",
             dev.run("pkg info TSTAPP /b", timeout=30))
    c.expect("ftype app /b assignments", "ASSOCAPP=",
             dev.run("ftype ASSOCAPP /b", timeout=30))
    c.expect("startup /b lists", "TSTAPP",
             dev.run("startup add TSTAPP & startup /b", timeout=30))
    dev.run("startup remove TSTAPP", timeout=30)
    c.expect("launch /list /v version", "v2.0",
             dev.run("launch /list /v", timeout=30))
    c.expect("pkg purge again deletes data", "purged",
             dev.run("pkg purge TSTAPP", timeout=60))
    dev.run("del /p PKGS/TSTAPP/TSTAPP.BAT", timeout=30)
    dev.run("del /p PKGS/TSTAPP/TSTAPP.APPINFO", timeout=30)
    dev.run("del /p PKGS/TSTAPP/TSTAPP.ASSETS", timeout=30)
    dev.run("rd PKGS/TSTAPP", timeout=30)

    # ---- menu timeout + notify + form skip notes ----------------------
    c.expect("menu bare is usage", "Usage", dev.run("menu"))
    c.expect("menu /t:1 times out unattended", "cancelled or timed out",
             dev.run("menu /t:1 apple banana", timeout=30, settle=2.0))
    c.expect("notify accepts leading /t:", "P4NOTIFY-EL=0",
             dev.run("notify /t:5 hello & echo P4NOTIFY-EL=%ERRORLEVEL%"))
    c.expect("form skips malformed specs", "skipping",
             dev.run("form \"T\" \"badfield\" \"Name=text:P4FORMV\" /t:1 & "
                     "if errorlevel 255 echo P4FORM-SKIP", timeout=60, settle=4))
    dev.run("set P4FORMV=")

    # ---- batch sync (lock/unlock/signal/wait) ------------------------
    c.expect("lock usage", "Usage", dev.run("lock"))
    c.expect("lock takes", "P4LOCK-EL=0",
             dev.run("lock P4SYNCM & echo P4LOCK-EL=%ERRORLEVEL%"))
    c.expect("unlock releases", "P4UNLOCK-EL=0",
             dev.run("unlock P4SYNCM & echo P4UNLOCK-EL=%ERRORLEVEL%"))
    c.expect("unlock of a free mutex fails", "cannot unlock",
             dev.run("unlock P4SYNCM"))
    c.expect("bad sync names refuse", "cannot lock", dev.run("lock \"has space\""))
    # Contention: a bg job holds the mutex across a delay; the foreground
    # /t:1 attempt times out; after taskkill the mutex is free again
    # (auto-release at job exit — a dead job never wedges it). NOTE: `&`
    # chains split on the FOREGROUND before `start` sees them, so the job
    # is a real .BAT file, not an inline chain. The job signals once it
    # holds the lock, so the attempt cannot win a startup race.
    dev.push_file("P4HOLD.BAT", b"lock P4HELD\r\nsignal P4GOT\r\ndelay 8000\r\n")
    dev.run("start P4HOLD.BAT")
    c.expect("bg job took the lock", "P4GOT-EL=0",
             dev.run("wait P4GOT /t:5 & echo P4GOT-EL=%ERRORLEVEL%", timeout=30))
    c.expect("locked mutex times out", "timeout waiting",
             dev.run("lock P4HELD /t:1", timeout=30))
    dev.run("taskkill bg0")
    dev.run("delay 500", timeout=30)
    c.expect("mutex freed at job exit", "P4HELD-EL=0",
             dev.run("lock P4HELD & echo P4HELD-EL=%ERRORLEVEL%"))
    dev.run("unlock P4HELD")
    dev.run("del /p P4HOLD.BAT")
    c.expect("signal/wait wakes once", "P4WAIT-EL=0",
             dev.run("signal P4SIGW & wait P4SIGW /t:2 & echo P4WAIT-EL=%ERRORLEVEL%",
                     timeout=30))
    c.expect("second wait times out (auto-clear)", "timeout waiting",
             dev.run("wait P4SIGW /t:1", timeout=30))

    # ---- resident services (registry + supervision) -------------------
    c.expect("service list starts empty or lists", "Services",
             dev.run("service list"))
    c.expect("service install validates names", "cannot install",
             dev.run("service install \"bad name\" \"echo x\""))
    dev.run("service install P4SVC \"delay 30000\" /restart /noauto")
    c.expect("service list shows idle", "P4SVC",
             dev.run("service list"))
    dev.run("service start P4SVC")
    dev.run("delay 1000", timeout=30)
    c.expect("service runs supervised", "running",
             dev.run("service list"))
    c.expect("service remove refuses while running", "stop it first",
             dev.run("service remove P4SVC"))
    dev.run("service stop P4SVC")
    c.expect("service stop parks it", "stopped",
             dev.run("service list"))
    dev.run("service remove P4SVC")
    c.expect("service remove clears it", "P4SVC",
             dev.run("service list"), want=False)
    # Crash-loop supervision: a failing line restarts (restarts counter
    # grows across two supervisor ticks), then stop+remove cleans up.
    dev.run("service install P4CRASH \"echobadcmd_xyz\" /restart /noauto")
    dev.run("service start P4CRASH")
    dev.run("delay 12000", timeout=60)
    out = dev.run("service list")
    # The supervisor ticks are independent of this command stream; poll for the
    # first restart instead of sampling exactly at 12 s (Tab5 can lag under a
    # long sweep, which used to record `restarts 0`).
    deadline = time.time() + 15.0
    while "restarts 0" in out and time.time() < deadline:
        time.sleep(1.0)
        out = dev.run("service list")
    # Match the P4CRASH row only: other resident services (e.g. P14MOOD from
    # the app sweep) also print "restarts 0" and used to poison the substring
    # check. Between supervisor ticks the service may read idle +restart.
    crash_line = next((ln for ln in out.splitlines() if "P4CRASH" in ln), "")
    m_rest = re.search(r"restarts\s+(\d+)", crash_line)
    c.check("crashed service stays supervised",
            any(state in crash_line for state in
                ("pending", "idle +restart", "parked +restart")),
            crash_line or out[-200:])
    c.expect("crash restarts counted", "restarts", crash_line)
    c.check("crash restarts nonzero",
            bool(m_rest) and int(m_rest.group(1)) > 0,
            crash_line or out[-200:])
    dev.run("service stop P4CRASH")
    dev.run("service remove P4CRASH")
    dev.run("del /p SERVICES.INI")

    # ---- DOS parity round: glued echo, for /f eol default, shift /n, title
    c.expect("echo. with text prints it", "Hello", dev.run("echo.Hello"))
    c.expect("echo. bare is not a state report", "ECHO is", dev.run("echo."), want=False)
    c.expect("echo/ prints text", "Bye", dev.run("echo/Bye"))
    c.expect("echo( prints text", "Paren", dev.run("echo(Paren"))
    c.expect("echo: prints text", "Word", dev.run("echo:Word"))

    dev.push_file("P4BEOL.TXT", b";comment\nkeep\n")
    out = dev.run("for /f %%l in (P4BEOL.TXT) do echo P4EOL: %%l")
    c.expect("for /f skips ;-lines by default", "P4EOL: keep", out)
    c.expect("for /f default eol hides comment", "P4EOL: ;comment", out, want=False)
    out = dev.run('for /f "eol=" %%l in (P4BEOL.TXT) do echo P4EOL2: %%l')
    c.expect("for /f explicit empty eol disables filter", "P4EOL2: ;comment", out)

    dev.push_file("P4BSHIFT.BAT", b"@echo off\r\nshift /2\r\necho P4SHIFT: %1-%2\r\n")
    out = dev.run("P4BSHIFT.BAT a b c d", timeout=60)
    c.expect("shift /2 preserves %1 and slides %2", "P4SHIFT: a-c", out)
    out = dev.run("shift /9")
    c.expect("shift outside a batch refuses", "only valid inside batch files", out)

    out = dev.run("title P4SYNC Hockey")
    c.expect("title stores multi-word text", "P4SYNC Hockey", out)
    c.expect("title bare reports it", "P4SYNC Hockey", dev.run("title"))
    c.expect("sysinfo lists the title", "title:", dev.run("sysinfo"))
    dev.run('title ""')
    c.expect("title clears", "(none)", dev.run("title"))

    # ---- cleanup -----------------------------------------------------
    dev.run("del /p P4GRP.BAT")
    dev.run("del /p P4GRPBG.TXT")
    dev.run("del /p P4BEOL.TXT")
    dev.run("del /p P4BSHIFT.BAT")
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
