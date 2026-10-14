@echo off
rem ---- XP test agent watchdog (single instance, recycles a hung agent) ----
taskkill /f /fi "WINDOWTITLE eq XPWatchdog" >nul 2>&1
title XPWatchdog
cd /d C:\xptest
set /a n=0
:loop
tasklist /fi "IMAGENAME eq cscript.exe" | find /i "cscript.exe" >nul
if errorlevel 1 cscript //nologo C:\xptest\agent.vbs
set /a n+=1
if %n% GEQ 100 set /a n=0 && taskkill /f /im cscript.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul
goto loop
