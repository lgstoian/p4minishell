@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  PKGTEST.BAT - minimal package-management demo app. Installed from the
rem  bundle PKGS/PKGTEST by `pkg install PKGTEST`; uninstalled with
rem  `pkg remove PKGTEST`. Markers: [M-PKGTEST] / [M-PKGTEST-DONE].
rem =====================================================================
echo [M-PKGTEST] 
if "%P4_APP_FEATURES%"=="1" gosub %APPROOT%/APPLIB.BAT::probe
title "Package Test"
echo pkgtest: installed package is running on %APPDRIVE%
rem ---- v1.6 richer-language showcase -----------------------------------
rem [M-RICH-PKGTEST] lists the manifest with map /set + for /K and computes
rem a checksum-like value with func/return and str /pad.
echo [M-RICH-PKGTEST] manifest language tour
set PKG=PKGTEST
str /count PKG T CT
str /pad PKG 10 PP .
map /set MANIFEST app PKGTEST
map /set MANIFEST type hybrid
for /K %%k in (MANIFEST) do echo [M-RICH-PKGTEST-K] %%k=%%l
func :pkgsum 3 7
echo [M-RICH-PKGTEST-F] sum=%RESULT%
echo [M-RICH-PKGTEST-S] T=%CT% pad=%PP%
echo [M-PKGTEST-DONE]
goto :eof

:pkgsum
return %1*%2
