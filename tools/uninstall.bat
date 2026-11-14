@echo off
rem Removes the CANsub RP1210 driver (both the 32-bit and the 64-bit DLL). Run as administrator.
rem
rem Only the DLLs are deleted. %WINDIR%\CANSUB32.ini may hold your device pins, and
rem %WINDIR%\RP121032.ini may list other vendors' drivers, so both are left alone.
rem To fully deregister, remove CANSUB32 from the APIImplementations line by hand.

del "%WINDIR%\SysWOW64\CANSUB32.dll"

rem Sysnative reaches the real System32 if this was started from a 32-bit program.
set SYSTEM64=%WINDIR%\System32
if exist "%WINDIR%\Sysnative" set SYSTEM64=%WINDIR%\Sysnative
del "%SYSTEM64%\CANSUB32.dll"

pause
