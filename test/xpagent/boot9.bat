@echo off
taskkill /f /im cscript.exe >nul 2>&1
attrib -r -h -s C:\xptest\agent.vbs >nul 2>&1
del /f /q C:\xptest\agent.vbs >nul 2>&1
tftp -i 192.168.1.116 GET agent.vbs C:\xptest\agent.vbs
dir C:\xptest\agent.vbs > C:\xptest\b9log.txt
attrib -r C:\xptest\agent.vbs >nul 2>&1
tftp -i 192.168.1.116 PUT C:\xptest\b9log.txt b9log.txt
start "" C:\xptest\runagent.bat
