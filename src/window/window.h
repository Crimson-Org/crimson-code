#pragma once

#include <gtk/gtk.h>

void register_window_settings(void);
/* Registers the built in commands that each window offers */
void register_window_commands(void);
/* One project window with its tree, file tabs, terminal and status bar */
typedef struct Window Window;

Window *create_window(GtkApplication *application);
GtkWindow *get_gtk_window(Window *window);
/* Opens a folder as the project or a file in a tab */
void open_window_path(Window *window, GFile *file);
/* Keeps the empty page blank while paths wait to open */
void set_window_loading(Window *window, gboolean is_loading);
/* Opens the last project when the window was started without anything to open */
void restore_window_project(Window *window);
