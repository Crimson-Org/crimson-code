#include <string.h>

#include "../lang/en.h"
#include "common/long-lines.h"
#include "editor-colours.h"
#include "editor-cursors.h"
#include "editor-guides.h"
#include "editor.h"
#include "settings.h"

#define CODE_TAG "colour-code"
#define SPACE_TAG "colour-space"
#define LINE_START_TAG "colour-line-start"
#define SCAN_KEY "colour-scan"
/* The lines still to scan lie between these two marks, which follow edits made in the meantime */
#define DIRTY_START_MARK "colour-dirty-start"
#define DIRTY_END_MARK "colour-dirty-end"
#define SCAN_DELAY_MILLISECONDS 300
/* Each pass scans lines for at most this long, so a huge file is marked over many frames */
#define SCAN_SLICE_MICROSECONDS 4000
#define SCAN_SLICE_LINES 200
/* The circle is this share of the text size, with this much space between it and the code */
#define SWATCH_SCALE 0.7
#define SWATCH_GAP_PIXELS 3
/* Hex colours of 3, 4, 6 or 8 digits that are not part of a longer word */
#define COLOUR_PATTERN "(?<![\\w#&])#(?:[0-9a-fA-F]{8}|[0-9a-fA-F]{6}|[0-9a-fA-F]{3,4})(?!\\w)"

static const SettingDefinition COLOUR_SETTINGS[] = {
	{"show-colours", "editor", TEXT_GROUP_DISPLAY, TEXT_SETTING_COLOURS, TEXT_SETTING_COLOURS_DETAIL, SETTING_SWITCH,
		"true", 0, 0, NULL, NULL, {NULL}},
};

/* A source view that draws colour circles above its text */
struct _CodeView {
	GtkSourceView parent_instance;
};

#define CODE_VIEW_TYPE (code_view_get_type())
G_DECLARE_FINAL_TYPE(CodeView, code_view, CRIMSON, CODE_VIEW, GtkSourceView)
G_DEFINE_FINAL_TYPE(CodeView, code_view, GTK_SOURCE_TYPE_VIEW)

static GRegex *colour_pattern;


static double get_swatch_pixels(void) {
	return get_number_setting("text-size") * SWATCH_SCALE;
}


/* ==================================== Marking codes ==================================== */


/* Letter spacing on the character before a code splits around that character, so twice the room is asked for */
static int get_space_units(void) {
	return (int)((get_swatch_pixels() + SWATCH_GAP_PIXELS) * 2 * PANGO_SCALE);
}


static void ensure_tags(GtkTextBuffer *buffer) {
	GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
	if (!gtk_text_tag_table_lookup(table, CODE_TAG))
		gtk_text_buffer_create_tag(buffer, CODE_TAG, NULL);
	GtkTextTag *space = gtk_text_tag_table_lookup(table, SPACE_TAG);
	if (!space)
		space = gtk_text_buffer_create_tag(buffer, SPACE_TAG, NULL);
	g_object_set(space, "letter-spacing", get_space_units(), NULL);
	GtkTextTag *line_start = gtk_text_tag_table_lookup(table, LINE_START_TAG);
	if (!line_start)
		line_start = gtk_text_buffer_create_tag(buffer, LINE_START_TAG, NULL);
	g_object_set(line_start, "left-margin", EDITOR_LEFT_MARGIN_PIXELS + (int)get_swatch_pixels() + SWATCH_GAP_PIXELS,
		NULL);
}


static void mark_code(GtkTextBuffer *buffer, int start_offset, int end_offset) {
	GtkTextIter start, end, before;
	gtk_text_buffer_get_iter_at_offset(buffer, &start, start_offset);
	gtk_text_buffer_get_iter_at_offset(buffer, &end, end_offset);
	gtk_text_buffer_apply_tag_by_name(buffer, CODE_TAG, &start, &end);
	before = start;
	/* With no character before it, the line itself moves in to make room in its margin */
	if (gtk_text_iter_starts_line(&start)) {
		gtk_text_buffer_apply_tag_by_name(buffer, LINE_START_TAG, &start, &end);
		return;
	}
	gtk_text_iter_backward_char(&before);
	gtk_text_buffer_apply_tag_by_name(buffer, SPACE_TAG, &before, &start);
}


/* Marks the codes between two line starts, with offsets counted forward from one match to the next */
static void scan_lines(GtkTextBuffer *buffer, GtkTextIter *start, GtkTextIter *end) {
	gtk_text_buffer_remove_tag_by_name(buffer, CODE_TAG, start, end);
	gtk_text_buffer_remove_tag_by_name(buffer, SPACE_TAG, start, end);
	gtk_text_buffer_remove_tag_by_name(buffer, LINE_START_TAG, start, end);
	if (!get_switch_setting("show-colours"))
		return;
	if (!colour_pattern)
		colour_pattern = g_regex_new(COLOUR_PATTERN, G_REGEX_OPTIMIZE, 0, NULL);
	int base = gtk_text_iter_get_offset(start);
	g_autofree char *text = gtk_text_buffer_get_text(buffer, start, end, FALSE);
	g_autoptr(GMatchInfo) match = NULL;
	g_regex_match(colour_pattern, text, 0, &match);
	const char *counted = text;
	int offset = 0;
	for (; g_match_info_matches(match); g_match_info_next(match, NULL)) {
		int start_byte = 0, end_byte = 0;
		g_match_info_fetch_pos(match, 0, &start_byte, &end_byte);
		offset += (int)g_utf8_pointer_to_offset(counted, text + start_byte);
		counted = text + start_byte;
		int length = (int)g_utf8_pointer_to_offset(counted, text + end_byte);
		mark_code(buffer, base + offset, base + offset + length);
	}
}


static void stop_scan(GtkTextBuffer *buffer) {
	guint source = GPOINTER_TO_UINT(g_object_steal_data(G_OBJECT(buffer), SCAN_KEY));
	if (source)
		g_source_remove(source);
}


/* Scans the waiting lines a slice at a time and comes back for the rest, so the window keeps drawing */
static gboolean run_scan(gpointer data) {
	GtkTextBuffer *buffer = data;
	GtkTextIter start, end, stop;
	gtk_text_buffer_get_iter_at_mark(buffer, &start, gtk_text_buffer_get_mark(buffer, DIRTY_START_MARK));
	gtk_text_buffer_get_iter_at_mark(buffer, &end, gtk_text_buffer_get_mark(buffer, DIRTY_END_MARK));
	gtk_text_iter_set_line_offset(&start, 0);
	if (!gtk_text_iter_ends_line(&end))
		gtk_text_iter_forward_to_line_end(&end);
	gint64 deadline = g_get_monotonic_time() + SCAN_SLICE_MICROSECONDS;
	while (gtk_text_iter_compare(&start, &end) < 0 && g_get_monotonic_time() < deadline) {
		stop = start;
		gtk_text_iter_forward_lines(&stop, SCAN_SLICE_LINES);
		if (gtk_text_iter_compare(&stop, &end) > 0)
			stop = end;
		scan_lines(buffer, &start, &stop);
		start = stop;
	}
	gtk_text_buffer_move_mark_by_name(buffer, DIRTY_START_MARK, &start);
	guint next = gtk_text_iter_compare(&start, &end) < 0 ? g_idle_add_full(G_PRIORITY_LOW, run_scan, buffer, NULL) : 0;
	g_object_set_data(G_OBJECT(buffer), SCAN_KEY, GUINT_TO_POINTER(next));
	return G_SOURCE_REMOVE;
}


/* Widens the waiting lines to take in this range, and scans them once the edits pause */
static void queue_scan(GtkTextBuffer *buffer, const GtkTextIter *changed_start, const GtkTextIter *changed_end) {
	GtkTextMark *start_mark = gtk_text_buffer_get_mark(buffer, DIRTY_START_MARK);
	GtkTextMark *end_mark = gtk_text_buffer_get_mark(buffer, DIRTY_END_MARK);
	if (!start_mark) {
		start_mark = gtk_text_buffer_create_mark(buffer, DIRTY_START_MARK, changed_start, TRUE);
		end_mark = gtk_text_buffer_create_mark(buffer, DIRTY_END_MARK, changed_end, FALSE);
	} else if (!g_object_get_data(G_OBJECT(buffer), SCAN_KEY)) {
		gtk_text_buffer_move_mark(buffer, start_mark, changed_start);
		gtk_text_buffer_move_mark(buffer, end_mark, changed_end);
	} else {
		GtkTextIter start, end;
		gtk_text_buffer_get_iter_at_mark(buffer, &start, start_mark);
		gtk_text_buffer_get_iter_at_mark(buffer, &end, end_mark);
		if (gtk_text_iter_compare(changed_start, &start) < 0)
			gtk_text_buffer_move_mark(buffer, start_mark, changed_start);
		if (gtk_text_iter_compare(changed_end, &end) > 0)
			gtk_text_buffer_move_mark(buffer, end_mark, changed_end);
	}
	stop_scan(buffer);
	guint source = g_timeout_add_full(G_PRIORITY_LOW, SCAN_DELAY_MILLISECONDS, run_scan, buffer, NULL);
	g_object_set_data(G_OBJECT(buffer), SCAN_KEY, GUINT_TO_POINTER(source));
}


static void queue_whole_scan(GtkTextBuffer *buffer) {
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(buffer, &start, &end);
	ensure_tags(buffer);
	queue_scan(buffer, &start, &end);
}


static void on_setting_changed(const char *key, gpointer data) {
	if (g_strcmp0(key, "show-colours") == 0 || g_strcmp0(key, "text-size") == 0)
		queue_whole_scan(data);
}


static void on_text_inserted(GtkTextBuffer *buffer, GtkTextIter *end, char *text, int length, gpointer data) {
	GtkTextIter start = *end;
	gtk_text_iter_backward_chars(&start, (int)g_utf8_strlen(text, length));
	queue_scan(buffer, &start, end);
}


static void on_range_deleted(GtkTextBuffer *buffer, GtkTextIter *start, GtkTextIter *end, gpointer data) {
	queue_scan(buffer, start, end);
}


void watch_colour_codes(GtkSourceBuffer *buffer) {
	/* The settings table doubles as a marker, so these handlers can be found again without comparing functions */
	g_signal_connect_after(buffer, "insert-text", G_CALLBACK(on_text_inserted), (gpointer)COLOUR_SETTINGS);
	g_signal_connect_after(buffer, "delete-range", G_CALLBACK(on_range_deleted), (gpointer)COLOUR_SETTINGS);
	watch_settings(on_setting_changed, buffer);
	queue_whole_scan(GTK_TEXT_BUFFER(buffer));
}


void unwatch_colour_codes(GtkSourceBuffer *buffer) {
	g_signal_handlers_disconnect_by_data(buffer, (gpointer)COLOUR_SETTINGS);
	unwatch_settings(buffer);
	stop_scan(GTK_TEXT_BUFFER(buffer));
}


/* ==================================== Drawing ==================================== */


static void draw_swatch(GtkSnapshot *snapshot, const GdkRectangle *code, const GdkRGBA *colour, GdkRGBA outline) {
	double size = get_swatch_pixels();
	graphene_rect_t area = GRAPHENE_RECT_INIT(code->x - size - SWATCH_GAP_PIXELS,
		code->y + (code->height - size) / 2, size, size);
	GskRoundedRect circle;
	gsk_rounded_rect_init_from_rect(&circle, &area, (float)size / 2);
	gtk_snapshot_push_rounded_clip(snapshot, &circle);
	gtk_snapshot_append_color(snapshot, colour, &area);
	gtk_snapshot_pop(snapshot);
	outline.alpha = 0.35f;
	const float widths[4] = {1, 1, 1, 1};
	const GdkRGBA outlines[4] = {outline, outline, outline, outline};
	gtk_snapshot_append_border(snapshot, &circle, widths, outlines);
}


/* Draws circles for the codes in view only, reading each code's colour from the text itself */
static void code_view_snapshot_layer(GtkTextView *view, GtkTextViewLayer layer, GtkSnapshot *snapshot) {
	GTK_TEXT_VIEW_CLASS(code_view_parent_class)->snapshot_layer(view, layer, snapshot);
	if (layer == GTK_TEXT_VIEW_LAYER_BELOW_TEXT)
		draw_indent_guides(view, snapshot);
	if (layer != GTK_TEXT_VIEW_LAYER_ABOVE_TEXT)
		return;
	draw_space_runs(view, snapshot);
	draw_extra_cursors(view, snapshot);
	if (!get_switch_setting("show-colours"))
		return;
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
	GtkTextTag *tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), CODE_TAG);
	if (!tag)
		return;
	GdkRectangle visible;
	gtk_text_view_get_visible_rect(view, &visible);
	GtkTextIter iter, last;
	gtk_text_view_get_line_at_y(view, &iter, visible.y, NULL);
	gtk_text_view_get_line_at_y(view, &last, visible.y + visible.height, NULL);
	gtk_text_iter_forward_to_line_end(&last);
	GdkRGBA text_colour;
	gtk_widget_get_color(GTK_WIDGET(view), &text_colour);
	while (gtk_text_iter_compare(&iter, &last) < 0) {
		if (!gtk_text_iter_starts_tag(&iter, tag) && !gtk_text_iter_forward_to_tag_toggle(&iter, tag))
			break;
		if (!gtk_text_iter_starts_tag(&iter, tag) || gtk_text_iter_compare(&iter, &last) >= 0)
			continue;
		GtkTextIter end = iter;
		gtk_text_iter_forward_to_tag_toggle(&end, tag);
		g_autofree char *code = gtk_text_iter_get_text(&iter, &end);
		GdkRGBA colour;
		GdkRectangle location;
		gtk_text_view_get_iter_location(view, &iter, &location);
		if (gdk_rgba_parse(&colour, code))
			draw_swatch(snapshot, &location, &colour, text_colour);
		iter = end;
	}
}


static void code_view_class_init(CodeViewClass *class) {
	GTK_TEXT_VIEW_CLASS(class)->snapshot_layer = code_view_snapshot_layer;
}


static void code_view_init(CodeView *view) {
}


GtkSourceView *create_code_view(GtkSourceBuffer *buffer) {
	GtkSourceView *view = g_object_new(CODE_VIEW_TYPE, "buffer", buffer, NULL);
	join_long_line_copies(GTK_TEXT_VIEW(view));
	return view;
}


void register_colour_settings(void) {
	register_settings(COLOUR_SETTINGS, G_N_ELEMENTS(COLOUR_SETTINGS));
}
