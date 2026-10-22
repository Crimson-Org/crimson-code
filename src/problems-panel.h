#pragma once

#include <gtk/gtk.h>

#include "editor.h"

/* Called with the file and the spot, in the language server's units, of the chosen problem */
typedef void (*ProblemChosenFunction)(GFile *file, int line, int column, gpointer data);

/* A list of the problems in the open files, for the bottom panel */
GtkWidget *create_problems_panel(ProblemChosenFunction on_chosen, gpointer data);
/* Lists the problems of these editors again, once per file even when several views show it */
void set_problems_panel_editors(GtkWidget *panel, GPtrArray *editors);
/* Puts the keyboard on the first problem, so the list works without a mouse */
void focus_problems_panel(GtkWidget *panel);
