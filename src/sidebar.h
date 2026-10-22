#pragma once

#include <gtk/gtk.h>

typedef void (*FileChosenFunction)(GFile *file, gpointer data);

void register_sidebar_settings(void);
/* The name patterns the project tree leaves out, split from the setting */
GStrv list_excluded_project_names(void);
/* Whether the project tree shows a file or folder name, which also works away from the main thread */
gboolean is_project_name_shown(const char *name, GStrv excluded, gboolean shows_hidden);
/* The project tree, which reads each folder in the background only when it is opened */
GtkWidget *create_sidebar(FileChosenFunction on_chosen, gpointer data);
/* Lists the folder's files, or nothing when folder is NULL, opening the given folders inside it as they are read */
void set_sidebar_folder(GtkWidget *sidebar, GFile *folder, const char *const *open_folders);
/* Paths inside the project of the folders open in the tree */
GStrv list_sidebar_open_folders(GtkWidget *sidebar);
