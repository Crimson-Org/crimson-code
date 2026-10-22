#pragma once

#include "commands.h"

/* Typed first in the palette to list commands instead of files */
#define COMMAND_PALETTE_PREFIX ">"
/* Typed first in the palette to run a Vim command line such as :w name */
#define COMMAND_LINE_PREFIX ":"

/* Finds project files by name, commands after a > and Vim commands after a :, starting with the given text */
void show_command_palette(Window *window, const char *start_text);
