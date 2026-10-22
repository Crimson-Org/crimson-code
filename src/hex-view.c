#include <string.h>

#include <adwaita.h>
#include <gtksourceview/gtksource.h>

#include "../lang/en.h"
#include "hex-view.h"

#define BYTES_PER_ROW 16
#define GROUP_SIZE 8
#define PADDING_PIXELS 12
/* Characters between the offset, hex and text columns */
#define COLUMN_GAP 3
#define SAVE_CHUNK_BYTES 65536
#define SNIFF_BYTES 8192
#define NULL_OPACITY 0.35
#define SELECTION_OPACITY 0.25
#define CURSOR_OPACITY 0.45
#define VIEW_KEY "hex-view"
/* Files larger than this are not counted through on each search, which would take too long */
#define COUNT_LIMIT_BYTES (64 * 1024 * 1024)

/* ==================================== The document ==================================== */


/* One change, where a value of -1 means the byte had no patch and showed the file's own value */
typedef struct {
	gint64 offset;
	int before, after;
	/* Changes made together, such as by Replace All, share a group and undo together */
	guint group;
} HexEdit;

typedef struct {
	HexChangedFunction on_changed;
	gpointer data;
} HexListener;

struct HexDocument {
	GFile *file;
	GMappedFile *mapped;
	const guint8 *bytes;
	gsize length;
	/* Offset to patched byte, keyed by a gint64 pointer */
	GHashTable *patches;
	GArray *undo, *redo;
	/* How deep the undo stack was when last saved, so undoing back to it counts as unmodified */
	guint saved_depth;
	GArray *listeners;
	guint next_group;
};

typedef struct {
	GFile *file;
	HexLoadedFunction on_loaded;
	gpointer data;
} LoadRequest;


gboolean is_binary_start(const guint8 *bytes, gsize length) {
	return memchr(bytes, 0, MIN(length, SNIFF_BYTES)) != NULL;
}


static void free_load_request(gpointer data) {
	LoadRequest *request = data;
	g_object_unref(request->file);
	g_free(request);
}


static void map_in_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	LoadRequest *request = data;
	g_autofree char *path = g_file_get_path(request->file);
	GError *error = NULL;
	GMappedFile *mapped = path ? g_mapped_file_new(path, FALSE, &error) : NULL;
	if (!mapped && !error)
		error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, TEXT_HEX_LOCAL_ONLY);
	if (mapped)
		g_task_return_pointer(task, mapped, (GDestroyNotify)g_mapped_file_unref);
	else
		g_task_return_error(task, error);
}


static void on_mapped(GObject *source, GAsyncResult *result, gpointer data) {
	LoadRequest *request = g_task_get_task_data(G_TASK(result));
	g_autoptr(GError) error = NULL;
	GMappedFile *mapped = g_task_propagate_pointer(G_TASK(result), &error);
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;
	if (!mapped) {
		request->on_loaded(NULL, error, request->data);
		return;
	}
	HexDocument *document = g_new0(HexDocument, 1);
	document->file = g_object_ref(request->file);
	document->mapped = mapped;
	document->bytes = (const guint8 *)g_mapped_file_get_contents(mapped);
	document->length = g_mapped_file_get_length(mapped);
	document->patches = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	document->undo = g_array_new(FALSE, FALSE, sizeof(HexEdit));
	document->redo = g_array_new(FALSE, FALSE, sizeof(HexEdit));
	document->listeners = g_array_new(FALSE, FALSE, sizeof(HexListener));
	request->on_loaded(document, NULL, request->data);
}


void load_hex_document(GFile *file, GCancellable *cancellable, HexLoadedFunction on_loaded, gpointer data) {
	LoadRequest *request = g_new0(LoadRequest, 1);
	request->file = g_object_ref(file);
	request->on_loaded = on_loaded;
	request->data = data;
	g_autoptr(GTask) task = g_task_new(NULL, cancellable, on_mapped, NULL);
	g_task_set_task_data(task, request, free_load_request);
	g_task_run_in_thread(task, map_in_thread);
}


void free_hex_document(HexDocument *document) {
	if (!document)
		return;
	g_object_unref(document->file);
	g_mapped_file_unref(document->mapped);
	g_hash_table_unref(document->patches);
	g_array_unref(document->undo);
	g_array_unref(document->redo);
	g_array_unref(document->listeners);
	g_free(document);
}


static int get_patch(HexDocument *document, gint64 offset) {
	gpointer value = NULL;
	if (!g_hash_table_lookup_extended(document->patches, &offset, NULL, &value))
		return -1;
	return GPOINTER_TO_INT(value);
}


static guint8 get_byte(HexDocument *document, gint64 offset) {
	int patch = get_patch(document, offset);
	return patch >= 0 ? (guint8)patch : document->bytes[offset];
}


static void set_patch(HexDocument *document, gint64 offset, int value) {
	if (value < 0) {
		g_hash_table_remove(document->patches, &offset);
		return;
	}
	gint64 *key = g_new(gint64, 1);
	*key = offset;
	g_hash_table_replace(document->patches, key, GINT_TO_POINTER(value));
}


static void notify_changed(HexDocument *document) {
	for (guint index = 0; index < document->listeners->len; index++) {
		HexListener *listener = &g_array_index(document->listeners, HexListener, index);
		listener->on_changed(listener->data);
	}
}


static void change_byte(HexDocument *document, gint64 offset, guint8 value) {
	if (get_byte(document, offset) == value)
		return;
	HexEdit edit = {offset, get_patch(document, offset), value, document->next_group};
	set_patch(document, offset, value);
	g_array_append_val(document->undo, edit);
	g_array_set_size(document->redo, 0);
	if (document->saved_depth > document->undo->len)
		document->saved_depth = G_MAXUINT;
}


/* Starts a new group of changes, so the next ones undo apart from those before */
static void begin_group(HexDocument *document) {
	document->next_group++;
}


/* Moves the newest group of changes from one stack to the other, returning an offset or -1 when there was none */
static gint64 step_history(HexDocument *document, gboolean is_undo) {
	GArray *from = is_undo ? document->undo : document->redo, *to = is_undo ? document->redo : document->undo;
	if (from->len == 0)
		return -1;
	guint group = g_array_index(from, HexEdit, from->len - 1).group;
	gint64 offset = -1;
	while (from->len > 0 && g_array_index(from, HexEdit, from->len - 1).group == group) {
		HexEdit edit = g_array_index(from, HexEdit, from->len - 1);
		g_array_set_size(from, from->len - 1);
		set_patch(document, edit.offset, is_undo ? edit.before : edit.after);
		g_array_append_val(to, edit);
		offset = edit.offset;
	}
	notify_changed(document);
	return offset;
}


gboolean is_hex_document_modified(HexDocument *document) {
	return document->undo->len != document->saved_depth;
}


/* The file is replaced as a whole, so the mapping of the old one stays valid and the patches still describe it */
gboolean save_hex_document(HexDocument *document, GFile *file, GError **error) {
	GFile *target = file ? file : document->file;
	g_autoptr(GFileOutputStream) stream = g_file_replace(target, NULL, FALSE, G_FILE_CREATE_NONE, NULL, error);
	if (!stream)
		return FALSE;
	g_autofree guint8 *chunk = g_malloc(SAVE_CHUNK_BYTES);
	for (gsize start = 0; start < document->length; start += SAVE_CHUNK_BYTES) {
		gsize size = MIN(SAVE_CHUNK_BYTES, document->length - start);
		for (gsize index = 0; index < size; index++)
			chunk[index] = get_byte(document, (gint64)(start + index));
		if (!g_output_stream_write_all(G_OUTPUT_STREAM(stream), chunk, size, NULL, NULL, error))
			return FALSE;
	}
	if (!g_output_stream_close(G_OUTPUT_STREAM(stream), NULL, error))
		return FALSE;
	document->saved_depth = document->undo->len;
	notify_changed(document);
	return TRUE;
}


/* ==================================== The view widget ==================================== */


/* Byte kinds coloured apart, as hexyl does, so text, padding and machine code stand out from each other */
typedef enum {
	BYTE_NULL,
	BYTE_PRINTABLE,
	BYTE_WHITESPACE,
	BYTE_OTHER_ASCII,
	BYTE_HIGH,
	BYTE_KIND_COUNT,
} ByteKind;

/* Scheme styles tried for each kind, and for offsets and changed bytes */
static const char *const KIND_STYLES[BYTE_KIND_COUNT][3] = {
	[BYTE_NULL] = {NULL}, [BYTE_PRINTABLE] = {"def:string", NULL},
	[BYTE_WHITESPACE] = {"def:comment", NULL}, [BYTE_OTHER_ASCII] = {"def:special-char", "def:keyword", NULL},
	[BYTE_HIGH] = {"def:number", "def:decimal", NULL},
};

struct _HexView {
	GtkWidget parent_instance;
	HexDocument *document;
	GtkSourceBuffer *colours;
	GtkAdjustment *hadjustment, *vadjustment;
	GtkScrollablePolicy hscroll_policy, vscroll_policy;
	gint64 cursor, anchor;
	gboolean is_text_pane, is_low_nibble;
	double char_width, row_height;
	GdkRGBA kind_colours[BYTE_KIND_COUNT], offset_colour, changed_colour, text_colour;
	/* Called after the cursor moves, for the data inspector */
	void (*on_moved)(gpointer data);
	gpointer moved_data;
	GtkWidget *goto_popover, *goto_entry;
	/* The find bar's patterns, whose matches are marked in the rows */
	GPtrArray *highlights;
	GdkRGBA match_colour;
};

enum {
	PROPERTY_HADJUSTMENT = 1,
	PROPERTY_VADJUSTMENT,
	PROPERTY_HSCROLL_POLICY,
	PROPERTY_VSCROLL_POLICY,
};

#define HEX_VIEW_TYPE (hex_view_get_type())
G_DECLARE_FINAL_TYPE(HexView, hex_view, CRIMSON, HEX_VIEW, GtkWidget)
G_DEFINE_FINAL_TYPE_WITH_CODE(HexView, hex_view, GTK_TYPE_WIDGET, G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, NULL))


static gint64 count_rows(HexView *view) {
	return (gint64)((view->document->length + BYTES_PER_ROW - 1) / BYTES_PER_ROW);
}


static int get_offset_digits(HexView *view) {
	return view->document->length > G_MAXUINT32 ? 16 : 8;
}


/* Column positions in characters, where the hex column has an extra space between its two groups */
static double get_hex_x(HexView *view, int column) {
	double start = PADDING_PIXELS + (get_offset_digits(view) + COLUMN_GAP) * view->char_width;
	return start + (column * 3 + (column >= GROUP_SIZE ? 1 : 0)) * view->char_width;
}


static double get_text_x(HexView *view, int column) {
	return get_hex_x(view, BYTES_PER_ROW) + (COLUMN_GAP - 1 + column) * view->char_width;
}


static ByteKind get_kind(guint8 byte) {
	if (byte == 0)
		return BYTE_NULL;
	if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r')
		return BYTE_WHITESPACE;
	if (byte >= 0x21 && byte <= 0x7e)
		return BYTE_PRINTABLE;
	return byte < 0x80 ? BYTE_OTHER_ASCII : BYTE_HIGH;
}


static gboolean read_style_colour(GtkSourceStyleScheme *scheme, const char *name, GdkRGBA *colour) {
	GtkSourceStyle *style = scheme && name ? gtk_source_style_scheme_get_style(scheme, name) : NULL;
	g_autofree char *value = NULL;
	gboolean is_set = FALSE;
	if (style)
		g_object_get(style, "foreground", &value, "foreground-set", &is_set, NULL);
	return is_set && value && gdk_rgba_parse(colour, value);
}


static void read_colours(HexView *view) {
	GtkSourceStyleScheme *scheme = gtk_source_buffer_get_style_scheme(view->colours);
	gtk_widget_get_color(GTK_WIDGET(view), &view->text_colour);
	for (int kind = 0; kind < BYTE_KIND_COUNT; kind++) {
		view->kind_colours[kind] = view->text_colour;
		for (int index = 0; KIND_STYLES[kind][index]; index++)
			if (read_style_colour(scheme, KIND_STYLES[kind][index], &view->kind_colours[kind]))
				break;
	}
	view->kind_colours[BYTE_NULL].alpha *= NULL_OPACITY;
	view->offset_colour = view->text_colour;
	view->offset_colour.alpha *= NULL_OPACITY;
	read_style_colour(scheme, "line-numbers", &view->offset_colour);
	view->changed_colour = (GdkRGBA){0.9f, 0.3f, 0.3f, 1};
	read_style_colour(scheme, "def:error", &view->changed_colour);
	view->match_colour = (GdkRGBA){0.95f, 0.8f, 0.2f, 0.45f};
	GtkSourceStyle *match = scheme ? gtk_source_style_scheme_get_style(scheme, "search-match") : NULL;
	g_autofree char *background = NULL;
	gboolean is_set = FALSE;
	if (match)
		g_object_get(match, "background", &background, "background-set", &is_set, NULL);
	if (is_set && background && gdk_rgba_parse(&view->match_colour, background))
		view->match_colour.alpha = MIN(view->match_colour.alpha, 0.6f);
}


static void measure_font(HexView *view) {
	PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(view), "0");
	int width = 0, height = 0;
	pango_layout_get_size(layout, &width, &height);
	g_object_unref(layout);
	view->char_width = (double)width / PANGO_SCALE;
	view->row_height = (double)height / PANGO_SCALE + 2;
}


static void update_adjustment(HexView *view) {
	if (!view->document || !view->vadjustment || view->row_height <= 0)
		return;
	double page = gtk_widget_get_height(GTK_WIDGET(view));
	/* A file shorter than the view still scrolls over the whole view, as GTK requires */
	double upper = MAX(count_rows(view) * view->row_height + PADDING_PIXELS * 2, page);
	double value = CLAMP(gtk_adjustment_get_value(view->vadjustment), 0, MAX(0, upper - page));
	gtk_adjustment_configure(view->vadjustment, value, 0, upper, view->row_height, page * 0.9, page);
	if (view->hadjustment) {
		double width = gtk_widget_get_width(GTK_WIDGET(view));
		double content = get_text_x(view, BYTES_PER_ROW) + PADDING_PIXELS;
		gtk_adjustment_configure(view->hadjustment, CLAMP(gtk_adjustment_get_value(view->hadjustment), 0,
			MAX(0, content - width)), 0, MAX(content, width), view->char_width, width * 0.9, width);
	}
}


static void draw_cell(GtkSnapshot *snapshot, const GdkRGBA *colour, double opacity, double x, double y,
	double width, double height) {
	GdkRGBA fill = *colour;
	fill.alpha *= (float)opacity;
	gtk_snapshot_append_color(snapshot, &fill, &GRAPHENE_RECT_INIT((float)x, (float)y, (float)width, (float)height));
}


static void add_colour(PangoAttrList *attributes, const GdkRGBA *colour, guint start, guint end) {
	PangoAttribute *attribute = pango_attr_foreground_new((guint16)(colour->red * 65535),
		(guint16)(colour->green * 65535), (guint16)(colour->blue * 65535));
	attribute->start_index = start;
	attribute->end_index = end;
	pango_attr_list_insert(attributes, attribute);
	PangoAttribute *alpha = pango_attr_foreground_alpha_new((guint16)(colour->alpha * 65535));
	alpha->start_index = start;
	alpha->end_index = end;
	pango_attr_list_insert(attributes, alpha);
}


static void draw_text(HexView *view, GtkSnapshot *snapshot, const char *text, PangoAttrList *attributes, double x,
	double y) {
	PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(view), text);
	pango_layout_set_attributes(layout, attributes);
	gtk_snapshot_save(snapshot);
	gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT((float)x, (float)y));
	gtk_snapshot_append_layout(snapshot, layout, &view->text_colour);
	gtk_snapshot_restore(snapshot);
	g_object_unref(layout);
}


static gboolean is_selected(HexView *view, gint64 offset) {
	if (view->anchor < 0)
		return FALSE;
	return offset >= MIN(view->anchor, view->cursor) && offset <= MAX(view->anchor, view->cursor);
}


static void draw_matches(HexView *view, GtkSnapshot *snapshot, gint64 start, int count, double y);


/* Each row draws its matches, selection and cursor first, then its offset, hex and characters as coloured text */
static void draw_row(HexView *view, GtkSnapshot *snapshot, gint64 row, double y) {
	HexDocument *document = view->document;
	gint64 start = row * BYTES_PER_ROW;
	int count = (int)MIN(BYTES_PER_ROW, (gint64)document->length - start);
	draw_matches(view, snapshot, start, count, y);
	for (int column = 0; column < count; column++) {
		gint64 offset = start + column;
		gboolean is_cursor = offset == view->cursor;
		if (!is_cursor && !is_selected(view, offset))
			continue;
		double opacity = is_cursor ? CURSOR_OPACITY : SELECTION_OPACITY;
		draw_cell(snapshot, &view->text_colour, view->is_text_pane && is_cursor ? opacity / 2 : opacity,
			get_hex_x(view, column) - 1, y, view->char_width * 2 + 2, view->row_height);
		draw_cell(snapshot, &view->text_colour, view->is_text_pane || !is_cursor ? opacity : opacity / 2,
			get_text_x(view, column), y, view->char_width, view->row_height);
	}
	g_autofree char *offset_text = g_strdup_printf(get_offset_digits(view) == 8 ? "%08" G_GINT64_MODIFIER "X"
		: "%016" G_GINT64_MODIFIER "X", start);
	PangoAttrList *offset_attributes = pango_attr_list_new();
	add_colour(offset_attributes, &view->offset_colour, 0, (guint)strlen(offset_text));
	draw_text(view, snapshot, offset_text, offset_attributes, PADDING_PIXELS, y + 1);
	pango_attr_list_unref(offset_attributes);
	GString *hex = g_string_new(NULL), *characters = g_string_new(NULL);
	PangoAttrList *hex_attributes = pango_attr_list_new(), *text_attributes = pango_attr_list_new();
	for (int column = 0; column < count; column++) {
		gint64 offset = start + column;
		guint8 byte = get_byte(document, offset);
		const GdkRGBA *colour = get_patch(document, offset) >= 0 ? &view->changed_colour
			: &view->kind_colours[get_kind(byte)];
		if (column == GROUP_SIZE)
			g_string_append_c(hex, ' ');
		guint hex_start = (guint)hex->len;
		g_string_append_printf(hex, "%02X ", byte);
		add_colour(hex_attributes, colour, hex_start, hex_start + 2);
		guint text_start = (guint)characters->len;
		g_string_append_c(characters, byte >= 0x20 && byte < 0x7f ? (char)byte : '.');
		add_colour(text_attributes, colour, text_start, text_start + 1);
	}
	draw_text(view, snapshot, hex->str, hex_attributes, get_hex_x(view, 0), y + 1);
	draw_text(view, snapshot, characters->str, text_attributes, get_text_x(view, 0), y + 1);
	pango_attr_list_unref(hex_attributes);
	pango_attr_list_unref(text_attributes);
	g_string_free(hex, TRUE);
	g_string_free(characters, TRUE);
}


static void hex_view_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
	HexView *view = CRIMSON_HEX_VIEW(widget);
	if (!view->document)
		return;
	if (view->row_height <= 0)
		measure_font(view);
	double top = view->vadjustment ? gtk_adjustment_get_value(view->vadjustment) : 0;
	double left = view->hadjustment ? gtk_adjustment_get_value(view->hadjustment) : 0;
	int height = gtk_widget_get_height(widget);
	gint64 first = (gint64)(MAX(0, top - PADDING_PIXELS) / view->row_height);
	gtk_snapshot_save(snapshot);
	gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT((float)-left, 0));
	for (gint64 row = first; row < count_rows(view); row++) {
		double y = PADDING_PIXELS + row * view->row_height - top;
		if (y > height)
			break;
		draw_row(view, snapshot, row, y);
	}
	gtk_snapshot_restore(snapshot);
}


static void hex_view_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
	update_adjustment(CRIMSON_HEX_VIEW(widget));
}


static void hex_view_css_changed(GtkWidget *widget, GtkCssStyleChange *change) {
	GTK_WIDGET_CLASS(hex_view_parent_class)->css_changed(widget, change);
	HexView *view = CRIMSON_HEX_VIEW(widget);
	measure_font(view);
	read_colours(view);
	update_adjustment(view);
	gtk_widget_queue_draw(widget);
}


static void hex_view_measure(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum,
	int *natural, int *minimum_baseline, int *natural_baseline) {
	HexView *view = CRIMSON_HEX_VIEW(widget);
	*minimum = *natural = 0;
	if (!view->document)
		return;
	if (view->row_height <= 0)
		measure_font(view);
	*natural = orientation == GTK_ORIENTATION_HORIZONTAL ? (int)(get_text_x(view, BYTES_PER_ROW) + PADDING_PIXELS)
		: (int)(count_rows(view) * view->row_height);
}


/* ==================================== Moving and editing ==================================== */


static void scroll_to_cursor(HexView *view) {
	if (!view->vadjustment || view->row_height <= 0)
		return;
	double y = PADDING_PIXELS + (double)(view->cursor / BYTES_PER_ROW) * view->row_height;
	double top = gtk_adjustment_get_value(view->vadjustment);
	double page = gtk_adjustment_get_page_size(view->vadjustment);
	if (y < top + PADDING_PIXELS)
		gtk_adjustment_set_value(view->vadjustment, y - PADDING_PIXELS);
	else if (y + view->row_height > top + page - PADDING_PIXELS)
		gtk_adjustment_set_value(view->vadjustment, y + view->row_height - page + PADDING_PIXELS);
}


static void move_cursor(HexView *view, gint64 offset, gboolean is_extending) {
	gint64 last = MAX(0, (gint64)view->document->length - 1);
	if (is_extending && view->anchor < 0)
		view->anchor = view->cursor;
	else if (!is_extending)
		view->anchor = -1;
	view->cursor = CLAMP(offset, 0, last);
	view->is_low_nibble = FALSE;
	scroll_to_cursor(view);
	gtk_widget_queue_draw(GTK_WIDGET(view));
	if (view->on_moved)
		view->on_moved(view->moved_data);
}


static void write_byte(HexView *view, guint8 value) {
	if (view->document->length == 0)
		return;
	begin_group(view->document);
	change_byte(view->document, view->cursor, value);
	notify_changed(view->document);
}


/* Two hex digits fill a byte from its high half to its low half, then the cursor moves on */
static gboolean type_hex_digit(HexView *view, guint key) {
	int digit = g_ascii_xdigit_value((char)gdk_keyval_to_unicode(key));
	if (digit < 0)
		return FALSE;
	guint8 byte = get_byte(view->document, view->cursor);
	byte = view->is_low_nibble ? (guint8)((byte & 0xf0) | digit) : (guint8)((byte & 0x0f) | (digit << 4));
	write_byte(view, byte);
	if (view->is_low_nibble)
		move_cursor(view, view->cursor + 1, FALSE);
	else
		view->is_low_nibble = TRUE;
	return TRUE;
}


static void copy_selection(HexView *view) {
	gint64 start = view->anchor < 0 ? view->cursor : MIN(view->anchor, view->cursor);
	gint64 end = view->anchor < 0 ? view->cursor : MAX(view->anchor, view->cursor);
	GString *text = g_string_new(NULL);
	for (gint64 offset = start; offset <= end && offset < (gint64)view->document->length; offset++) {
		guint8 byte = get_byte(view->document, offset);
		if (view->is_text_pane)
			g_string_append_c(text, byte >= 0x20 && byte < 0x7f ? (char)byte : '.');
		else
			g_string_append_printf(text, offset > start ? " %02X" : "%02X", byte);
	}
	gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(view)), text->str);
	g_string_free(text, TRUE);
}


/* Offsets are read as hex, as hex editors show them and guides copy them, with or without 0x before them */
static void on_goto_activated(GtkEntry *entry, HexView *view) {
	g_autofree char *text = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));
	gboolean has_prefix = g_str_has_prefix(text, "0x") || g_str_has_prefix(text, "0X");
	char *end = NULL;
	gint64 offset = g_ascii_strtoll(has_prefix ? text + 2 : text, &end, 16);
	gtk_popover_popdown(GTK_POPOVER(view->goto_popover));
	if (end && !*end)
		move_cursor(view, offset, FALSE);
	gtk_widget_grab_focus(GTK_WIDGET(view));
}


static void show_goto(HexView *view) {
	gtk_editable_set_text(GTK_EDITABLE(view->goto_entry), "");
	gtk_popover_popup(GTK_POPOVER(view->goto_popover));
	gtk_widget_grab_focus(view->goto_entry);
}


static gboolean handle_control_key(HexView *view, guint key, gboolean has_shift) {
	gint64 offset = -1;
	switch (key) {
	case GDK_KEY_z: case GDK_KEY_Z:
		offset = step_history(view->document, !has_shift);
		break;
	case GDK_KEY_y:
		offset = step_history(view->document, FALSE);
		break;
	case GDK_KEY_c:
		copy_selection(view);
		return TRUE;
	case GDK_KEY_g:
		show_goto(view);
		return TRUE;
	case GDK_KEY_a:
		view->anchor = 0;
		move_cursor(view, (gint64)view->document->length - 1, TRUE);
		return TRUE;
	case GDK_KEY_Home:
		move_cursor(view, 0, has_shift);
		return TRUE;
	case GDK_KEY_End:
		move_cursor(view, (gint64)view->document->length - 1, has_shift);
		return TRUE;
	default:
		return FALSE;
	}
	if (offset >= 0)
		move_cursor(view, offset, FALSE);
	return TRUE;
}


static gboolean on_key_pressed(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType state,
	HexView *view) {
	gboolean has_shift = (state & GDK_SHIFT_MASK) != 0;
	if (!view->document || view->document->length == 0)
		return FALSE;
	if (state & GDK_CONTROL_MASK)
		return handle_control_key(view, key, has_shift);
	gint64 page = MAX(1, (gint64)(gtk_widget_get_height(GTK_WIDGET(view)) / MAX(view->row_height, 1)) - 1);
	switch (key) {
	case GDK_KEY_Left: move_cursor(view, view->cursor - 1, has_shift); return TRUE;
	case GDK_KEY_Right: move_cursor(view, view->cursor + 1, has_shift); return TRUE;
	case GDK_KEY_Up: move_cursor(view, view->cursor - BYTES_PER_ROW, has_shift); return TRUE;
	case GDK_KEY_Down: move_cursor(view, view->cursor + BYTES_PER_ROW, has_shift); return TRUE;
	case GDK_KEY_Page_Up: move_cursor(view, view->cursor - page * BYTES_PER_ROW, has_shift); return TRUE;
	case GDK_KEY_Page_Down: move_cursor(view, view->cursor + page * BYTES_PER_ROW, has_shift); return TRUE;
	case GDK_KEY_Home: move_cursor(view, view->cursor - view->cursor % BYTES_PER_ROW, has_shift); return TRUE;
	case GDK_KEY_End:
		move_cursor(view, view->cursor - view->cursor % BYTES_PER_ROW + BYTES_PER_ROW - 1, has_shift);
		return TRUE;
	case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab:
		view->is_text_pane = !view->is_text_pane;
		view->is_low_nibble = FALSE;
		gtk_widget_queue_draw(GTK_WIDGET(view));
		return TRUE;
	case GDK_KEY_BackSpace: move_cursor(view, view->cursor - 1, FALSE); return TRUE;
	default: break;
	}
	if (!view->is_text_pane)
		return type_hex_digit(view, key);
	gunichar character = gdk_keyval_to_unicode(key);
	if (character < 0x20 || character >= 0x7f)
		return FALSE;
	write_byte(view, (guint8)character);
	move_cursor(view, view->cursor + 1, FALSE);
	return TRUE;
}


/* The click picks the byte and the column it is in, and Shift or dragging selects from the cursor */
static gint64 find_offset_at(HexView *view, double x, double y, gboolean *is_text) {
	double top = view->vadjustment ? gtk_adjustment_get_value(view->vadjustment) : 0;
	x += view->hadjustment ? gtk_adjustment_get_value(view->hadjustment) : 0;
	gint64 row = (gint64)MAX(0, (y + top - PADDING_PIXELS) / view->row_height);
	int column = 0;
	*is_text = x >= get_text_x(view, 0) - view->char_width;
	if (*is_text)
		column = (int)((x - get_text_x(view, 0)) / view->char_width);
	else
		for (column = BYTES_PER_ROW - 1; column > 0 && x < get_hex_x(view, column) - view->char_width / 2; column--) {
		}
	return row * BYTES_PER_ROW + CLAMP(column, 0, BYTES_PER_ROW - 1);
}


static void on_pressed(GtkGestureClick *gesture, int presses, double x, double y, HexView *view) {
	if (!view->document)
		return;
	gtk_widget_grab_focus(GTK_WIDGET(view));
	gboolean is_text = FALSE;
	gint64 offset = find_offset_at(view, x, y, &is_text);
	GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
	view->is_text_pane = is_text;
	move_cursor(view, offset, (state & GDK_SHIFT_MASK) != 0);
}


static void on_drag_updated(GtkGestureDrag *drag, double x, double y, HexView *view) {
	if (!view->document)
		return;
	double start_x = 0, start_y = 0;
	gtk_gesture_drag_get_start_point(drag, &start_x, &start_y);
	gboolean is_text = FALSE;
	move_cursor(view, find_offset_at(view, start_x + x, start_y + y, &is_text), TRUE);
}


static void on_adjustment_changed(GtkAdjustment *adjustment, HexView *view) {
	gtk_widget_queue_draw(GTK_WIDGET(view));
}


static void set_adjustment(HexView *view, GtkAdjustment **slot, GtkAdjustment *adjustment) {
	if (*slot == adjustment)
		return;
	if (*slot) {
		g_signal_handlers_disconnect_by_data(*slot, view);
		g_object_unref(*slot);
	}
	*slot = adjustment ? g_object_ref_sink(adjustment) : NULL;
	if (*slot)
		g_signal_connect(*slot, "value-changed", G_CALLBACK(on_adjustment_changed), view);
	update_adjustment(view);
}


static void hex_view_set_property(GObject *object, guint id, const GValue *value, GParamSpec *specification) {
	HexView *view = CRIMSON_HEX_VIEW(object);
	switch (id) {
	case PROPERTY_HADJUSTMENT: set_adjustment(view, &view->hadjustment, g_value_get_object(value)); break;
	case PROPERTY_VADJUSTMENT: set_adjustment(view, &view->vadjustment, g_value_get_object(value)); break;
	case PROPERTY_HSCROLL_POLICY: view->hscroll_policy = g_value_get_enum(value); break;
	case PROPERTY_VSCROLL_POLICY: view->vscroll_policy = g_value_get_enum(value); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, specification);
	}
}


static void hex_view_get_property(GObject *object, guint id, GValue *value, GParamSpec *specification) {
	HexView *view = CRIMSON_HEX_VIEW(object);
	switch (id) {
	case PROPERTY_HADJUSTMENT: g_value_set_object(value, view->hadjustment); break;
	case PROPERTY_VADJUSTMENT: g_value_set_object(value, view->vadjustment); break;
	case PROPERTY_HSCROLL_POLICY: g_value_set_enum(value, view->hscroll_policy); break;
	case PROPERTY_VSCROLL_POLICY: g_value_set_enum(value, view->vscroll_policy); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, specification);
	}
}


static void hex_view_dispose(GObject *object) {
	HexView *view = CRIMSON_HEX_VIEW(object);
	g_clear_pointer(&view->highlights, g_ptr_array_unref);
	set_adjustment(view, &view->hadjustment, NULL);
	set_adjustment(view, &view->vadjustment, NULL);
	g_clear_pointer(&view->goto_popover, gtk_widget_unparent);
	G_OBJECT_CLASS(hex_view_parent_class)->dispose(object);
}


static void hex_view_class_init(HexViewClass *class) {
	GObjectClass *object_class = G_OBJECT_CLASS(class);
	object_class->set_property = hex_view_set_property;
	object_class->get_property = hex_view_get_property;
	object_class->dispose = hex_view_dispose;
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(class);
	widget_class->snapshot = hex_view_snapshot;
	widget_class->size_allocate = hex_view_size_allocate;
	widget_class->measure = hex_view_measure;
	widget_class->css_changed = hex_view_css_changed;
	g_object_class_override_property(object_class, PROPERTY_HADJUSTMENT, "hadjustment");
	g_object_class_override_property(object_class, PROPERTY_VADJUSTMENT, "vadjustment");
	g_object_class_override_property(object_class, PROPERTY_HSCROLL_POLICY, "hscroll-policy");
	g_object_class_override_property(object_class, PROPERTY_VSCROLL_POLICY, "vscroll-policy");
}


static void hex_view_init(HexView *view) {
	view->anchor = -1;
	gtk_widget_set_focusable(GTK_WIDGET(view), TRUE);
	gtk_widget_set_overflow(GTK_WIDGET(view), GTK_OVERFLOW_HIDDEN);
	gtk_widget_add_css_class(GTK_WIDGET(view), "code-text");
	gtk_widget_add_css_class(GTK_WIDGET(view), "hex-view");
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), view);
	gtk_widget_add_controller(GTK_WIDGET(view), keys);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), view);
	gtk_widget_add_controller(GTK_WIDGET(view), GTK_EVENT_CONTROLLER(click));
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_updated), view);
	gtk_widget_add_controller(GTK_WIDGET(view), GTK_EVENT_CONTROLLER(drag));
	view->goto_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(view->goto_entry), TEXT_HEX_GOTO);
	g_signal_connect(view->goto_entry, "activate", G_CALLBACK(on_goto_activated), view);
	view->goto_popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(view->goto_popover), view->goto_entry);
	gtk_widget_set_parent(view->goto_popover, GTK_WIDGET(view));
}


/* ==================================== The inspector ==================================== */


/* The page holds the view, the inspector reading the bytes at the cursor, and the document's listener */
typedef struct {
	HexView *view;
	GtkWidget *values[12], *position, *byte_order;
	HexChangedFunction on_changed;
	gpointer data;
} HexPage;

static const char *const INSPECTOR_NAMES[] = {
	"u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "f32", "f64", TEXT_HEX_BINARY, TEXT_HEX_CHARACTER,
};


/* Reads up to eight bytes from the cursor in the chosen byte order, reporting how many were there */
static guint64 read_number(HexPage *page, int size, gboolean is_big, int *available) {
	HexDocument *document = page->view->document;
	gint64 cursor = page->view->cursor;
	*available = (int)MIN(size, (gint64)document->length - cursor);
	guint64 value = 0;
	for (int index = 0; index < *available; index++) {
		guint64 byte = get_byte(document, cursor + index);
		value |= byte << (8 * (is_big ? size - 1 - index : index));
	}
	return value;
}


static void update_inspector(gpointer data);


/* A value being typed into is left alone until it is committed */
static void set_value(HexPage *page, int index, const char *text) {
	if (!gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(page->values[index])))
		gtk_editable_set_text(GTK_EDITABLE(page->values[index]), text);
}


/* A plain number, or a division such as 2560/1080 or 21/9, since aspect ratios are given that way */
static gboolean parse_number_or_ratio(const char *text, double *number) {
	char *end = NULL;
	*number = g_ascii_strtod(text, &end);
	if (!*text || !end || end == text)
		return FALSE;
	if (!*end)
		return TRUE;
	if (*end != '/')
		return FALSE;
	const char *divisor_text = end + 1;
	double divisor = g_ascii_strtod(divisor_text, &end);
	if (end == divisor_text || *end || divisor == 0)
		return FALSE;
	*number /= divisor;
	return TRUE;
}


static gboolean parse_integer(const char *text, gboolean is_signed, int size, guint64 *bits) {
	gboolean is_hex = g_str_has_prefix(text, "0x") || g_str_has_prefix(text, "0X");
	if (is_signed && !is_hex) {
		gint64 limit = size == 8 ? G_MAXINT64 : ((gint64)1 << (size * 8 - 1)) - 1, value = 0;
		if (!g_ascii_string_to_signed(text, 10, -limit - 1, limit, &value, NULL))
			return FALSE;
		*bits = (guint64)value;
		return TRUE;
	}
	guint64 limit = size == 8 ? G_MAXUINT64 : ((guint64)1 << (size * 8)) - 1;
	return g_ascii_string_to_unsigned(is_hex ? text + 2 : text, is_hex ? 16 : 10, 0, limit, bits, NULL);
}


/* Turns what was typed for one of the inspector's kinds into bits and their size, or FALSE when it does not read */
static gboolean parse_inspector_value(int index, const char *text, guint64 *bits, int *size) {
	static const int SIZES[] = {1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 1, 1};
	*size = SIZES[index];
	if (index < 8)
		return parse_integer(text, index % 2 == 1, *size, bits);
	if (index == 8 || index == 9) {
		double number = 0;
		if (!parse_number_or_ratio(text, &number))
			return FALSE;
		if (index == 8) {
			float single = (float)number;
			guint32 single_bits = 0;
			memcpy(&single_bits, &single, sizeof single_bits);
			*bits = single_bits;
		} else {
			memcpy(bits, &number, sizeof number);
		}
		return TRUE;
	}
	if (index == 10) {
		if (strlen(text) != 8 || strspn(text, "01") != 8)
			return FALSE;
		*bits = g_ascii_strtoull(text, NULL, 2);
		return TRUE;
	}
	if (strlen(text) != 1 || (guchar)text[0] < 0x20 || (guchar)text[0] >= 0x7f)
		return FALSE;
	*bits = (guchar)text[0];
	return TRUE;
}


/* A value typed into the inspector is written at the cursor in the chosen byte order, as one undo step, so a
 * float such as an aspect ratio can be changed as a number rather than worked out in hex */
static void on_inspector_edited(GObject *label, GParamSpec *property, HexPage *page) {
	if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(label)) || !page->view->document)
		return;
	int index = GPOINTER_TO_INT(g_object_get_data(label, "inspector-index"));
	HexDocument *document = page->view->document;
	gint64 cursor = page->view->cursor;
	g_autofree char *text = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(label))));
	guint64 bits = 0;
	int size = 0;
	gboolean is_big = g_strcmp0(adw_toggle_group_get_active_name(ADW_TOGGLE_GROUP(page->byte_order)), "big") == 0;
	if (!parse_inspector_value(index, text, &bits, &size) || cursor + size > (gint64)document->length) {
		update_inspector(page);
		return;
	}
	begin_group(document);
	for (int byte = 0; byte < size; byte++)
		change_byte(document, cursor + byte, (guint8)(bits >> (8 * (is_big ? size - 1 - byte : byte))));
	notify_changed(document);
}


static void update_inspector(gpointer data) {
	HexPage *page = data;
	HexDocument *document = page->view->document;
	gboolean is_big = g_strcmp0(adw_toggle_group_get_active_name(ADW_TOGGLE_GROUP(page->byte_order)), "big") == 0;
	static const int SIZES[] = {1, 2, 4, 8};
	for (int size_index = 0; size_index < 4; size_index++) {
		int size = SIZES[size_index], available = 0;
		guint64 value = read_number(page, size, is_big, &available);
		gint64 signed_value = size == 8 ? (gint64)value : (gint64)(value << (64 - size * 8)) >> (64 - size * 8);
		g_autofree char *unsigned_text = g_strdup_printf("%" G_GUINT64_FORMAT, value);
		g_autofree char *signed_text = g_strdup_printf("%" G_GINT64_FORMAT, signed_value);
		set_value(page, size_index * 2, available == size ? unsigned_text : "—");
		set_value(page, size_index * 2 + 1, available == size ? signed_text : "—");
	}
	int available = 0;
	guint32 single_bits = (guint32)read_number(page, 4, is_big, &available);
	float single = 0;
	memcpy(&single, &single_bits, sizeof single);
	char single_text[G_ASCII_DTOSTR_BUF_SIZE];
	g_ascii_formatd(single_text, sizeof single_text, "%g", single);
	set_value(page, 8, available == 4 ? single_text : "—");
	guint64 double_bits = read_number(page, 8, is_big, &available);
	double number = 0;
	memcpy(&number, &double_bits, sizeof number);
	char double_text[G_ASCII_DTOSTR_BUF_SIZE];
	g_ascii_formatd(double_text, sizeof double_text, "%g", number);
	set_value(page, 9, available == 8 ? double_text : "—");
	guint8 byte = document->length ? get_byte(document, page->view->cursor) : 0;
	char binary[9];
	for (int bit = 0; bit < 8; bit++)
		binary[bit] = (byte >> (7 - bit)) & 1 ? '1' : '0';
	binary[8] = '\0';
	set_value(page, 10, binary);
	char character[2] = {byte >= 0x20 && byte < 0x7f ? (char)byte : '.', '\0'};
	set_value(page, 11, character);
	g_autofree char *size = g_format_size(document->length);
	g_autofree char *position = g_strdup_printf(TEXT_HEX_POSITION, page->view->cursor, page->view->cursor, size);
	gtk_label_set_text(GTK_LABEL(page->position), position);
}


static void on_document_changed(gpointer data) {
	HexPage *page = data;
	gtk_widget_queue_draw(GTK_WIDGET(page->view));
	update_inspector(page);
	if (page->on_changed)
		page->on_changed(page->data);
}


static void on_byte_order_changed(GObject *group, GParamSpec *property, HexPage *page) {
	update_inspector(page);
}


static void on_scheme_changed(GObject *buffer, GParamSpec *property, HexView *view) {
	read_colours(view);
	gtk_widget_queue_draw(GTK_WIDGET(view));
}


static GtkWidget *create_inspector(HexPage *page) {
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
	for (guint index = 0; index < G_N_ELEMENTS(INSPECTOR_NAMES); index++) {
		GtkWidget *name = gtk_label_new(INSPECTOR_NAMES[index]);
		gtk_label_set_xalign(GTK_LABEL(name), 0);
		gtk_widget_add_css_class(name, "dim-label");
		page->values[index] = gtk_editable_label_new("");
		g_object_set_data(G_OBJECT(page->values[index]), "inspector-index", GINT_TO_POINTER(index));
		g_signal_connect(page->values[index], "notify::editing", G_CALLBACK(on_inspector_edited), page);
		gtk_widget_set_tooltip_text(page->values[index], TEXT_HEX_EDIT_VALUE);
		gtk_widget_add_css_class(page->values[index], "monospace");
		gtk_grid_attach(GTK_GRID(grid), name, 0, (int)index, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), page->values[index], 1, (int)index, 1, 1);
	}
	page->byte_order = adw_toggle_group_new();
	const char *const names[] = {"little", "big"}, *const labels[] = {TEXT_HEX_LITTLE, TEXT_HEX_BIG};
	for (int index = 0; index < 2; index++) {
		AdwToggle *toggle = adw_toggle_new();
		adw_toggle_set_name(toggle, names[index]);
		adw_toggle_set_label(toggle, labels[index]);
		adw_toggle_group_add(ADW_TOGGLE_GROUP(page->byte_order), toggle);
	}
	g_signal_connect(page->byte_order, "notify::active-name", G_CALLBACK(on_byte_order_changed), page);
	page->position = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(page->position), 0);
	gtk_label_set_wrap(GTK_LABEL(page->position), TRUE);
	gtk_widget_add_css_class(page->position, "dim-label");
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_add_css_class(box, "hex-inspector");
	gtk_box_append(GTK_BOX(box), page->byte_order);
	gtk_box_append(GTK_BOX(box), grid);
	gtk_box_append(GTK_BOX(box), page->position);
	return box;
}


static void free_page(gpointer data) {
	g_free(data);
}


GtkWidget *create_hex_view(HexDocument *document, GtkSourceBuffer *colours, HexChangedFunction on_changed,
	gpointer data) {
	HexPage *page = g_new0(HexPage, 1);
	page->on_changed = on_changed;
	page->data = data;
	HexView *view = g_object_new(HEX_VIEW_TYPE, NULL);
	view->document = document;
	view->colours = colours;
	view->on_moved = update_inspector;
	view->moved_data = page;
	page->view = view;
	read_colours(view);
	g_signal_connect(colours, "notify::style-scheme", G_CALLBACK(on_scheme_changed), view);
	HexListener listener = {on_document_changed, page};
	g_array_append_val(document->listeners, listener);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), GTK_WIDGET(view));
	gtk_widget_set_hexpand(scroller, TRUE);
	GtkWidget *inspector = create_inspector(page);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(box, "hex-page");
	gtk_box_append(GTK_BOX(box), scroller);
	gtk_box_append(GTK_BOX(box), inspector);
	g_object_set_data_full(G_OBJECT(box), VIEW_KEY, page, free_page);
	update_inspector(page);
	return box;
}


void free_hex_view(GtkWidget *widget) {
	HexPage *page = g_object_get_data(G_OBJECT(widget), VIEW_KEY);
	if (!page)
		return;
	HexDocument *document = page->view->document;
	for (guint index = 0; index < document->listeners->len; index++)
		if (g_array_index(document->listeners, HexListener, index).data == page) {
			g_array_remove_index(document->listeners, index);
			break;
		}
	g_signal_handlers_disconnect_by_data(page->view->colours, page->view);
	page->view->on_moved = NULL;
	page->on_changed = NULL;
	/* The tab may keep the widget a moment after the document is gone, so it stops reading from it */
	page->view->document = NULL;
}


void focus_hex_view(GtkWidget *widget) {
	HexPage *page = g_object_get_data(G_OBJECT(widget), VIEW_KEY);
	if (page)
		gtk_widget_grab_focus(GTK_WIDGET(page->view));
}


/* ==================================== Searching ==================================== */


/* One way of writing what is searched for, where wild marks bytes that match anything */
typedef struct {
	GByteArray *bytes, *wild;
	gboolean is_text;
} SearchPattern;


static void free_pattern(gpointer data) {
	SearchPattern *pattern = data;
	g_byte_array_unref(pattern->bytes);
	g_byte_array_unref(pattern->wild);
	g_free(pattern);
}


static SearchPattern *new_pattern(gboolean is_text) {
	SearchPattern *pattern = g_new0(SearchPattern, 1);
	pattern->bytes = g_byte_array_new();
	pattern->wild = g_byte_array_new();
	pattern->is_text = is_text;
	return pattern;
}


static void add_pattern_byte(SearchPattern *pattern, guint8 byte, gboolean is_wild) {
	guint8 wild = is_wild;
	g_byte_array_append(pattern->bytes, &byte, 1);
	g_byte_array_append(pattern->wild, &wild, 1);
}


/* Bytes written as hex pairs with ?? for any byte, such as 39 8E E3 3F
 * Pairs may be run together when the text has a digit, so a plain word such as cafe stays text */
static SearchPattern *parse_hex(const char *query) {
	g_autofree char *trimmed = g_strstrip(g_strdup(query));
	gboolean has_digit = FALSE, has_space = FALSE;
	for (const char *character = trimmed; *character; character++) {
		has_digit |= g_ascii_isdigit(*character);
		has_space |= g_ascii_isspace(*character);
		if (!g_ascii_isxdigit(*character) && !g_ascii_isspace(*character) && *character != '?')
			return NULL;
	}
	if (!*trimmed || (!has_space && !has_digit && !strchr(trimmed, '?')))
		return NULL;
	SearchPattern *pattern = new_pattern(FALSE);
	g_auto(GStrv) words = g_strsplit_set(trimmed, " \t", -1);
	for (char **word = words; *word; word++) {
		size_t length = strlen(*word);
		if (length % 2) {
			free_pattern(pattern);
			return NULL;
		}
		for (size_t index = 0; index < length; index += 2) {
			gboolean is_wild = (*word)[index] == '?' && (*word)[index + 1] == '?';
			int high = g_ascii_xdigit_value((*word)[index]), low = g_ascii_xdigit_value((*word)[index + 1]);
			if (!is_wild && (high < 0 || low < 0)) {
				free_pattern(pattern);
				return NULL;
			}
			add_pattern_byte(pattern, is_wild ? 0 : (guint8)(high << 4 | low), is_wild);
		}
	}
	return pattern;
}


/* Text is looked for both as single bytes and as UTF-16, which Windows programs use for many of their strings.
 * Quotes around the text keep a run of hex digits as text */
static GPtrArray *parse_query(const char *query) {
	GPtrArray *patterns = g_ptr_array_new_with_free_func(free_pattern);
	size_t length = strlen(query);
	gboolean is_quoted = length >= 2 && query[0] == '"' && query[length - 1] == '"';
	SearchPattern *hex = is_quoted ? NULL : parse_hex(query);
	if (hex) {
		g_ptr_array_add(patterns, hex);
		return patterns;
	}
	g_autofree char *text = is_quoted ? g_strndup(query + 1, length - 2) : g_strdup(query);
	if (!*text)
		return patterns;
	SearchPattern *single = new_pattern(TRUE), *wide = new_pattern(TRUE);
	for (const char *character = text; *character; character++) {
		add_pattern_byte(single, (guint8)*character, FALSE);
		add_pattern_byte(wide, (guint8)*character, FALSE);
		add_pattern_byte(wide, 0, FALSE);
	}
	g_ptr_array_add(patterns, single);
	g_ptr_array_add(patterns, wide);
	return patterns;
}


static gboolean matches_at(HexDocument *document, gint64 offset, const SearchPattern *pattern) {
	guint length = pattern->bytes->len;
	if (offset < 0 || offset + (gint64)length > (gint64)document->length)
		return FALSE;
	for (guint index = 0; index < length; index++) {
		if (pattern->wild->data[index])
			continue;
		guint8 byte = get_byte(document, offset + index), wanted = pattern->bytes->data[index];
		if (pattern->is_text ? g_ascii_tolower(byte) != g_ascii_tolower(wanted) : byte != wanted)
			return FALSE;
	}
	return TRUE;
}


static SearchPattern *find_pattern_at(HexDocument *document, gint64 offset, GPtrArray *patterns) {
	for (guint index = 0; index < patterns->len; index++)
		if (matches_at(document, offset, g_ptr_array_index(patterns, index)))
			return g_ptr_array_index(patterns, index);
	return NULL;
}


/* Searches from an offset round the whole file, returning the offset of the first match or -1 */
static gint64 find_from(HexDocument *document, GPtrArray *patterns, gint64 from, gboolean is_backward) {
	gint64 length = (gint64)document->length;
	if (length == 0 || patterns->len == 0)
		return -1;
	for (gint64 step = 0; step < length; step++) {
		gint64 offset = is_backward ? ((from - step) % length + length) % length : (from + step) % length;
		if (find_pattern_at(document, offset, patterns))
			return offset;
	}
	return -1;
}


static void draw_matches(HexView *view, GtkSnapshot *snapshot, gint64 start, int count, double y) {
	if (!view->highlights || view->highlights->len == 0)
		return;
	gboolean marked[BYTES_PER_ROW] = {FALSE};
	for (guint index = 0; index < view->highlights->len; index++) {
		SearchPattern *pattern = g_ptr_array_index(view->highlights, index);
		gint64 length = pattern->bytes->len;
		for (gint64 offset = MAX(0, start - length + 1); offset < start + count; offset++) {
			if (!matches_at(view->document, offset, pattern))
				continue;
			for (gint64 at = MAX(offset, start); at < MIN(offset + length, start + count); at++)
				marked[at - start] = TRUE;
		}
	}
	for (int column = 0; column < count; column++) {
		if (!marked[column])
			continue;
		draw_cell(snapshot, &view->match_colour, 1, get_hex_x(view, column) - 1, y, view->char_width * 2 + 2,
			view->row_height);
		draw_cell(snapshot, &view->match_colour, 1, get_text_x(view, column), y, view->char_width,
			view->row_height);
	}
}


static HexView *get_target_view(gpointer data) {
	HexPage *page = g_object_get_data(G_OBJECT(data), VIEW_KEY);
	return page && page->view->document ? page->view : NULL;
}


static void select_range(HexView *view, gint64 start, gint64 length) {
	view->anchor = start;
	view->cursor = start + MAX(1, length) - 1;
	view->is_low_nibble = FALSE;
	scroll_to_cursor(view);
	gtk_widget_queue_draw(GTK_WIDGET(view));
	if (view->on_moved)
		view->on_moved(view->moved_data);
}


/* The match the selection covers, or NULL when the selection is not exactly a match */
static SearchPattern *get_selected_match(HexView *view, GPtrArray *patterns, gint64 *start) {
	*start = view->anchor < 0 ? view->cursor : MIN(view->anchor, view->cursor);
	gint64 length = (view->anchor < 0 ? 0 : ABS(view->cursor - view->anchor)) + 1;
	SearchPattern *pattern = find_pattern_at(view->document, *start, patterns);
	return pattern && pattern->bytes->len == length ? pattern : NULL;
}


static gboolean find_in_view(gpointer data, const char *query, gboolean is_backward, gboolean includes_current) {
	HexView *view = get_target_view(data);
	if (!view)
		return FALSE;
	g_autoptr(GPtrArray) patterns = parse_query(query);
	gint64 selected = 0;
	gboolean is_on_match = get_selected_match(view, patterns, &selected) != NULL;
	gint64 from = includes_current ? selected : is_backward ? selected - 1 : selected + (is_on_match ? 1 : 0);
	gint64 found = find_from(view->document, patterns, from, is_backward);
	if (found < 0)
		return FALSE;
	select_range(view, found, find_pattern_at(view->document, found, patterns)->bytes->len);
	return TRUE;
}


/* Counting reads the whole file, so files too large to count quickly show only the selected match */
static void count_in_view(gpointer data, const char *query, int *position, int *total) {
	*position = *total = 0;
	HexView *view = get_target_view(data);
	if (!view || view->document->length > COUNT_LIMIT_BYTES)
		return;
	g_autoptr(GPtrArray) patterns = parse_query(query);
	gint64 selected = 0;
	gboolean is_on_match = get_selected_match(view, patterns, &selected) != NULL;
	for (gint64 offset = 0; patterns->len && offset < (gint64)view->document->length; offset++) {
		if (!find_pattern_at(view->document, offset, patterns))
			continue;
		(*total)++;
		if (is_on_match && offset == selected)
			*position = *total;
	}
}


/* The replacement is written the way the match was, as bytes, single byte text or UTF-16, and keeps the same
 * length since bytes are overwritten rather than inserted */
static GByteArray *encode_replacement(const char *replacement, const SearchPattern *match, char **message) {
	g_autoptr(GPtrArray) parsed = parse_query(replacement);
	GByteArray *bytes = g_byte_array_new();
	gboolean is_wide = match->is_text && match->bytes->len >= 2 && match->bytes->data[1] == 0;
	if (!match->is_text) {
		SearchPattern *hex = parsed->len == 1 && !((SearchPattern *)parsed->pdata[0])->is_text ? parsed->pdata[0] : NULL;
		if (hex)
			g_byte_array_append(bytes, hex->bytes->data, hex->bytes->len);
	} else {
		for (const char *character = replacement; *character; character++) {
			guint8 byte = (guint8)*character, zero = 0;
			g_byte_array_append(bytes, &byte, 1);
			if (is_wide)
				g_byte_array_append(bytes, &zero, 1);
		}
	}
	if (bytes->len != match->bytes->len) {
		*message = g_strdup_printf(TEXT_HEX_REPLACE_LENGTH, match->bytes->len);
		g_byte_array_unref(bytes);
		return NULL;
	}
	return bytes;
}


static void write_match(HexView *view, gint64 start, GByteArray *bytes, const SearchPattern *pattern) {
	for (guint index = 0; index < bytes->len; index++)
		if (!pattern->wild->data[index])
			change_byte(view->document, start + index, bytes->data[index]);
}


static int replace_in_view(gpointer data, const char *query, const char *replacement, gboolean is_all,
	char **message) {
	HexView *view = get_target_view(data);
	if (!view || !*query)
		return 0;
	g_autoptr(GPtrArray) patterns = parse_query(query);
	gint64 start = 0;
	SearchPattern *selected = get_selected_match(view, patterns, &start);
	if (!is_all && !selected) {
		find_in_view(data, query, FALSE, TRUE);
		return 0;
	}
	int count = 0;
	begin_group(view->document);
	for (gint64 offset = is_all ? 0 : start; offset < (gint64)view->document->length; offset++) {
		SearchPattern *pattern = is_all ? find_pattern_at(view->document, offset, patterns) : selected;
		if (!pattern)
			continue;
		g_autoptr(GByteArray) bytes = encode_replacement(replacement, pattern, message);
		if (!bytes)
			return count > 0 ? count : -1;
		write_match(view, offset, bytes, pattern);
		count++;
		offset += pattern->bytes->len - 1;
		if (!is_all)
			break;
	}
	notify_changed(view->document);
	return count;
}


static void highlight_in_view(gpointer data, const char *query) {
	HexView *view = get_target_view(data);
	if (!view)
		return;
	g_clear_pointer(&view->highlights, g_ptr_array_unref);
	if (query)
		view->highlights = parse_query(query);
	gtk_widget_queue_draw(GTK_WIDGET(view));
}


static void focus_target(gpointer data) {
	focus_hex_view(data);
}


static const FindTarget HEX_FIND_TARGET = {
	find_in_view, count_in_view, replace_in_view, highlight_in_view, focus_target, TEXT_HEX_FIND_PLACEHOLDER,
};


const FindTarget *get_hex_find_target(void) {
	return &HEX_FIND_TARGET;
}
