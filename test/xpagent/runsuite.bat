@echo off
set E=C:\xptest\ExamTimer.exe
set R=C:\xptest\rt
if not exist %R% mkdir %R%
del /f /q %R%\*.avi %R%\*.log %R%\suite.log >nul 2>&1
echo ==== T1 basic 1024x768 5fps q75 ==== >> %R%\suite.log
%E% --run-seconds 6 --fps 5 --out %R%\t1.avi --no-tray --log %R%\t1.log >> %R%\suite.log 2>&1
echo ==== T2 scale 50 ==== >> %R%\suite.log
%E% --run-seconds 5 --fps 4 --scale 50 --out %R%\t2.avi --no-tray --log %R%\t2.log >> %R%\suite.log 2>&1
echo ==== T3 region 200,100,400,300 ==== >> %R%\suite.log
%E% --run-seconds 5 --fps 4 --region 200,100,400,300 --out %R%\t3.avi --no-tray --log %R%\t3.log >> %R%\suite.log 2>&1
echo ==== T4 countdown 30s ==== >> %R%\suite.log
%E% --run-seconds 5 --fps 4 --countdown 30 --out %R%\t4.avi --no-tray --log %R%\t4.log >> %R%\suite.log 2>&1
echo ==== T5 granularity second pos topleft ==== >> %R%\suite.log
%E% --run-seconds 5 --fps 4 --granularity second --pos topleft --out %R%\t5.avi --no-tray --log %R%\t5.log >> %R%\suite.log 2>&1
echo ==== T6 split 1MB ==== >> %R%\suite.log
%E% --run-seconds 8 --fps 5 --split-mb 1 --out %R%\t6.avi --no-tray --log %R%\t6.log >> %R%\suite.log 2>&1
echo ==== T7 quality 40 ==== >> %R%\suite.log
%E% --run-seconds 5 --fps 5 --quality 40 --out %R%\t7.avi --no-tray --log %R%\t7.log >> %R%\suite.log 2>&1
echo ==== T8 4:4:4 subsample ==== >> %R%\suite.log
%E% --run-seconds 5 --fps 5 --subsample 0 --out %R%\t8.avi --no-tray --log %R%\t8.log >> %R%\suite.log 2>&1
echo ==== T9 no-burnin no-overlay ==== >> %R%\suite.log
%E% --run-seconds 4 --fps 3 --no-burnin --no-overlay --out %R%\t9.avi --no-tray --log %R%\t9.log >> %R%\suite.log 2>&1
echo ==== T10 fps 15 ==== >> %R%\suite.log
%E% --run-seconds 5 --fps 15 --quality 60 --out %R%\t10.avi --no-tray --log %R%\t10.log >> %R%\suite.log 2>&1
echo ALLDONE >> %R%\suite.log
dir %R% > %R%\files.txt
echo finished
