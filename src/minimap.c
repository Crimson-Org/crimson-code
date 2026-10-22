#include "minimap.h"

#define MINIMAP_KEY "minimap"
/* Lines are read and coloured this many at a time, so a huge file never holds up the window */
#define LINES_PER_STEP 2000
/* A block covers this share of its line's height, leaving a gap that keeps lines apart */
#define BLOCK_HEIGHT_SHARE 0.7
#define BLOCK_ALPHA 0.75
#define SLIDER_ALPHA 0.08
#define SLIDER_ACTIVE_ALPHA 0.16
/* One turn of the wheel over the map scrolls this many lines of the view */
#define WHEEL_LINES 3

/* Width of a character and height of a line in pixels, for each MinimapSize */
static const struct {
	double character_width, line_height;
} SIZES[] = {
	{1.0, 2.0},
	{1.5, 3.0},
	{2.0, 4.0},
};

/* A word drawn as one block, in columns of its line */
typedef struct {
	guint line;
	float start, end;
	guint16 colour;
} Block;

/* The view it pictures, the blocks read from its text and the picture drawn from them */
typedef struct {
	GtkWidget *area;
	GtkSourceView *view;
	GtkSourceBuffer *buffer;
	GtkAdjustment *adjustment;
	MinimapSize size;
	/* Blocks and colours of the finished reading, and those of a reading still going on */
	GArray *blocks, *colours, *new_blocks, *new_colours;
	guint line_count, next_line, read_source;
	gboolean is_reading;
	/* The blocks drawn at the area's size, made again when either changes */
	cairo_surface_t *picture;
	int picture_width, picture_height;
	GdkRGBA background, text;
	/* Where the drag began, as the view's scroll position */
	double drag_start;
	gboolean is_active;
} Minimap;


/* ==================================== Reading the text ==================================== */


static void read_scheme_colours(Minimap *map) {
	map->background = (GdkRGBA){0, 0, 0, 0};
	gtk_widget_get_color(map->area, &map->text);
	GtkSourceStyleScheme *scheme = gtk_source_buffer_get_style_scheme(map->buffer);
	GtkSourceStyle *style = scheme ? gtk_source_style_scheme_get_style(scheme, "text") : NULL;
	if (!style)
		return;
	g_autofree char *background = NULL, *foreground = NULL;
	g_object_get(style, "background", &background, "foreground", &foreground, NULL);
	if (background)
		gdk_rgba_parse(&map->background, background);
	if (foreground)
		gdk_rgba_parse(&map->text, foreground);
}


static guint16 find_colour(GArray *colours, const GdkRGBA *colour) {
	for (guint index = 0; index < colours->len; index++)
		if (gdk_rgba_equal(&g_array_index(colours, GdkRGBA, index), colour))
			return (guint16)index;
	g_array_append_val(colours, *colour);
	return (guint16)(colours->len - 1);
}


/* The tags come lowest priority first, so the last one with a colour is the one the view shows */
static guint16 get_colour_at(Minimap *map, const GtkTextIter *iter) {
	GdkRGBA colour = map->text;
	g_autoptr(GSList) tags = gtk_text_iter_get_tags(iter);
	for (GSList *tag = tags; tag; tag = tag->next) {
		gboolean has_colour = FALSE;
		GdkRGBA *tag_colour = NULL;
		g_object_get(tag->data, "foreground-set", &has_colour, "foreground-rgba", &tag_colour, NULL);
		if (has_colour && tag_colour)
			colour = *tag_colour;
		g_clear_pointer(&tag_colour, gdk_rgba_free);
	}
	return find_colour(map->new_colours, &colour);
}


/* Tabs move to the next tab stop, so blocks line up with the text as the view shows it */
static void read_line(Minimap *map, guint line, guint tab_width) {
	GtkTextIter iter;
	gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(map->buffer), &iter, (int)line);
	guint16 colour = get_colour_at(map, &iter);
	gboolean is_in_word = FALSE;
	Block block = {line, 0, 0, colour};
	for (guint column = 0; !gtk_text_iter_ends_line(&iter); gtk_text_iter_forward_char(&iter)) {
		gunichar character = gtk_text_iter_get_char(&iter);
		if (gtk_text_iter_toggles_tag(&iter, NULL))
			colour = get_colour_at(map, &iter);
		gboolean is_space = g_unichar_isspace(character);
		if (is_in_word && (is_space || colour != block.colour)) {
			block.end = (float)column;
			g_array_append_val(map->new_blocks, block);
			is_in_word = FALSE;
		}
		if (!is_space && !is_in_word) {
			block = (Block){line, (float)column, 0, colour};
			is_in_word = TRUE;
		}
		column = character == '\t' ? (column / tab_width + 1) * tab_width : column + 1;
		block.end = (float)column;
	}
	if (is_in_word)
		g_array_append_val(map->new_blocks, block);
}


static void clear_picture(Minimap *map) {
	g_clear_pointer(&map->picture, cairo_surface_destroy);
	gtk_widget_queue_draw(map->area);
}


/* Highlighting is made sure of first, since the view only colours the lines it has shown */
static gboolean read_next_lines(gpointer data) {
	Minimap *map = data;
	GtkTextBuffer *buffer = GTK_TEXT_BUFFER(map->buffer);
	guint total = (guint)gtk_text_buffer_get_line_count(buffer);
	guint last = MIN(map->next_line + LINES_PER_STEP, total);
	GtkTextIter start, end;
	gtk_text_buffer_get_iter_at_line(buffer, &start, (int)map->next_line);
	gtk_text_buffer_get_iter_at_line(buffer, &end, (int)last);
	if (last == total)
		gtk_text_buffer_get_end_iter(buffer, &end);
	map->is_reading = TRUE;
	gtk_source_buffer_ensure_highlight(map->buffer, &start, &end);
	map->is_reading = FALSE;
	guint tab_width = MAX(gtk_source_view_get_tab_width(map->view), 1);
	for (guint line = map->next_line; line < last; line++)
		read_line(map, line, tab_width);
	map->next_line = last;
	if (last < total)
		return G_SOURCE_CONTINUE;
	map->read_source = 0;
	g_array_unref(map->blocks);
	g_array_unref(map->colours);
	map->blocks = g_steal_pointer(&map->new_blocks);
	map->colours = g_steal_pointer(&map->new_colours);
	map->line_count = total;
	clear_picture(map);
	return G_SOURCE_REMOVE;
}


/* A hidden map reads nothing, and starts over once shown */
static void read_text(Minimap *map) {
	g_clear_handle_id(&map->read_source, g_source_remove);
	g_clear_pointer(&map->new_blocks, g_array_unref);
	g_clear_pointer(&map->new_colours, g_array_unref);
	if (!gtk_widget_get_mapped(map->area))
		return;
	read_scheme_colours(map);
	map->new_blocks = g_array_new(FALSE, FALSE, sizeof(Block));
	map->new_colours = g_array_new(FALSE, FALSE, sizeof(GdkRGBA));
	map->next_line = 0;
	map->read_source = g_idle_add(read_next_lines, map);
}


static void on_text_changed(GtkTextBuffer *buffer, Minimap *map) {
	read_text(map);
}


/* Making sure of highlighting while reading reports those lines too, which needs no new reading */
static void on_highlight_updated(GtkSourceBuffer *buffer, GtkTextIter *start, GtkTextIter *end, Minimap *map) {
	if (!map->is_reading)
		read_text(map);
}


static void on_scheme_changed(GObject *buffer, GParamSpec *property, Minimap *map) {
	read_text(map);
}


static void on_mapped(GtkWidget *area, Minimap *map) {
	read_text(map);
}


/* ==================================== Drawing ==================================== */


/* Lines keep their size while the file fits, and share the height between them when it does not */
static double get_line_height(Minimap *map) {
	double height = SIZES[map->size].line_height;
	if (map->line_count == 0)
		return height;
	return MIN(height, gtk_widget_get_height(map->area) / (double)map->line_count);
}


static void draw_blocks(Minimap *map, cairo_t *cairo) {
	double line_height = get_line_height(map), width = SIZES[map->size].character_width;
	double block_height = line_height >= 2 ? line_height * BLOCK_HEIGHT_SHARE : line_height;
	for (guint index = 0; index < map->blocks->len; index++) {
		Block *block = &g_array_index(map->blocks, Block, index);
		GdkRGBA *colour = &g_array_index(map->colours, GdkRGBA, block->colour);
		cairo_set_source_rgba(cairo, colour->red, colour->green, colour->blue, colour->alpha * BLOCK_ALPHA);
		cairo_rectangle(cairo, block->start * width, block->line * line_height, (block->end - block->start) * width,
			block_height);
		cairo_fill(cairo);
	}
}


/* The blocks are drawn once into a picture, so scrolling only moves the slider over it */
static void draw_picture(Minimap *map, int width, int height) {
	int scale = gtk_widget_get_scale_factor(map->area);
	map->picture = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width * scale, height * scale);
	cairo_surface_set_device_scale(map->picture, scale, scale);
	map->picture_width = width;
	map->picture_height = height;
	cairo_t *cairo = cairo_create(map->picture);
	gdk_cairo_set_source_rgba(cairo, &map->background);
	cairo_paint(cairo);
	draw_blocks(map, cairo);
	cairo_destroy(cairo);
}


/* The slider covers the lines the view shows, as a share of the map's lines */
static void get_slider(Minimap *map, double *top, double *height) {
	double content = get_line_height(map) * map->line_count;
	double upper = gtk_adjustment_get_upper(map->adjustment);
	*top = upper > 0 ? gtk_adjustment_get_value(map->adjustment) / upper * content : 0;
	*height = upper > 0 ? gtk_adjustment_get_page_size(map->adjustment) / upper * content : 0;
}


static void draw_map(GtkDrawingArea *area, cairo_t *cairo, int width, int height, gpointer data) {
	Minimap *map = data;
	if (!map->picture || map->picture_width != width || map->picture_height != height)
		g_clear_pointer(&map->picture, cairo_surface_destroy);
	if (!map->picture)
		draw_picture(map, width, height);
	cairo_set_source_surface(cairo, map->picture, 0, 0);
	cairo_paint(cairo);
	double top = 0, slider_height = 0;
	get_slider(map, &top, &slider_height);
	GdkRGBA colour = map->text;
	colour.alpha = map->is_active ? SLIDER_ACTIVE_ALPHA : SLIDER_ALPHA;
	gdk_cairo_set_source_rgba(cairo, &colour);
	cairo_rectangle(cairo, 0, top, width, slider_height);
	cairo_fill(cairo);
}


static void on_view_scrolled(GtkAdjustment *adjustment, Minimap *map) {
	gtk_widget_queue_draw(map->area);
}


/* The map is as wide as the view's right margin, at the size's character width */
static void update_width(Minimap *map) {
	guint columns = gtk_source_view_get_right_margin_position(map->view);
	gtk_widget_set_size_request(map->area, (int)(columns * SIZES[map->size].character_width), -1);
}


static void on_margin_changed(GObject *view, GParamSpec *property, Minimap *map) {
	update_width(map);
}


/* ==================================== Pointer ==================================== */


/* A press beside the slider centres the view there, and a drag then moves it as far as the pointer goes */
static void on_drag_begin(GtkGestureDrag *drag, double x, double y, Minimap *map) {
	double top = 0, height = 0;
	get_slider(map, &top, &height);
	double content = get_line_height(map) * map->line_count;
	if ((y < top || y > top + height) && content > 0) {
		double upper = gtk_adjustment_get_upper(map->adjustment);
		gtk_adjustment_set_value(map->adjustment, y / content * upper - gtk_adjustment_get_page_size(map->adjustment) / 2);
	}
	map->drag_start = gtk_adjustment_get_value(map->adjustment);
	map->is_active = TRUE;
	gtk_widget_queue_draw(map->area);
}


static void on_drag_update(GtkGestureDrag *drag, double x, double y, Minimap *map) {
	double content = get_line_height(map) * map->line_count;
	if (content > 0)
		gtk_adjustment_set_value(map->adjustment, map->drag_start + y / content * gtk_adjustment_get_upper(map->adjustment));
}


static void on_drag_end(GtkGestureDrag *drag, double x, double y, Minimap *map) {
	map->is_active = FALSE;
	gtk_widget_queue_draw(map->area);
}


static gboolean on_wheel(GtkEventControllerScroll *scroll, double x, double y, Minimap *map) {
	double line = map->line_count > 0 ? gtk_adjustment_get_upper(map->adjustment) / map->line_count : 0;
	gtk_adjustment_set_value(map->adjustment, gtk_adjustment_get_value(map->adjustment) + y * line * WHEEL_LINES);
	return GDK_EVENT_STOP;
}


/* ==================================== Creating ==================================== */


static void release_sources(Minimap *map) {
	if (!map->view)
		return;
	g_clear_handle_id(&map->read_source, g_source_remove);
	g_clear_pointer(&map->new_blocks, g_array_unref);
	g_clear_pointer(&map->new_colours, g_array_unref);
	g_signal_handlers_disconnect_by_data(map->buffer, map);
	g_signal_handlers_disconnect_by_data(map->adjustment, map);
	g_signal_handlers_disconnect_by_data(map->view, map);
	g_clear_object(&map->buffer);
	g_clear_object(&map->adjustment);
	g_clear_object(&map->view);
}


/* GLib has already removed the data from the widget when this runs, so it must not look it up again */
static void free_minimap(gpointer data) {
	Minimap *map = data;
	release_sources(map);
	g_array_unref(map->blocks);
	g_array_unref(map->colours);
	g_clear_pointer(&map->picture, cairo_surface_destroy);
	g_free(map);
}


static void add_pointer_controllers(Minimap *map) {
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), map);
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), map);
	g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), map);
	gtk_widget_add_controller(map->area, GTK_EVENT_CONTROLLER(drag));
	GtkEventController *wheel = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	g_signal_connect(wheel, "scroll", G_CALLBACK(on_wheel), map);
	gtk_widget_add_controller(map->area, wheel);
}


GtkWidget *create_minimap(GtkSourceView *view) {
	Minimap *map = g_new0(Minimap, 1);
	map->view = g_object_ref(view);
	map->buffer = GTK_SOURCE_BUFFER(g_object_ref(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view))));
	map->adjustment = g_object_ref(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view)));
	map->blocks = g_array_new(FALSE, FALSE, sizeof(Block));
	map->colours = g_array_new(FALSE, FALSE, sizeof(GdkRGBA));
	map->area = gtk_drawing_area_new();
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(map->area), draw_map, map, NULL);
	gtk_widget_set_vexpand(map->area, TRUE);
	gtk_widget_add_css_class(map->area, "minimap");
	add_pointer_controllers(map);
	g_signal_connect(map->area, "map", G_CALLBACK(on_mapped), map);
	g_signal_connect(map->buffer, "changed", G_CALLBACK(on_text_changed), map);
	g_signal_connect(map->buffer, "highlight-updated", G_CALLBACK(on_highlight_updated), map);
	g_signal_connect(map->buffer, "notify::style-scheme", G_CALLBACK(on_scheme_changed), map);
	g_signal_connect(map->adjustment, "value-changed", G_CALLBACK(on_view_scrolled), map);
	g_signal_connect(map->adjustment, "changed", G_CALLBACK(on_view_scrolled), map);
	g_signal_connect(map->view, "notify::right-margin-position", G_CALLBACK(on_margin_changed), map);
	g_object_set_data_full(G_OBJECT(map->area), MINIMAP_KEY, map, free_minimap);
	update_width(map);
	return map->area;
}


void set_minimap_size(GtkWidget *minimap, MinimapSize size) {
	Minimap *map = g_object_get_data(G_OBJECT(minimap), MINIMAP_KEY);
	map->size = size;
	update_width(map);
	clear_picture(map);
}


void disconnect_minimap(GtkWidget *minimap) {
	release_sources(g_object_get_data(G_OBJECT(minimap), MINIMAP_KEY));
}
