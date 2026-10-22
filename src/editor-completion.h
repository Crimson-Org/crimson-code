#pragma once

#include <gtk/gtk.h>

typedef struct Editor Editor;

void register_completion_settings(void);
/* Language server completion, shown while typing and on Ctrl+Space */
void add_editor_completion(Editor *editor);
void remove_editor_completion(Editor *editor);
void add_editor_completion_keys(Editor *editor, GtkWidget *scroller);
/* Asks for suggestions at the cursor at once, as Ctrl+Space does */
void show_editor_suggestions(Editor *editor);
