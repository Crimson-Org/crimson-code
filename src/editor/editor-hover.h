#pragma once

#include <gtk/gtk.h>

typedef struct Editor Editor;

/* Hover cards that show the problems under the pointer and the language server's documentation */
void add_editor_hover(Editor *editor);
/* Opens the card for a line's problems at once, as clicking a problem icon does */
void show_editor_line_hover(Editor *editor, int line, const GdkRectangle *area);
/* Opens the card for the problems and documentation at the cursor, for keyboard and Vim use */
void show_editor_cursor_hover(Editor *editor);
/* Detaches the cards from an editor that is going away, as a hover may still be on screen */
void remove_editor_hover(Editor *editor);
