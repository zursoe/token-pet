@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if errorlevel 1 (
  echo [build] vcvars64 failed
  exit /b 1
)
cd /d "%~dp0.."
if not exist build mkdir build

rc /nologo /i . /i src /fo build\tokenpet.res src\tokenpet.rc
if errorlevel 1 (
  echo [build] rc FAILED
  exit /b 1
)

cl /nologo /utf-8 /W3 /O2 /MT /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /GS- /Fo:build\ /Fe:build\token-pet.exe ^
  src\*.c src\*.cpp ^
  wsl-collector\scan_codex.c wsl-collector\scan_kimi.c wsl-collector\scan_opencode.c ^
  wsl-collector\scan_claude.c wsl-collector\tp_util_common.c wsl-collector\tp_fs_win.c ^
  third_party\sqlite\sqlite3.c third_party\cjson\cJSON.c ^
  build\tokenpet.res ^
  /I src /I wsl-collector /I third_party\sqlite /I third_party\cjson ^
  /link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO ^
  user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib advapi32.lib shlwapi.lib comctl32.lib

if errorlevel 1 (
  echo [build] FAILED
  exit /b 1
)
echo [build] OK -^> build\token-pet.exe
