@echo off
rem =====================================================================
rem  SNAKE.BAT - TUI snake on packed coordinates (v = y*64+x, one env var
rem  per segment). Batch has no arrays/indirection/substrings and env caps
rem  at 24 vars, so segments are opaque packed numbers: copies compare and
rem  shift without ever splitting; split (q=y, t=x) happens only for wall
rem  checks and drawing. choice /T timeout steering (1 s pace), draw hold
rem  coalescing (one flush per frame via draw refresh).
rem  Markers: [M-SNAKE] / [M-SNAKE-EAT] / [M-SNAKE-DONE].
rem  Foreground-only: draw refuses in background jobs (see :nogfx).
rem  Grid: x 3..42, y 4..21 maps to packed max 21*64+42=1386. Sentinel 6435
rem  (99,99) never matches a live head; guarded draws keep it off-screen.
rem =====================================================================
setlocal
echo [M-SNAKE]
draw hold on
if errorlevel 1 goto nogfx
draw box 1 2 44 22 double SNAKE
if errorlevel 1 goto nogfx
set hh=596
set dir=1
set dc=d
set len=3
set score=0
set s0=596
set s1=595
set s2=594
set s3=6435
set s4=6435
set s5=6435
set s6=6435
set s7=6435
set s8=6435
set s9=6435
set s10=6435
set s11=6435
goto newfood

:newfood
set /a fx=%RANDOM% / 820 + 3
set /a fy=%RANDOM% / 1821 + 4
set /a nh=%fy% * 64 + %fx%
if %nh%==%s0% goto newfood
if %nh%==%s1% goto newfood
if %nh%==%s2% goto newfood
if %nh%==%s3% goto newfood
if %nh%==%s4% goto newfood
if %nh%==%s5% goto newfood
if %nh%==%s6% goto newfood
if %nh%==%s7% goto newfood
if %nh%==%s8% goto newfood
if %nh%==%s9% goto newfood
if %nh%==%s10% goto newfood
if %nh%==%s11% goto newfood
goto frame

:frame
draw clear
draw box 1 2 44 22 double SNAKE
draw text 48 4 Score %score%
draw text 48 6 Length %len%
draw text 48 8 wasd move
draw text 48 9 Enter on serial
draw text 48 10 q quit
draw text 48 12 1s per step
draw text %fx% %fy% *
set /a q=%s0% / 64
set /a t=%s0% - %q% * 64
draw text %t% %q% @
set /a q=%s1% / 64
set /a t=%s1% - %q% * 64
draw text %t% %q% O
set /a q=%s2% / 64
set /a t=%s2% - %q% * 64
draw text %t% %q% O
set /a q=%s3% / 64
set /a t=%s3% - %q% * 64
if %len% GTR 3 draw text %t% %q% O
set /a q=%s4% / 64
set /a t=%s4% - %q% * 64
if %len% GTR 4 draw text %t% %q% O
set /a q=%s5% / 64
set /a t=%s5% - %q% * 64
if %len% GTR 5 draw text %t% %q% O
set /a q=%s6% / 64
set /a t=%s6% - %q% * 64
if %len% GTR 6 draw text %t% %q% O
set /a q=%s7% / 64
set /a t=%s7% - %q% * 64
if %len% GTR 7 draw text %t% %q% O
set /a q=%s8% / 64
set /a t=%s8% - %q% * 64
if %len% GTR 8 draw text %t% %q% O
set /a q=%s9% / 64
set /a t=%s9% - %q% * 64
if %len% GTR 9 draw text %t% %q% O
set /a q=%s10% / 64
set /a t=%s10% - %q% * 64
if %len% GTR 10 draw text %t% %q% O
set /a q=%s11% / 64
set /a t=%s11% - %q% * 64
if %len% GTR 11 draw text %t% %q% O
draw refresh
if %dir% EQU 0 set dc=w
if %dir% EQU 1 set dc=d
if %dir% EQU 2 set dc=s
if %dir% EQU 3 set dc=a
choice /C:wasdq /N /T:%dc%,1 Move?
if errorlevel 5 goto quit
if errorlevel 4 goto go_d
if errorlevel 3 goto go_s
if errorlevel 2 goto go_a
goto go_w
:go_d
if %dir% EQU 3 goto move
set dir=1
goto move
:go_a
if %dir% EQU 1 goto move
set dir=3
goto move
:go_s
if %dir% EQU 0 goto move
set dir=2
goto move
:go_w
if %dir% EQU 2 goto move
set dir=0
goto move

:move
if %dir% EQU 0 set /a nh=%hh% - 64
if %dir% EQU 1 set /a nh=%hh% + 1
if %dir% EQU 2 set /a nh=%hh% + 64
if %dir% EQU 3 set /a nh=%hh% - 1
set /a q=%nh% / 64
set /a t=%nh% - %q% * 64
if %q% LSS 4 goto dead
if %q% GTR 21 goto dead
if %t% LSS 3 goto dead
if %t% GTR 42 goto dead
if %nh%==%s0% goto dead
if %nh%==%s1% goto dead
if %nh%==%s2% goto dead
if %nh%==%s3% goto dead
if %nh%==%s4% goto dead
if %nh%==%s5% goto dead
if %nh%==%s6% goto dead
if %nh%==%s7% goto dead
if %nh%==%s8% goto dead
if %nh%==%s9% goto dead
if %nh%==%s10% goto dead
if %nh%==%s11% goto dead
set /a q=%fy% * 64 + %fx%
if %q%==%nh% goto eat
if %len% GTR 11 set s11=%s10%
if %len% GTR 10 set s10=%s9%
if %len% GTR 9 set s9=%s8%
if %len% GTR 8 set s8=%s7%
if %len% GTR 7 set s7=%s6%
if %len% GTR 6 set s6=%s5%
if %len% GTR 5 set s5=%s4%
if %len% GTR 4 set s4=%s3%
if %len% GTR 3 set s3=%s2%
if %len% GTR 2 set s2=%s1%
set s1=%s0%
set s0=%nh%
set hh=%nh%
goto frame

:eat
set /a score+=10
set /a len+=1
echo [M-SNAKE-EAT] %score%
if %len% GEQ 12 goto win
tone 880 80
goto newfood

:dead
draw text 48 13 GAME OVER
draw text 48 14 Score %score%
draw refresh
screenshot SNAKE.BMP
echo [M-SNAKE-DONE] score=%score% len=%len%
goto end

:win
draw text 48 13 YOU WIN
draw text 48 14 Score %score%
draw refresh
screenshot SNAKE.BMP
echo [M-SNAKE-DONE] score=%score% len=%len% WIN
goto end

:quit
draw text 48 13 QUIT
draw text 48 14 Score %score%
draw refresh
echo [M-SNAKE-DONE] score=%score% len=%len% QUIT
goto end

:nogfx
echo SNAKE needs the foreground display (no draw in background jobs)
goto end

:end
draw hold off
draw close
endlocal
