#include <string.h>

#include "lang/en.h"
#include "problems-panel.h"

#define ROW_SPACING 10
#define MESSAGE_LINES_SPACING 2
/* The source and place columns keep these widths in characters, so most rows line up without measuring every row */
#define SOURCE_COLUMN_CHARACTERS 6
#define PLACE_COLUMN_CHARACTERS 18
/* Folders before a file name in a message, such as /home/me/project/src/ before main.c */
#define FOLDERS_PATTERN "(?:/[^\\s/:]+)+/(?=[^\\s/:]+)"

/* The list and where chosen problems go */
typedef struct {
	GtkWidget *list;
	ProblemChosenFunction on_chosen;
	gpointer data;
} ProblemsPanel;

/* One listed problem, kept on its row since the editor's own list changes whenever the server reports again */
typedef struct {
	GFile *file;
	int line, column;
	DiagnosticSeverity severity;
} ProblemSpot;

static const char *const SEVERITY_ICONS[] = {
	[DIAGNOSTIC_ERROR] = "dialog-error-symbolic",
	[DIAGNOSTIC_WARNING] = "dialog-warning-symbolic",
	[DIAGNOSTIC_INFORMATION] = "dialog-information-symbolic",
	[DIAGNOSTIC_HINT] = "dialog-information-symbolic",
};

static const char *const SEVERITY_STYLES[] = {
	[DIAGNOSTIC_ERROR] = "error",
	[DIAGNOSTIC_WARNING] = "warning",
	[DIAGNOSTIC_INFORMATION] = "accent",
	[DIAGNOSTIC_HINT] = "dim-label",
};


static void free_spot(gpointer data) {
	ProblemSpot *spot = data;
	g_clear_object(&spot->file);
	g_free(spot);
}


static GtkWidget *create_label(const char *text, gboolean is_dim) {
	GtkWidget *label = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_set_valign(label, GTK_ALIGN_START);
	if (is_dim)
		gtk_widget_add_css_class(label, "dim-label");
	return label;
}


static GtkWidget *create_wrapping_label(const char *text, gboolean is_dim) {
	GtkWidget *label = create_label(text, is_dim);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	return label;
}


/* A long path in a note says less than its file name, so the note shows the name and the row keeps the whole message
 * for its tooltip */
static char *shorten_paths(const char *text) {
	static GRegex *folders;
	if (!folders)
		folders = g_regex_new(FOLDERS_PATTERN, G_REGEX_OPTIMIZE, 0, NULL);
	return g_regex_replace_literal(folders, text, -1, 0, "", 0, NULL);
}


/* A message's first line is the problem, and any further lines, such as where a note points, sit below it dimmed
 * without the blank lines between them, and a line ending in a colon runs on into the next */
static GtkWidget *create_message(const char *text) {
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	g_autoptr(GString) details = g_string_new(NULL);
	/* An empty message splits into no lines at all, so the details start only after a first line */
	for (guint index = 1; lines[0] && lines[index]; index++) {
		g_strstrip(lines[index]);
		g_autofree char *line = shorten_paths(lines[index]);
		if (!*line)
			continue;
		gboolean runs_on = details->len && details->str[details->len - 1] == ':';
		g_string_append_printf(details, "%s%s", runs_on ? " " : details->len ? "\n" : "", line);
	}
	GtkWidget *message = gtk_box_new(GTK_ORIENTATION_VERTICAL, MESSAGE_LINES_SPACING);
	gtk_widget_set_hexpand(message, TRUE);
	gtk_box_append(GTK_BOX(message), create_wrapping_label(lines[0] ? lines[0] : "", FALSE));
	if (details->len) {
		GtkWidget *detail = create_wrapping_label(details->str, TRUE);
		gtk_widget_add_css_class(detail, "problem-detail");
		gtk_box_append(GTK_BOX(message), detail);
	}
	return message;
}


static GtkWidget *create_column(const char *text, int characters) {
	GtkWidget *label = create_label(text, TRUE);
	gtk_label_set_width_chars(GTK_LABEL(label), characters);
	return label;
}


/* Long messages wrap rather than being cut off, since the whole message is the point of the panel, and the source
 * and place line up in columns of their own */
static GtkWidget *create_row(Editor *editor, const Diagnostic *problem, gboolean shows_source) {
	gboolean is_known = problem->severity < G_N_ELEMENTS(SEVERITY_ICONS) && SEVERITY_ICONS[problem->severity];
	DiagnosticSeverity severity = is_known ? problem->severity : DIAGNOSTIC_ERROR;
	GtkWidget *image = gtk_image_new_from_icon_name(SEVERITY_ICONS[severity]);
	gtk_widget_add_css_class(image, SEVERITY_STYLES[severity]);
	gtk_widget_set_valign(image, GTK_ALIGN_START);
	g_autofree char *title = get_editor_title(editor);
	g_autofree char *place = g_strdup_printf(TEXT_PROBLEM_PLACE, title, problem->start_line + 1,
		problem->start_column + 1);
	GtkWidget *place_label = create_column(place, PLACE_COLUMN_CHARACTERS);
	gtk_widget_add_css_class(place_label, "problem-place");
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, ROW_SPACING);
	gtk_box_append(GTK_BOX(box), image);
	gtk_box_append(GTK_BOX(box), create_message(problem->message));
	g_autofree char *shown = shorten_paths(problem->message);
	if (strcmp(shown, problem->message) != 0)
		gtk_widget_set_tooltip_text(box, problem->message);
	if (shows_source)
		gtk_box_append(GTK_BOX(box), create_column(problem->source ? problem->source : "", SOURCE_COLUMN_CHARACTERS));
	gtk_box_append(GTK_BOX(box), place_label);
	GtkWidget *row = gtk_list_box_row_new();
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	ProblemSpot *spot = g_new0(ProblemSpot, 1);
	spot->file = g_object_ref(get_editor_file(editor));
	spot->line = problem->start_line;
	spot->column = problem->start_column;
	spot->severity = severity;
	g_object_set_data_full(G_OBJECT(row), "problem", spot, free_spot);
	return row;
}


static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, ProblemsPanel *panel) {
	ProblemSpot *spot = g_object_get_data(G_OBJECT(row), "problem");
	if (spot)
		panel->on_chosen(spot->file, spot->line, spot->column, panel->data);
}


/* Errors come first, then each file's problems in the order they appear in it */
static int compare_rows(GtkListBoxRow *first_row, GtkListBoxRow *second_row, gpointer data) {
	ProblemSpot *first = g_object_get_data(G_OBJECT(first_row), "problem");
	ProblemSpot *second = g_object_get_data(G_OBJECT(second_row), "problem");
	if (first->severity != second->severity)
		return first->severity < second->severity ? -1 : 1;
	g_autofree char *first_path = g_file_get_path(first->file);
	g_autofree char *second_path = g_file_get_path(second->file);
	int order = g_strcmp0(first_path, second_path);
	if (order)
		return order;
	if (first->line != second->line)
		return first->line < second->line ? -1 : 1;
	return first->column - second->column;
}


static void free_panel(gpointer data) {
	g_free(data);
}


GtkWidget *create_problems_panel(ProblemChosenFunction on_chosen, gpointer data) {
	ProblemsPanel *panel = g_new0(ProblemsPanel, 1);
	panel->on_chosen = on_chosen;
	panel->data = data;
	panel->list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(panel->list), GTK_SELECTION_BROWSE);
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(panel->list), TRUE);
	gtk_list_box_set_sort_func(GTK_LIST_BOX(panel->list), compare_rows, NULL, NULL);
	GtkWidget *empty = gtk_label_new(TEXT_NO_OPEN_PROBLEMS);
	gtk_widget_add_css_class(empty, "dim-label");
	gtk_list_box_set_placeholder(GTK_LIST_BOX(panel->list), empty);
	g_signal_connect(panel->list, "row-activated", G_CALLBACK(on_row_activated), panel);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), panel->list);
	gtk_widget_add_css_class(scroller, "problems-panel");
	/* A row cut by the panel's edge gets libadwaita's shade, so the cut reads as more to scroll */
	gtk_widget_add_css_class(scroller, "undershoot-top");
	gtk_widget_add_css_class(scroller, "undershoot-bottom");
	g_object_set_data_full(G_OBJECT(scroller), "problems-panel", panel, free_panel);
	return scroller;
}


/* One source for every problem says nothing on each row, so its column only shows when the sources differ */
static gboolean has_one_source(GPtrArray *editors) {
	const Diagnostic *first = NULL;
	for (guint index = 0; index < editors->len; index++) {
		GPtrArray *problems = get_editor_problems(g_ptr_array_index(editors, index));
		for (guint problem = 0; problem < problems->len; problem++) {
			const Diagnostic *diagnostic = g_ptr_array_index(problems, problem);
			if (!first)
				first = diagnostic;
			else if (g_strcmp0(first->source, diagnostic->source) != 0)
				return FALSE;
		}
	}
	return TRUE;
}


void set_problems_panel_editors(GtkWidget *panel_widget, GPtrArray *editors) {
	ProblemsPanel *panel = g_object_get_data(G_OBJECT(panel_widget), "problems-panel");
	/* Rows go one by one, since removing all children would take the placeholder with them */
	GtkListBoxRow *row;
	while ((row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(panel->list), 0)))
		gtk_list_box_remove(GTK_LIST_BOX(panel->list), GTK_WIDGET(row));
	gboolean shows_source = !has_one_source(editors);
	for (guint index = 0; index < editors->len; index++) {
		Editor *editor = g_ptr_array_index(editors, index);
		gboolean is_listed = FALSE;
		for (guint earlier = 0; earlier < index && !is_listed; earlier++)
			is_listed = is_same_document(editor, g_ptr_array_index(editors, earlier));
		GPtrArray *problems = get_editor_problems(editor);
		if (is_listed || !get_editor_file(editor))
			continue;
		for (guint problem = 0; problem < problems->len; problem++)
			gtk_list_box_append(GTK_LIST_BOX(panel->list), create_row(editor, g_ptr_array_index(problems, problem),
				shows_source));
	}
}


void focus_problems_panel(GtkWidget *panel_widget) {
	ProblemsPanel *panel = g_object_get_data(G_OBJECT(panel_widget), "problems-panel");
	GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(panel->list), 0);
	if (row)
		gtk_widget_grab_focus(GTK_WIDGET(row));
	else
		gtk_widget_grab_focus(panel->list);
}
