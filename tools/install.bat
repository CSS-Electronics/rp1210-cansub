@echo off
rem Installs the CANsub RP1210 driver. Run as administrator, from the folder holding these files.
rem Every step is below in full. README.md (Install details) explains them.

rem 1. The driver itself, in both bitnesses, so 32-bit and 64-bit applications each find one.
rem     On 64-bit Windows, 32-bit DLLs live in SysWOW64 and 64-bit DLLs in System32, under the same
rem     name: a 32-bit application asking for CANSUB32.dll is sent to SysWOW64 by Windows, a 64-bit
rem     one (e.g. 64-bit LabVIEW) gets System32.
copy /y "%~dp0CANSUB32.dll" "%WINDIR%\SysWOW64\CANSUB32.dll"

rem     If this script is started from a 32-bit program, Windows quietly redirects System32 to
rem     SysWOW64; Sysnative is the way through to the real System32 in that case.
set SYSTEM64=%WINDIR%\System32
if exist "%WINDIR%\Sysnative" set SYSTEM64=%WINDIR%\Sysnative
copy /y "%~dp0x64\CANSUB32.dll" "%SYSTEM64%\CANSUB32.dll"

rem 2. The device configuration, shared by both. It is replaced, keeping a copy of whatever was there
rem     before: the driver finds the device by itself, so the file is the same on every machine, and an
rem     update must be able to deliver a corrected one. Re-apply any host= pins from the backup.
if exist "%WINDIR%\CANSUB32.ini" (
    copy /y "%WINDIR%\CANSUB32.ini" "%WINDIR%\CANSUB32.ini.backup" >nul
    echo Previous configuration saved as %WINDIR%\CANSUB32.ini.backup
)
copy /y "%~dp0CANSUB32.ini" "%WINDIR%\CANSUB32.ini"

rem 3. Register the driver so applications can find it. RP121032.ini is shared: it may already list
rem     other vendors' drivers, so this only creates it when absent. If it already exists, add
rem     CANSUB32 to its APIImplementations line by hand. See README.md.
rem     One entry covers both bitnesses: 64-bit applications read the same file.
if exist "%WINDIR%\RP121032.ini" echo Add CANSUB32 to APIImplementations in %WINDIR%\RP121032.ini by hand
if not exist "%WINDIR%\RP121032.ini" (
    echo [RP1210Support]>"%WINDIR%\RP121032.ini"
    echo APIImplementations=CANSUB32>>"%WINDIR%\RP121032.ini"
)

pause
