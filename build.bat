@echo off
set SRC_EXE=C:\Users\Knownniu\Desktop\VolumeTool\VolumeTool\build\Desktop_Qt_6_11_1_MSVC2022_64bit-Release\VolumeTool.exe
set DIST_DIR=C:\Users\Knownniu\Desktop\VolumeTool\dist\VolumeTool
set QT_BIN=D:\Software\Qt\6.11.1\msvc2022_64\bin

if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"

copy /Y "%SRC_EXE%" "%DIST_DIR%\VolumeTool.exe"

"%QT_BIN%\windeployqt.exe" --release "%DIST_DIR%\VolumeTool.exe"

pause