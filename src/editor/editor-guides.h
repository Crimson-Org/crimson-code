#pragma once

#include <gtk/gtk.h>

void register_guide_settings(void);
/* Draws a faint line at each indentation level of the visible lines and a faint wide line at each margin column,
 * in buffer coordinates below the text */
void draw_indent_guides(GtkTextView *view, GtkSnapshot *snapshot);
/* Marks runs of several spaces inside lines, which the source view's own space drawing cannot pick out from single
 * spaces between words */
void draw_space_runs(GtkTextView *view, GtkSnapshot *snapshot);
