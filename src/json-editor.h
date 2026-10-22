#pragma once

#include <gtk/gtk.h>

/* Edits a JSON file as a compact tree of named values that can be edited in place, added, removed, retyped and
 * dragged into order, changing only the affected text so comments, spacing and undo survive */
GtkWidget *create_json_editor(GtkTextBuffer *buffer);
/* Reads the text again, after it changed while the visual editor was hidden */
void reload_json_editor(GtkWidget *editor);
/* Sets the text put before each level, such as a tab or four spaces */
void set_json_editor_indent(GtkWidget *editor, const char *indent);
void free_json_editor(GtkWidget *editor);
