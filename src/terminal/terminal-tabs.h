#pragma once

#include <gtk/gtk.h>

/* Called when the last terminal tab closed, so the panel holding them can go */
typedef void (*TerminalsEmptiedFunction)(gpointer data);

/* Terminals in tabs, with the tab bar showing only while there are several */
GtkWidget *create_terminal_tabs(TerminalsEmptiedFunction on_emptied, gpointer data);
/* Opens another terminal with its shell in the folder and brings it forward */
GtkWidget *add_terminal_tab(GtkWidget *tabs, const char *directory);
/* The terminal in front, opening one in the folder when there is none */
GtkWidget *get_current_terminal(GtkWidget *tabs, const char *directory);
