#pragma once

#include <gtk/gtk.h>

/* Called each time rows read on a worker are shown */
typedef void (*JsonEditorReady)(gpointer data);

/* Edits a JSON file as a compact tree of named values that can be edited in place, added, removed, retyped and
 * dragged into order, changing only the affected text so comments, spacing and undo survive */
GtkWidget *create_json_editor(GtkTextBuffer *buffer, JsonEditorReady on_ready, gpointer data);
/* Reads the text again on a worker, after it changed while the visual editor was hidden */
void reload_json_editor(GtkWidget *editor);
/* Shows rows read from the file on disk while its text still loads into the buffer, unclickable until
 * reload_json_editor reads them again from the loaded text */
void preview_json_editor_file(GtkWidget *editor, GFile *file);
/* Sets the text put before each level, such as a tab or four spaces */
void set_json_editor_indent(GtkWidget *editor, const char *indent);
void free_json_editor(GtkWidget *editor);
