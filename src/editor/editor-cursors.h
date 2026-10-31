#pragma once

#include <gtk/gtk.h>

/* Extra cursors and selections beside the text view's own, which typing, deleting and moving apply to as well */
void add_editor_cursors(GtkTextView *view, GtkWidget *scroller);
void remove_editor_cursors(GtkTextView *view);
/* Selects the word at the cursor, or adds the next match of the selection as another cursor, as Ctrl+D does in
 * most editors */
void select_next_match(GtkTextView *view);
/* Puts a cursor on every match of the selection or of the word at the cursor */
void select_all_matches(GtkTextView *view);
/* Draws the extra cursors, in buffer coordinates above the text */
void draw_extra_cursors(GtkTextView *view, GtkSnapshot *snapshot);
