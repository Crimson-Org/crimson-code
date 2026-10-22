#pragma once

#include <gtk/gtk.h>

/* Called with the chosen task, or with NULL when the task file should be opened for editing */
typedef void (*TaskChosenFunction)(const char *name, gpointer data);

/* A header button whose menu lists the project's tasks, read again each time it opens */
GtkWidget *create_task_button(TaskChosenFunction on_chosen, gpointer data);
void set_task_button_project(GtkWidget *button, const char *project);
void show_task_menu(GtkWidget *button);
