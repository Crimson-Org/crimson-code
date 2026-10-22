#include <gtksourceview/gtksource.h>

#include "../lang/en.h"
#include "editor-guides.h"
#include "settings.h"

#define GUIDE_OPACITY 0.22
#define GUIDE_WIDTH_PIXELS 1
#define MARGIN_OPACITY 0.14
#define MARGIN_WIDTH_PIXELS 2
#define SPACE_DOT_PIXELS 2
#define SPACE_OPACITY 0.4
/* How far a blank line looks for code above and below it, to carry the guides through gaps between blocks */
#define BLANK_SEARCH_LINES 200

static const SettingDefinition GUIDE_SETTINGS[] = {
	{"show-indent-guides", "editor", TEXT_GROUP_DISPLAY, TEXT_SETTING_INDENT_GUIDES, NULL, SETTING_SWITCH, "true", 0,
		0, NULL, NULL, {NULL}},
};


void register_guide_settings(void) {
	register_settings(GUIDE_SETTINGS, G_N_ELEMENTS(GUIDE_SETTINGS));
}


/* The indentation of a line in columns, or -1 when the line holds only spaces */
static int measure_indent(GtkTextBuffer *buffer, int line, int tab_width) {
	GtkTextIter iter;
	gtk_text_buffer_get_iter_at_line(buffer, &iter, line);
	int columns = 0;
	for (; !gtk_text_iter_ends_line(&iter); gtk_text_iter_forward_char(&iter)) {
		gunichar character = gtk_text_iter_get_char(&iter);
		if (character == '\t')
			columns += tab_width - columns % tab_width;
		else if (character == ' ')
			columns++;
		else
			return columns;
	}
	return -1;
}


/* A blank line inside a block keeps the block's guides, taking the smaller indentation of the code around it */
static int measure_blank_indent(GtkTextBuffer *buffer, int line, int tab_width) {
	int line_count = gtk_text_buffer_get_line_count(buffer), above = -1, below = -1;
	for (int other = line - 1; other >= 0 && other >= line - BLANK_SEARCH_LINES && above < 0; other--)
		above = measure_indent(buffer, other, tab_width);
	for (int other = line + 1; other < line_count && other <= line + BLANK_SEARCH_LINES && below < 0; other++)
		below = measure_indent(buffer, other, tab_width);
	return MAX(0, MIN(above, below));
}


static double measure_space_width(GtkTextView *view) {
	PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(view), " ");
	int width = 0;
	pango_layout_get_size(layout, &width, NULL);
	g_object_unref(layout);
	return (double)width / PANGO_SCALE;
}


static void draw_margin_lines(GtkTextView *view, GtkSnapshot *snapshot) {
	if (!get_switch_setting("show-margin"))
		return;
	GdkRGBA colour;
	gtk_widget_get_color(GTK_WIDGET(view), &colour);
	colour.alpha *= MARGIN_OPACITY;
	GdkRectangle visible;
	gtk_text_view_get_visible_rect(view, &visible);
	double space_width = measure_space_width(view);
	const char *const keys[] = {"margin-column", "second-margin-column"};
	for (guint index = 0; index < G_N_ELEMENTS(keys); index++) {
		int column = get_number_setting(keys[index]);
		if (column <= 0)
			continue;
		double x = gtk_text_view_get_left_margin(view) + column * space_width;
		gtk_snapshot_append_color(snapshot, &colour, &GRAPHENE_RECT_INIT((float)x, (float)visible.y,
			MARGIN_WIDTH_PIXELS, (float)visible.height));
	}
}


void draw_indent_guides(GtkTextView *view, GtkSnapshot *snapshot) {
	draw_margin_lines(view, snapshot);
	if (!get_switch_setting("show-indent-guides"))
		return;
	GtkSourceView *source = GTK_SOURCE_VIEW(view);
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
	int tab_width = MAX(1, (int)gtk_source_view_get_tab_width(source));
	int step = gtk_source_view_get_indent_width(source);
	step = step > 0 ? step : tab_width;
	double space_width = measure_space_width(view);
	GdkRGBA colour;
	gtk_widget_get_color(GTK_WIDGET(view), &colour);
	colour.alpha *= GUIDE_OPACITY;
	GdkRectangle visible;
	gtk_text_view_get_visible_rect(view, &visible);
	GtkTextIter iter;
	gtk_text_view_get_line_at_y(view, &iter, visible.y, NULL);
	while (TRUE) {
		int top = 0, height = 0;
		gtk_text_view_get_line_yrange(view, &iter, &top, &height);
		if (top > visible.y + visible.height)
			break;
		int line = gtk_text_iter_get_line(&iter);
		int indent = measure_indent(buffer, line, tab_width);
		if (indent < 0)
			indent = measure_blank_indent(buffer, line, tab_width);
		GdkRectangle start;
		gtk_text_view_get_iter_location(view, &iter, &start);
		for (int column = 0; column < indent; column += step) {
			double x = start.x + column * space_width;
			gtk_snapshot_append_color(snapshot, &colour, &GRAPHENE_RECT_INIT((float)x, (float)top,
				GUIDE_WIDTH_PIXELS, (float)height));
		}
		if (!gtk_text_iter_forward_line(&iter))
			break;
	}
}


/* Uses the scheme's own colour for drawn spaces, so the runs look like the spaces the source view draws */
static void get_space_colour(GtkTextView *view, GdkRGBA *colour) {
	gtk_widget_get_color(GTK_WIDGET(view), colour);
	colour->alpha *= SPACE_OPACITY;
	GtkSourceStyleScheme *scheme = gtk_source_buffer_get_style_scheme(GTK_SOURCE_BUFFER(
		gtk_text_view_get_buffer(view)));
	GtkSourceStyle *style = scheme ? gtk_source_style_scheme_get_style(scheme, "draw-spaces") : NULL;
	if (!style)
		return;
	g_autofree char *value = NULL;
	gboolean is_set = FALSE;
	g_object_get(style, "foreground", &value, "foreground-set", &is_set, NULL);
	if (is_set && value)
		gdk_rgba_parse(colour, value);
}


static void draw_dot(GtkTextView *view, GtkSnapshot *snapshot, const GtkTextIter *iter, const GdkRGBA *colour) {
	GdkRectangle cell;
	gtk_text_view_get_iter_location(view, iter, &cell);
	float x = cell.x + (cell.width - SPACE_DOT_PIXELS) / 2.0f, y = cell.y + (cell.height - SPACE_DOT_PIXELS) / 2.0f;
	gtk_snapshot_append_color(snapshot, colour, &GRAPHENE_RECT_INIT(x, y, SPACE_DOT_PIXELS, SPACE_DOT_PIXELS));
}


/* Indentation and line ends are left to the source view, which already draws them */
static void draw_line_runs(GtkTextView *view, GtkSnapshot *snapshot, GtkTextIter line, const GdkRGBA *colour) {
	while (!gtk_text_iter_ends_line(&line) && g_unichar_isspace(gtk_text_iter_get_char(&line)))
		gtk_text_iter_forward_char(&line);
	while (!gtk_text_iter_ends_line(&line)) {
		if (gtk_text_iter_get_char(&line) != ' ') {
			gtk_text_iter_forward_char(&line);
			continue;
		}
		GtkTextIter end = line;
		int length = 0;
		while (!gtk_text_iter_ends_line(&end) && gtk_text_iter_get_char(&end) == ' ') {
			gtk_text_iter_forward_char(&end);
			length++;
		}
		if (length > 1 && !gtk_text_iter_ends_line(&end))
			for (GtkTextIter dot = line; !gtk_text_iter_equal(&dot, &end); gtk_text_iter_forward_char(&dot))
				draw_dot(view, snapshot, &dot, colour);
		line = end;
	}
}


void draw_space_runs(GtkTextView *view, GtkSnapshot *snapshot) {
	if (g_strcmp0(get_text_setting("draw-spaces"), "all") != 0)
		return;
	GdkRGBA colour;
	get_space_colour(view, &colour);
	GdkRectangle visible;
	gtk_text_view_get_visible_rect(view, &visible);
	GtkTextIter iter;
	gtk_text_view_get_line_at_y(view, &iter, visible.y, NULL);
	while (TRUE) {
		int top = 0, height = 0;
		gtk_text_view_get_line_yrange(view, &iter, &top, &height);
		if (top > visible.y + visible.height)
			break;
		draw_line_runs(view, snapshot, iter, &colour);
		if (!gtk_text_iter_forward_line(&iter))
			break;
	}
}
