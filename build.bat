@echo off
setlocal

rem Builds the CANsub RP1210 driver, its tools and tests, and assembles dist\.
rem Usage: build.bat [x86|x64|both]   (default: both; dist\ needs both)
rem
rem Needs Visual Studio 2022 or later with the C++ workload, and CMake 3.27 or later. The build itself is
rem CMakeLists.txt: this script only runs its presets and copies the results into dist\.

set CMAKE=cmake
where cmake >nul 2>nul || set CMAKE=C:\Program Files\CMake\bin\cmake.exe
if not "%CMAKE%"=="cmake" if not exist "%CMAKE%" (
    echo ERROR: CMake 3.27 or later not found. Install it from cmake.org or with: winget install Kitware.CMake
    exit /b 1
)

set ROOT=%~dp0
set SDK=%ROOT%third_party\c-cpp-sdk-cansub

rem The SDK is a git submodule. mbedTLS inside it has paths longer than Windows allows by default.
if not exist "%SDK%\include\cansub\cansub_ffi.h" (
    echo ERROR: the CANsub SDK is missing from third_party\c-cpp-sdk-cansub. Fetch it with:
    echo   git config core.longpaths true
    echo   git submodule update --init
    exit /b 1
)

set TARGETS=%1
if "%TARGETS%"=="" set TARGETS=both
if /i "%TARGETS%"=="both" set TARGETS=x86 x64

pushd "%ROOT%"
for %%A in (%TARGETS%) do (
    echo === %%A ===
    "%CMAKE%" --preset %%A >"build-%%A-configure.log" 2>&1 || (type "build-%%A-configure.log" & popd & exit /b 1)
    del "build-%%A-configure.log"
    "%CMAKE%" --build --preset %%A -- -m -v:minimal -nologo || (popd & exit /b 1)
)
popd

echo %TARGETS% | findstr /i "x86" >nul || goto :nodist
echo %TARGETS% | findstr /i "x64" >nul || goto :nodist
call :dist || exit /b 1

echo.
echo Done. Binaries in build\x86\ and build\x64\, release folder in dist\
exit /b 0

:nodist
echo.
echo Done. Binaries in build\. dist\ not refreshed: it needs both x86 and x64 (build.bat with no argument).
exit /b 0

rem The release folder, zipped as is for a GitHub release. The 32-bit files stay at the top level: most
rem RP1210 diagnostic applications are 32-bit, and the instructions point there. The 64-bit driver, for
rem 64-bit hosts such as 64-bit LabVIEW, sits in x64\ so the two are never confused.
:dist
set DIST=%ROOT%dist
set LV=%DIST%\examples\labview-nixnet
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%\x64" || exit /b 1

copy /y "%ROOT%build\x86\CANSUB32.dll"            "%DIST%\" >nul || exit /b 1
copy /y "%ROOT%build\x86\rp1210_connect.exe"      "%DIST%\" >nul || exit /b 1
copy /y "%ROOT%build\x86\cansub_discover.exe"     "%DIST%\" >nul || exit /b 1
copy /y "%ROOT%ini\CANSUB32.ini"                  "%DIST%\" >nul || exit /b 1
copy /y "%ROOT%tools\install.bat"                 "%DIST%\" >nul || exit /b 1
copy /y "%ROOT%tools\uninstall.bat"               "%DIST%\" >nul || exit /b 1
copy /y "%ROOT%README.md"                        "%DIST%\" >nul || exit /b 1
copy /y "%ROOT%LICENSE"                           "%DIST%\LICENSE.txt" >nul || exit /b 1

rem 64-bit driver, with its own INI and connection test so the folder can be tested before installing.
copy /y "%ROOT%build\x64\CANSUB32.dll"            "%DIST%\x64\" >nul || exit /b 1
copy /y "%ROOT%build\x64\rp1210_connect.exe"      "%DIST%\x64\" >nul || exit /b 1
copy /y "%ROOT%ini\CANSUB32.ini"                  "%DIST%\x64\" >nul || exit /b 1

rem The LabVIEW example loads CANSUB32.dll from its own folder, so it gets the 64-bit driver and the INI.
rem For 32-bit LabVIEW, replace that DLL with the 32-bit one from the top of dist\.
xcopy /e /i /q /y "%ROOT%examples\labview-nixnet" "%LV%" >nul || exit /b 1
copy /y "%ROOT%build\x64\CANSUB32.dll"            "%LV%\" >nul || exit /b 1
copy /y "%ROOT%ini\CANSUB32.ini"                  "%LV%\" >nul || exit /b 1

rem The DLL contains the SDK's third-party code (mbedTLS, picohttpparser, parts of Zephyr), whose
rem licences require their notices to travel with it.
copy /y "%SDK%\THIRD_PARTY_NOTICES.md"            "%DIST%\THIRD_PARTY_NOTICES.txt" >nul || exit /b 1

rem RP121032.ini is deliberately absent: it is shared with other vendors' drivers on the target machine,
rem so install.bat edits it in place rather than shipping a copy that would overwrite it.

echo.
echo === dist ===
for /r "%DIST%" %%F in (*) do echo   %%F
exit /b 0
