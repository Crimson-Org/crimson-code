@echo off
rem Generated from tasks.json by Crimson Build, edit that file instead
rem Pack Crimson Code into a tar.gz archive in dist
setlocal
cd /d "%~dp0.."

call "%~dp0build.cmd"
if errorlevel 1 exit /b %errorlevel%
set "PROGRAM=crimson-code"
set "APPLICATION_ID=io.github.CrimsonOrg.CrimsonCode"
set "VERSION=0.1.0"
set "PREFIX=%HOME%/.local"
set "STD=gnu11"
set "WARN=-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter"
set "OPT=-O2 -g"
set "PKGS=gtk4 libadwaita-1 gtksourceview-5"
set "LIBS=-lm -lutil"
set "DEFINES=-DAPPLICATION_VERSION=%VERSION%"
set "INCLUDES=-Isrc -Isubprojects/crimson-core/src -Isubprojects/crimson-build/src -Isrc/vterm/include"
set "EXTRA_LICENSES=vterm=src/vterm/LICENSE"
echo(==^> sh subprojects/crimson-core/scripts/package.sh
sh subprojects/crimson-core/scripts/package.sh
if errorlevel 1 exit /b %errorlevel%
exit /b 0
