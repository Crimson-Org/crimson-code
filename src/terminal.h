#pragma once

#include <gtk/gtk.h>

void register_terminal_settings(void);
/* A terminal running the person's own shell, drawn by this program on the bundled libvterm */
GtkWidget *create_terminal(void);
/* The colour behind text that sets none, or NULL for the plain light or dark one. Terminals redraw when their
 * style next changes, which the theme's own style sheet causes */
void set_terminal_background(const GdkRGBA *background);
/* Starts the shell in the folder if none is running */
void start_terminal(GtkWidget *terminal, const char *directory);
/* Types the command into the shell and presses Enter, starting the shell first when needed */
void run_terminal_command(GtkWidget *terminal, const char *directory, const char *command);
