#!/bin/sh
# Generated from tasks.json by Crimson Build, edit that file instead
# Pack Crimson Code into a tar.gz archive in dist
set -eu
script_directory=$(cd "$(dirname "$0")" && pwd)
cd "$script_directory/.."

sh "$script_directory/build.sh"
export PROGRAM="crimson-code"
export APPLICATION_ID="io.github.CrimsonOrg.CrimsonCode"
export VERSION="0.1.0"
export PREFIX="${HOME}/.local"
export STD="gnu11"
export WARN="-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter"
export OPT="-O2 -g"
export GTK_MINOR="16"
export ADWAITA_MINOR="7"
export SOURCE_VIEW_MINOR="16"
export PKGS="gtk4 >= 4.${GTK_MINOR} libadwaita-1 >= 1.${ADWAITA_MINOR} gtksourceview-5 >= 5.${SOURCE_VIEW_MINOR}"
export LIBS="-lm -lutil"
export DEFINES="-DAPPLICATION_VERSION=\"${VERSION}\" -DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_${GTK_MINOR} -DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_${GTK_MINOR} -DADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_${ADWAITA_MINOR} -DADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_${ADWAITA_MINOR} -DGTK_SOURCE_VERSION_MIN_REQUIRED=GTK_SOURCE_VERSION_5_${SOURCE_VIEW_MINOR} -DGTK_SOURCE_VERSION_MAX_ALLOWED=GTK_SOURCE_VERSION_5_${SOURCE_VIEW_MINOR}"
export INCLUDES="-Isrc -Isubprojects/crimson-core/src -Isubprojects/crimson-build/src -Isrc/vterm/include"
export HEADERS="src/*/*.h subprojects/crimson-core/src/*/*.h subprojects/crimson-core/src/*/*/*.h subprojects/crimson-build/src/build/*.h"
export EXTRA_LICENSES="vterm=src/vterm/LICENSE"
printf '==> %s\n' 'sh subprojects/crimson-core/scripts/package.sh'
sh subprojects/crimson-core/scripts/package.sh
