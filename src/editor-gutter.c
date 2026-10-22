#include "common/long-lines.h"
#include "editor-gutter.h"
#include "settings.h"

#define MINIMUM_NUMBER 999
/* Right of the problem icons, which GtkSourceView keeps at GTK_SOURCE_VIEW_GUTTER_POSITION_MARKS */
#define NUMBERS_POSITION 0
#define ICONS_POSITION -40
#define ICON_PIXELS 16
#define ICON_PADDING 4

/* Colours and icons for problem severities, readable on both light and dark backgrounds */
static const char *const SEVERITY_COLOURS[] = {NULL, "#e01b24", "#e5a50a", "#3584e4", "#9a9996"};
static const char *const SEVERITY_ICONS[] = {
	NULL, "dialog-error-symbolic", "dialog-warning-symbolic", "dialog-information-symbolic",
	"dialog-information-symbolic",
};

/* Line numbers drawn by the text renderer, which already shows the current line in the scheme's style */
struct _LineNumbers {
	GtkSourceGutterRendererText parent_instance;
	LineSeverityFunction get_severity;
	gpointer data;
	int digits;
};

#define LINE_NUMBERS_TYPE (line_numbers_get_type())
G_DECLARE_FINAL_TYPE(LineNumbers, line_numbers, CRIMSON, LINE_NUMBERS, GtkSourceGutterRendererText)
G_DEFINE_FINAL_TYPE(LineNumbers, line_numbers, GTK_SOURCE_TYPE_GUTTER_RENDERER_TEXT)


static int get_cursor_line(GtkTextBuffer *buffer) {
	GtkTextIter cursor;
	gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
	return get_joined_line(buffer, gtk_text_iter_get_line(&cursor));
}


static void line_numbers_query_data(GtkSourceGutterRenderer *renderer, GtkSourceGutterLines *lines, guint line) {
	LineNumbers *numbers = CRIMSON_LINE_NUMBERS(renderer);
	GtkSourceGutterRendererText *text = GTK_SOURCE_GUTTER_RENDERER_TEXT(renderer);
	GtkTextBuffer *buffer = GTK_TEXT_BUFFER(gtk_source_gutter_renderer_get_buffer(renderer));
	/* The later pieces of a split line show no number, and the rest count the file's own lines */
	int file_line = (int)line;
	if (has_split_lines(buffer)) {
		GtkTextIter iter;
		gtk_text_buffer_get_iter_at_line(buffer, &iter, (int)line);
		if (continues_split_line(&iter)) {
			gtk_source_gutter_renderer_text_set_text(text, "", -1);
			return;
		}
		file_line = get_joined_line(buffer, (int)line);
	}
	int cursor_line = get_cursor_line(buffer);
	gboolean is_relative = get_switch_setting("relative-numbers") && get_switch_setting("vim-mode")
		&& file_line != cursor_line;
	int number = is_relative ? ABS(file_line - cursor_line) : file_line + 1;
	int severity = numbers->get_severity ? numbers->get_severity(numbers->data, (int)line) : 0;
	char label[64];
	if (severity <= 0 || severity >= (int)G_N_ELEMENTS(SEVERITY_COLOURS)) {
		g_snprintf(label, sizeof label, "%d", number);
		gtk_source_gutter_renderer_text_set_text(text, label, -1);
		return;
	}
	g_snprintf(label, sizeof label, "<span foreground=\"%s\">%d</span>", SEVERITY_COLOURS[severity], number);
	gtk_source_gutter_renderer_text_set_markup(text, label, -1);
}


static int count_digits(LineNumbers *numbers) {
	GtkSourceBuffer *buffer = gtk_source_gutter_renderer_get_buffer(GTK_SOURCE_GUTTER_RENDERER(numbers));
	int count = buffer ? MAX(gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(buffer)), MINIMUM_NUMBER)
		: MINIMUM_NUMBER;
	int digits = 0;
	for (; count > 0; count /= 10)
		digits++;
	return digits;
}


static void line_numbers_measure(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum,
	int *natural, int *minimum_baseline, int *natural_baseline) {
	*minimum_baseline = *natural_baseline = -1;
	if (orientation == GTK_ORIENTATION_VERTICAL) {
		*minimum = *natural = 0;
		return;
	}
	LineNumbers *numbers = CRIMSON_LINE_NUMBERS(widget);
	numbers->digits = count_digits(numbers);
	g_autofree char *sample = g_strnfill(numbers->digits, '0');
	int width = 0;
	gtk_source_gutter_renderer_text_measure(GTK_SOURCE_GUTTER_RENDERER_TEXT(widget), sample, &width, NULL);
	*minimum = *natural = width + 2 * gtk_source_gutter_renderer_get_xpad(GTK_SOURCE_GUTTER_RENDERER(widget));
}


/* Asks for a new width only when the line count gains or loses a digit, so typing does not relayout */
static void on_buffer_changed(GtkTextBuffer *buffer, LineNumbers *numbers) {
	if (count_digits(numbers) != numbers->digits)
		gtk_widget_queue_resize(GTK_WIDGET(numbers));
}


static void on_cursor_moved(GtkSourceBuffer *buffer, LineNumbers *numbers) {
	gtk_widget_queue_draw(GTK_WIDGET(numbers));
}


static void line_numbers_change_buffer(GtkSourceGutterRenderer *renderer, GtkSourceBuffer *old_buffer) {
	if (old_buffer)
		g_signal_handlers_disconnect_by_data(old_buffer, renderer);
	GtkSourceBuffer *buffer = gtk_source_gutter_renderer_get_buffer(renderer);
	if (buffer) {
		g_signal_connect_object(buffer, "changed", G_CALLBACK(on_buffer_changed), renderer, 0);
		g_signal_connect_object(buffer, "cursor-moved", G_CALLBACK(on_cursor_moved), renderer, 0);
	}
	GTK_SOURCE_GUTTER_RENDERER_CLASS(line_numbers_parent_class)->change_buffer(renderer, old_buffer);
	gtk_widget_queue_resize(GTK_WIDGET(renderer));
}


static void line_numbers_class_init(LineNumbersClass *class) {
	GTK_WIDGET_CLASS(class)->measure = line_numbers_measure;
	GTK_SOURCE_GUTTER_RENDERER_CLASS(class)->query_data = line_numbers_query_data;
	GTK_SOURCE_GUTTER_RENDERER_CLASS(class)->change_buffer = line_numbers_change_buffer;
}


static void line_numbers_init(LineNumbers *numbers) {
	gtk_source_gutter_renderer_set_xalign(GTK_SOURCE_GUTTER_RENDERER(numbers), 1.0);
	gtk_source_gutter_renderer_set_xpad(GTK_SOURCE_GUTTER_RENDERER(numbers), 6);
}


GtkSourceGutterRenderer *add_line_numbers(GtkSourceView *view, LineSeverityFunction get_severity, gpointer data) {
	LineNumbers *numbers = g_object_new(LINE_NUMBERS_TYPE, NULL);
	numbers->get_severity = get_severity;
	numbers->data = data;
	GtkSourceGutter *gutter = gtk_source_view_get_gutter(view, GTK_TEXT_WINDOW_LEFT);
	gtk_source_gutter_insert(gutter, GTK_SOURCE_GUTTER_RENDERER(numbers), NUMBERS_POSITION);
	return GTK_SOURCE_GUTTER_RENDERER(numbers);
}


void disconnect_line_numbers(GtkSourceGutterRenderer *renderer) {
	CRIMSON_LINE_NUMBERS(renderer)->get_severity = NULL;
	CRIMSON_LINE_NUMBERS(renderer)->data = NULL;
}


void refresh_line_numbers(GtkSourceGutterRenderer *renderer) {
	gtk_widget_queue_draw(GTK_WIDGET(renderer));
}


/* ==================================== Problem icons ==================================== */


/* Draws the worst problem of each line as a symbolic icon recoloured to its severity */
struct _ProblemIcons {
	GtkSourceGutterRenderer parent_instance;
	LineSeverityFunction get_severity;
	ProblemIconActivatedFunction on_activated;
	gpointer data;
};

#define PROBLEM_ICONS_TYPE (problem_icons_get_type())
G_DECLARE_FINAL_TYPE(ProblemIcons, problem_icons, CRIMSON, PROBLEM_ICONS, GtkSourceGutterRenderer)
G_DEFINE_FINAL_TYPE(ProblemIcons, problem_icons, GTK_SOURCE_TYPE_GUTTER_RENDERER)


static int get_icon_severity(ProblemIcons *icons, int line) {
	int severity = icons->get_severity ? icons->get_severity(icons->data, line) : 0;
	return severity > 0 && severity < (int)G_N_ELEMENTS(SEVERITY_ICONS) ? severity : 0;
}


static void problem_icons_snapshot_line(GtkSourceGutterRenderer *renderer, GtkSnapshot *snapshot,
	GtkSourceGutterLines *lines, guint line) {
	ProblemIcons *icons = CRIMSON_PROBLEM_ICONS(renderer);
	int severity = get_icon_severity(icons, (int)line);
	if (!severity)
		return;
	GtkWidget *widget = GTK_WIDGET(renderer);
	g_autoptr(GtkIconPaintable) icon = gtk_icon_theme_lookup_icon(gtk_icon_theme_get_for_display(
		gtk_widget_get_display(widget)), SEVERITY_ICONS[severity], NULL, ICON_PIXELS,
		gtk_widget_get_scale_factor(widget), gtk_widget_get_direction(widget), 0);
	float x = 0, y = 0;
	gtk_source_gutter_renderer_align_cell(renderer, line, ICON_PIXELS, ICON_PIXELS, &x, &y);
	GdkRGBA colour;
	gdk_rgba_parse(&colour, SEVERITY_COLOURS[severity]);
	gtk_snapshot_save(snapshot);
	gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(x, y));
	gtk_symbolic_paintable_snapshot_symbolic(GTK_SYMBOLIC_PAINTABLE(icon), snapshot, ICON_PIXELS, ICON_PIXELS,
		&colour, 1);
	gtk_snapshot_restore(snapshot);
}


static gboolean problem_icons_query_activatable(GtkSourceGutterRenderer *renderer, GtkTextIter *iter,
	GdkRectangle *area) {
	return get_icon_severity(CRIMSON_PROBLEM_ICONS(renderer), gtk_text_iter_get_line(iter)) > 0;
}


static void problem_icons_activate(GtkSourceGutterRenderer *renderer, GtkTextIter *iter, GdkRectangle *area,
	guint button, GdkModifierType state, int presses) {
	ProblemIcons *icons = CRIMSON_PROBLEM_ICONS(renderer);
	if (icons->on_activated)
		icons->on_activated(icons->data, gtk_text_iter_get_line(iter), area);
}


static void problem_icons_measure(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum,
	int *natural, int *minimum_baseline, int *natural_baseline) {
	*minimum_baseline = *natural_baseline = -1;
	*minimum = *natural = orientation == GTK_ORIENTATION_HORIZONTAL ? ICON_PIXELS + 2 * ICON_PADDING : 0;
}


static void problem_icons_class_init(ProblemIconsClass *class) {
	GTK_WIDGET_CLASS(class)->measure = problem_icons_measure;
	GTK_SOURCE_GUTTER_RENDERER_CLASS(class)->snapshot_line = problem_icons_snapshot_line;
	GTK_SOURCE_GUTTER_RENDERER_CLASS(class)->query_activatable = problem_icons_query_activatable;
	GTK_SOURCE_GUTTER_RENDERER_CLASS(class)->activate = problem_icons_activate;
}


static void problem_icons_init(ProblemIcons *icons) {
	gtk_source_gutter_renderer_set_xalign(GTK_SOURCE_GUTTER_RENDERER(icons), 0.5);
	gtk_source_gutter_renderer_set_yalign(GTK_SOURCE_GUTTER_RENDERER(icons), 0.5);
}


GtkSourceGutterRenderer *add_problem_icons(GtkSourceView *view, LineSeverityFunction get_severity,
	ProblemIconActivatedFunction on_activated, gpointer data) {
	ProblemIcons *icons = g_object_new(PROBLEM_ICONS_TYPE, NULL);
	icons->get_severity = get_severity;
	icons->on_activated = on_activated;
	icons->data = data;
	gtk_source_gutter_insert(gtk_source_view_get_gutter(view, GTK_TEXT_WINDOW_LEFT),
		GTK_SOURCE_GUTTER_RENDERER(icons), ICONS_POSITION);
	return GTK_SOURCE_GUTTER_RENDERER(icons);
}


void disconnect_problem_icons(GtkSourceGutterRenderer *renderer) {
	ProblemIcons *icons = CRIMSON_PROBLEM_ICONS(renderer);
	icons->get_severity = NULL;
	icons->on_activated = NULL;
	icons->data = NULL;
}
