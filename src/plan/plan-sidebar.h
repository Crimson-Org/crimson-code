#pragma once

#include <gtk/gtk.h>

/* What the plan asks of the window */
typedef struct {
	/* Opens the file at a line numbered from 0 */
	void (*spot_chosen)(GFile *file, int line, gpointer data);
	/* The text of the file in an open editor, or NULL when no editor has it */
	char *(*get_open_text)(GFile *file, gpointer data);
	/* Hands new text to an open editor, which saves it, returning FALSE when no editor has the file */
	gboolean (*change_open_text)(GFile *file, const char *text, gpointer data);
	void (*failed)(const char *message, gpointer data);
	gpointer data;
} PlanEvents;

void register_plan_settings(void);
/* The plan files of the project with their tasks, then the files with tagged comments, read when first shown */
GtkWidget *create_plan_sidebar(const PlanEvents *events);
/* Shows the folder's plan, or nothing when folder is NULL, opening the rows named by their keys once read */
void set_plan_sidebar_folder(GtkWidget *plan, const char *folder, const char *const *open_rows);
/* Keys of the rows open in the plan */
GStrv list_plan_open_rows(GtkWidget *plan);
/* Reads a saved file again for its plan or tagged comments */
void refresh_plan_sidebar_file(GtkWidget *plan, GFile *file);
