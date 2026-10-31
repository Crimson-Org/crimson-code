#pragma once

#include <gtksourceview/gtksource.h>

/* Gives the most severe problem on a line, or 0 when it has none */
typedef int (*LineSeverityFunction)(gpointer data, int line);

/* Called when a problem icon is clicked, with the line and its gutter area in the view's coordinates */
typedef void (*ProblemIconActivatedFunction)(gpointer data, int line, const GdkRectangle *area);

/* Error and warning icons in their severity colours at the far left of the gutter, which open details on click */
GtkSourceGutterRenderer *add_problem_icons(GtkSourceView *view, LineSeverityFunction get_severity,
	ProblemIconActivatedFunction on_activated, gpointer data);
/* Line numbers that can count from the cursor and take the colour of the most severe problem on their line */
GtkSourceGutterRenderer *add_line_numbers(GtkSourceView *view, LineSeverityFunction get_severity, gpointer data);
/* Stops asking for problems once the editor that answered is gone, for line numbers and problem icons alike */
void disconnect_line_numbers(GtkSourceGutterRenderer *renderer);
void disconnect_problem_icons(GtkSourceGutterRenderer *renderer);
/* Redraws after problems or the numbering setting changed */
void refresh_line_numbers(GtkSourceGutterRenderer *renderer);
