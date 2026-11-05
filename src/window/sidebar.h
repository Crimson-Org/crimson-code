#pragma once

#include <adwaita.h>

typedef void (*FileChosenFunction)(GFile *file, gpointer data);

void register_sidebar_settings(void);
/* The name patterns the project tree leaves out, split from the setting */
GStrv list_excluded_project_names(void);
/* Whether the project tree shows a file or folder name, which also works away from the main thread */
gboolean is_project_name_shown(const char *name, GStrv excluded, gboolean shows_hidden);
/* Called for each regular file found while visiting the project */
typedef void (*ProjectFileFunction)(GFile *file, GFileInfo *info, gpointer data);
/* Visits each file the project tree would show below folder, reading the given attributes, which must hold the
 * name and type, and also works away from the main thread */
void visit_project_files(GFile *folder, GStrv excluded, gboolean shows_hidden, const char *attributes,
	ProjectFileFunction on_file, gpointer data, GCancellable *cancellable);
/* A sidebar page under a header bar of its own, which lines up with the window's title bar beside it */
GtkWidget *create_sidebar_page(const char *title, GtkWidget *content, AdwHeaderBar **header);
/* The type icon for a file name, guessed from the name alone so long lists read no files */
GIcon *get_file_name_icon(const char *name);
/* The project tree, which reads each folder in the background only when it is opened */
GtkWidget *create_sidebar(FileChosenFunction on_chosen, gpointer data);
/* Lists the folder's files, or nothing when folder is NULL, opening the given folders inside it as they are read */
void set_sidebar_folder(GtkWidget *sidebar, GFile *folder, const char *const *open_folders);
/* Paths inside the project of the folders open in the tree */
GStrv list_sidebar_open_folders(GtkWidget *sidebar);
