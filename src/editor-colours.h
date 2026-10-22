#pragma once

#include <gtksourceview/gtksource.h>

void register_colour_settings(void);
/* A source view that draws a small circle in the colour before each colour code such as #ff8800 */
GtkSourceView *create_code_view(GtkSourceBuffer *buffer);
/* Keeps the colour codes of a buffer marked while it is edited */
void watch_colour_codes(GtkSourceBuffer *buffer);
void unwatch_colour_codes(GtkSourceBuffer *buffer);
