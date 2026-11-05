#include "common/text/long-lines.h"
#include "source/line-numbers.h"
#include "editor-gutter.h"
#include "app/settings.h"

/* Line numbers keep room for three digits and only grow past that */
#define MINIMUM_DIGITS 3
#define NUMBER_SOURCE_KEY "number-source"
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

/* What the line numbers ask the editor, kept beside the numbers for as long as they live */
typedef struct {
	LineSeverityFunction get_severity;
	gpointer data;
} NumberSource;


/* Vim mode with relative numbers counts every other line from the cursor's */
static int get_line_number(int file_line, int cursor_line, gpointer data) {
	gboolean is_relative = get_switch_setting("relative-numbers") && get_switch_setting("vim-mode")
		&& file_line != cursor_line;
	return is_relative ? ABS(file_line - cursor_line) : file_line + 1;
}


static const char *get_line_colour(int line, gpointer data) {
	NumberSource *source = data;
	int severity = source->get_severity ? source->get_severity(source->data, line) : 0;
	return severity > 0 && severity < (int)G_N_ELEMENTS(SEVERITY_COLOURS) ? SEVERITY_COLOURS[severity] : NULL;
}


GtkSourceGutterRenderer *add_line_numbers(GtkSourceView *view, LineSeverityFunction get_severity, gpointer data) {
	NumberSource *source = g_new0(NumberSource, 1);
	*source = (NumberSource){get_severity, data};
	LineNumberStyle style = {get_line_number, get_line_colour, source};
	GtkSourceGutterRenderer *numbers = add_file_line_numbers(view, NUMBERS_POSITION, MINIMUM_DIGITS, &style);
	g_object_set_data_full(G_OBJECT(numbers), NUMBER_SOURCE_KEY, source, g_free);
	return numbers;
}


void disconnect_line_numbers(GtkSourceGutterRenderer *renderer) {
	NumberSource *source = g_object_get_data(G_OBJECT(renderer), NUMBER_SOURCE_KEY);
	*source = (NumberSource){NULL, NULL};
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
