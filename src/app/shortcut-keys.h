#pragma once

#include <gtk/gtk.h>

/* Shows a shortcut such as <Control><Shift>p as a row of key caps, or returns NULL when it cannot be read */
GtkWidget *create_shortcut_keys(const char *accelerator);
