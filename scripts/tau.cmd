@echo off
rem works with the engine it belongs to, run it from a game project (this script is untested btw):
rem   tau configure [-m mode]   record this engine in the project
rem   tau build [-m mode]       configure, then build
rem   tau open [project]        open the project in this engine's editor
rem   tau <anything else>       passed to xmake
setlocal

rem bin\ in a packaged engine, scripts\ in a checkout
for %%I in ("%~dp0..") do set "engine_dir=%%~fI"

where xmake >nul 2>nul
if errorlevel 1 (
    echo tau: xmake was not found on PATH, install it from https://xmake.io 1>&2
    exit /b 1
)

if /i "%~1"=="configure" goto cmd_configure
if /i "%~1"=="build" goto cmd_build
if /i "%~1"=="open" goto cmd_open
call xmake %*
exit /b %errorlevel%

:cmd_configure
call :parse_mode "%~2" "%~3"
call :configure
exit /b %errorlevel%

:cmd_build
call :parse_mode "%~2" "%~3"
call :configure
if errorlevel 1 exit /b %errorlevel%
call xmake build -P .
exit /b %errorlevel%

:cmd_open
call :find_editor
if not defined editor (
    echo tau: no editor found in %engine_dir%, build the engine first 1>&2
    exit /b 1
)
set "project=%~2"
if not defined project set "project=."
"%editor%" "%project%"
exit /b %errorlevel%

:configure
call xmake f -P . -y -m "%mode%" "--tau_engine_dir=%engine_dir%"
exit /b %errorlevel%

:parse_mode
set "mode="
if /i "%~1"=="-m" set "mode=%~2"
if not defined mode call :default_mode
exit /b 0

rem a project must be built in its editor's mode
:default_mode
set "mode=release"
call :find_editor
if not defined editor exit /b 0
if /i "%editor%"=="%engine_dir%\bin\tau-editor.exe" exit /b 0
for %%D in ("%editor%") do set "editor_dir=%%~dpD"
for %%D in ("%editor_dir:~0,-1%") do set "mode=%%~nxD"
exit /b 0

rem a packaged engine's editor, else the most recently built one in a checkout
:find_editor
set "editor="
if exist "%engine_dir%\bin\tau-editor.exe" (
    set "editor=%engine_dir%\bin\tau-editor.exe"
    exit /b 0
)
set "pattern=%engine_dir:'=''%\build\*\*\*\tau-editor.exe"
for /f "usebackq delims=" %%E in (`powershell -NoProfile -Command "Get-Item -Path '%pattern%' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName"`) do set "editor=%%E"
exit /b 0
