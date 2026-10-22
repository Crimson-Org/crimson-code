#pragma once

#include <gtksourceview/gtksource.h>

/* The search strip above the editors, which searches whichever file is in front */
typedef struct FindBar FindBar;

/* Something other than a text view that the bar can search, such as the hex view */
typedef struct {
	/* Selects the next or previous match from the cursor, or the one at the cursor when includes_current is set */
	gboolean (*find)(gpointer data, const char *query, gboolean is_backward, gboolean includes_current);
	/* Fills in how many matches there are and which one is selected, or 0 when none is */
	void (*count)(gpointer data, const char *query, int *position, int *total);
	/* Replaces the selected match, or all matches, returning how many were replaced or -1 with a message */
	int (*replace)(gpointer data, const char *query, const char *replacement, gboolean is_all, char **message);
	/* Marks every match, or none when query is NULL */
	void (*highlight)(gpointer data, const char *query);
	void (*focus)(gpointer data);
	/* What the search entry suggests typing */
	const char *placeholder;
} FindTarget;

FindBar *create_find_bar(void);
GtkWidget *get_find_bar_widget(FindBar *bar);
/* Points the bar at the file in front, or at nothing when no file is open */
void set_find_bar_view(FindBar *bar, GtkSourceView *view);
/* Points the bar at something that is not a text view, which it searches through the target's functions */
void set_find_bar_target(FindBar *bar, const FindTarget *target, gpointer data);
/* Opens the bar with the selected text as the search, ready for typing */
void show_find_bar(FindBar *bar);
/* Opens the bar with its replace row showing */
void show_replace_bar(FindBar *bar);
