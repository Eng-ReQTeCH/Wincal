@echo off
setlocal
set "ARCH=%~1"
if not defined ARCH set "ARCH=x64"
if /i "%ARCH%"=="x64" set "VC_COMPONENT=Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
if /i "%ARCH%"=="arm64" set "VC_COMPONENT=Microsoft.VisualStudio.Component.VC.Tools.ARM64"
if not defined VC_COMPONENT (
    echo Usage: build.cmd x64 ^| arm64
    exit /b 2
)
if /i "%VSCMD_ARG_TGT_ARCH%"=="%ARCH%" goto compile
for /f "usebackq tokens=*" %%V in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires %VC_COMPONENT% -find Common7\Tools\VsDevCmd.bat`) do set "VSDEV=%%V"
if not defined VSDEV (
    echo Install the Visual Studio C++ build component for %ARCH% and the Windows 11 SDK.
    exit /b 2
)
call "%VSDEV%" -no_logo -host_arch=amd64 -arch=%ARCH%
if errorlevel 1 exit /b 2
:compile
pushd "%~dp0"
set "OUT=build\%ARCH%"
if not exist "%OUT%" mkdir "%OUT%"
set "CXX=/nologo /std:c++20 /utf-8 /EHsc /W4 /MT /O2 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN"
cl %CXX% /c /Fo"%OUT%\calendar.obj" src\calendar.cpp || exit /b 1
cl %CXX% /c /Fo"%OUT%\cache.obj" src\cache.cpp || exit /b 1
cl %CXX% /c /Fo"%OUT%\launcher.obj" src\launcher.cpp || exit /b 1
cl %CXX% /c /Fo"%OUT%\flyout.obj" src\flyout.cpp || exit /b 1
set "LIBS=user32.lib gdi32.lib ole32.lib oleaut32.lib runtimeobject.lib windowsapp.lib winhttp.lib crypt32.lib xmllite.lib shlwapi.lib shell32.lib"
link /nologo /subsystem:windows /out:"%OUT%\Win11CalendarNative.exe" "%OUT%\launcher.obj" "%OUT%\calendar.obj" "%OUT%\cache.obj" %LIBS% || exit /b 1
link /nologo /dll /def:src\Flyout.def /out:"%OUT%\CalendarFlyout-044.dll" "%OUT%\flyout.obj" %LIBS% || exit /b 1
cl %CXX% /Fe"%OUT%\calendar_test.exe" /Fo"%OUT%\calendar_test.obj" tests\calendar_test.cpp "%OUT%\calendar.obj" "%OUT%\cache.obj" /link %LIBS% || exit /b 1
if /i "%ARCH%"=="x64" "%OUT%\calendar_test.exe" || exit /b 1
cl %CXX% /Fe"%OUT%\attachment_test.exe" /Fo"%OUT%\attachment_test.obj" tests\attachment_test.cpp /link %LIBS% || exit /b 1
if /i "%ARCH%"=="x64" "%OUT%\attachment_test.exe" || exit /b 1
cl %CXX% /Fe"%OUT%\flyout_test.exe" /Fo"%OUT%\flyout_test.obj" tests\flyout_test.cpp "%OUT%\calendar.obj" "%OUT%\cache.obj" /link %LIBS% /MANIFEST:EMBED /MANIFESTINPUT:tests\flyout_test.manifest || exit /b 1
set "CSC=%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
set "RESOURCES=/resource:%OUT%\Win11CalendarNative.exe,Win11CalendarNative.exe /resource:%OUT%\CalendarFlyout-044.dll,CalendarFlyout-044.dll /resource:installer\Uninstall.cmd,Uninstall.cmd"
set "SETUP_SOURCES=installer\Setup.cs installer\SetupForm.cs"
set "SETUP=build\wincal-setup-%ARCH%.exe"
if not "%~2"=="" set "SETUP=%~2"
"%CSC%" /nologo /target:winexe /platform:anycpu /optimize+ /define:%ARCH% /out:"%SETUP%" /r:System.Security.dll %RESOURCES% %SETUP_SOURCES% || exit /b 1
if /i "%ARCH%"=="x64" (
    "%CSC%" /nologo /target:exe /main:InstallerTest /out:"%OUT%\installer_test.exe" /r:System.Security.dll %RESOURCES% %SETUP_SOURCES% tests\InstallerTest.cs || exit /b 1
    "%OUT%\installer_test.exe" || exit /b 1
)
popd
