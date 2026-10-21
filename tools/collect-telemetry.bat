@echo off
rem Collect bring-up telemetry from the phone over adb (no flashing).
rem Works both in TWRP/recovery (adb is root there) and in a rooted
rem userdebug ROM. Everything lands in C:\hackios\ as plain text files.
setlocal enabledelayedexpansion

set "ADB=C:\Users\nzzna\Downloads\platform-tools\adb.exe"
set "OUT=C:\hackios"

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%ADB%" goto noadb

echo ============================================================
echo  HackiOS begonia - telemetry collector
echo ============================================================
echo.

echo [1/7] adb devices
"%ADB%" devices
"%ADB%" get-state >nul 2>&1
if errorlevel 1 goto nodevice
echo.

echo [2/7] asking adb for root (silently ignored if not supported)
"%ADB%" root >nul 2>&1
"%ADB%" wait-for-device
timeout /t 2 >nul

echo [3/7] /proc/cmdline -> cmdline.txt
"%ADB%" exec-out cat /proc/cmdline > "%OUT%\cmdline.txt" 2>&1

echo [4/7] dmesg -> dmesg.txt
"%ADB%" exec-out dmesg > "%OUT%\dmesg.txt" 2>&1

echo [5/7] graphics nodes -> graphics.txt / fb0_*.txt
"%ADB%" shell ls -l /sys/class/graphics/ > "%OUT%\graphics.txt" 2>&1
for %%f in (name virtual_size stride) do "%ADB%" shell cat /sys/class/graphics/fb0/%%f > "%OUT%\fb0_%%f.txt" 2>&1

echo [6/7] pulling TWRP backups (boot.emmc.win, ~64 MB) - may take a minute
"%ADB%" shell ls /sdcard/TWRP/BACKUPS > "%OUT%\twrp_backups.txt" 2>&1
"%ADB%" pull /sdcard/TWRP/BACKUPS "%OUT%\TWRP_BACKUPS" > "%OUT%\pull.log" 2>&1

echo [7/7] done
echo.
echo ===== /proc/cmdline =====
type "%OUT%\cmdline.txt" 2>nul
echo.
echo ===== fb / disp / lcm lines from dmesg =====
findstr /i /c:"fb" /c:"disp" /c:"lcm" /c:"panel" "%OUT%\dmesg.txt" 2>nul
echo.
findstr /i /c:"Permission denied" /c:"not found" "%OUT%\dmesg.txt" >nul 2>&1
if not errorlevel 1 (
    echo NOTE: dmesg was denied. Re-run this script while the phone is in TWRP,
    echo       or try: adb shell su -c dmesg ^> C:\hackios\dmesg.txt
)
echo.
echo All files saved to %OUT%
echo Tell the assistant "gotovo" - it will read them from the PC.
echo.
pause
goto :eof

:noadb
echo ERROR: adb.exe not found at %ADB%
echo Install platform-tools or fix the ADB path inside this script.
pause
goto :eof

:nodevice
echo.
echo ERROR: no device visible to adb.
echo  * phone in bootloop? hold Power 20 s, then hold Volume+ + Power
echo    to boot TWRP (adb works there too)
echo  * check the USB cable and "adb devices"
pause
goto :eof
