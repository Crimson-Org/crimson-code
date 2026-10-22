#define _GNU_SOURCE

#include <errno.h>
#include <pty.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <adwaita.h>
#include <glib-unix.h>
#include <vterm.h>

#include "../lang/en.h"
#include "settings.h"
#include "terminal.h"

#define READ_BUFFER_BYTES 65536
#define WHEEL_LINES 3
#define MINIMUM_COLUMNS 2
#define MINIMUM_ROWS 1
#define SAMPLE_TEXT "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
#define PADDING 6
#define CONTINUATION_CELL ((uint32_t)-1)
/* Bold text stays this much heavier than the chosen font weight */
#define BOLD_WEIGHT_STEP 300

/* One line that scrolled off the top, kept so it can be scrolled back to */
typedef struct {
	int columns;
	VTermScreenCell cells[];
} ScrollbackLine;

/* Bit flags for the four font styles a cell can use */
enum {
	STYLE_NORMAL = 0,
	STYLE_BOLD = 1,
	STYLE_ITALIC = 2,
	STYLE_COUNT = 4,
};

/* A character shaped once, with the font Pango picked for it */
typedef struct {
	PangoFont *font;
	PangoGlyphString *glyphs;
} CachedGlyph;

/* Glyphs of one font and colour waiting to become a single text node */
typedef struct {
	GArray *glyphs;
	PangoFont *font;
	GdkRGBA colour;
} GlyphRun;

/* A cell position counting scrollback lines first and then the screen rows */
typedef struct {
	int row, column;
} BufferPosition;

struct _Terminal {
	GtkWidget parent_instance;
	VTerm *vterm;
	VTermScreen *screen;
	int pty, rows, columns;
	GPid child;
	guint read_source, child_watch;
	int cell_width, cell_height, ascent;
	PangoLayout *layouts[STYLE_COUNT];
	GHashTable *glyphs;
	GPtrArray *scrollback;
	int scroll_offset;
	VTermPos cursor;
	gboolean is_cursor_visible, is_alternate_screen, is_focused, has_ended;
	char *directory;
	GtkIMContext *input_method;
	gboolean has_selection;
	BufferPosition selection_anchor, selection_end;
};

#define TERMINAL_TYPE (terminal_get_type())
G_DECLARE_FINAL_TYPE(Terminal, terminal, CRIMSON, TERMINAL, GtkWidget)
G_DEFINE_FINAL_TYPE(Terminal, terminal, GTK_TYPE_WIDGET)

/* Keys that terminals send as escape sequences instead of text */
static const struct {
	guint key;
	VTermKey terminal_key;
} SPECIAL_KEYS[] = {
	{GDK_KEY_Return, VTERM_KEY_ENTER}, {GDK_KEY_KP_Enter, VTERM_KEY_ENTER}, {GDK_KEY_Tab, VTERM_KEY_TAB},
	{GDK_KEY_ISO_Left_Tab, VTERM_KEY_TAB}, {GDK_KEY_BackSpace, VTERM_KEY_BACKSPACE},
	{GDK_KEY_Escape, VTERM_KEY_ESCAPE}, {GDK_KEY_Up, VTERM_KEY_UP}, {GDK_KEY_Down, VTERM_KEY_DOWN},
	{GDK_KEY_Left, VTERM_KEY_LEFT}, {GDK_KEY_Right, VTERM_KEY_RIGHT}, {GDK_KEY_Insert, VTERM_KEY_INS},
	{GDK_KEY_Delete, VTERM_KEY_DEL}, {GDK_KEY_Home, VTERM_KEY_HOME}, {GDK_KEY_End, VTERM_KEY_END},
	{GDK_KEY_Page_Up, VTERM_KEY_PAGEUP}, {GDK_KEY_Page_Down, VTERM_KEY_PAGEDOWN},
	{GDK_KEY_KP_Up, VTERM_KEY_UP}, {GDK_KEY_KP_Down, VTERM_KEY_DOWN}, {GDK_KEY_KP_Left, VTERM_KEY_LEFT},
	{GDK_KEY_KP_Right, VTERM_KEY_RIGHT}, {GDK_KEY_KP_Home, VTERM_KEY_HOME}, {GDK_KEY_KP_End, VTERM_KEY_END},
	{GDK_KEY_KP_Delete, VTERM_KEY_DEL},
};

static const SettingDefinition TERMINAL_SETTINGS[] = {
	{"terminal-shell", "terminal", TEXT_GROUP_SHELL, TEXT_SETTING_SHELL, TEXT_SETTING_SHELL_DETAIL, SETTING_TEXT, "",
		0, 0, NULL, NULL, {NULL}},
	{"terminal-text-size", "terminal", TEXT_GROUP_TERMINAL_DISPLAY, TEXT_SETTING_TERMINAL_SIZE,
		TEXT_SETTING_TERMINAL_SIZE_DETAIL, SETTING_NUMBER, "0", 0, 48, NULL, NULL, {NULL}},
	{"terminal-scrollback", "terminal", TEXT_GROUP_TERMINAL_DISPLAY, TEXT_SETTING_SCROLLBACK, NULL, SETTING_NUMBER,
		"10000", 100, 1000000, NULL, NULL, {NULL}},
	{"copy-on-select", "terminal", TEXT_GROUP_SELECTION, TEXT_SETTING_COPY_ON_SELECT,
		TEXT_SETTING_COPY_ON_SELECT_DETAIL, SETTING_SWITCH, "true", 0, 0, NULL, NULL, {NULL}},
};

static GtkCssProvider *terminal_style;

/* Backgrounds matching the libadwaita view colour in each style, used until a theme gives one */
static const GdkRGBA DARK_BACKGROUND = {0.118, 0.118, 0.118, 1};
static const GdkRGBA LIGHT_BACKGROUND = {1, 1, 1, 1};
static GdkRGBA theme_background;
static gboolean has_theme_background;


/* ==================================== Cells ==================================== */


static int get_scrollback_length(Terminal *terminal) {
	return (int)terminal->scrollback->len;
}


static void get_blank_cell(VTermScreenCell *cell) {
	*cell = (VTermScreenCell){0};
	cell->width = 1;
	cell->fg.type = VTERM_COLOR_DEFAULT_FG;
	cell->bg.type = VTERM_COLOR_DEFAULT_BG;
}


static void get_cell(Terminal *terminal, int row, int column, VTermScreenCell *cell) {
	int scrollback = get_scrollback_length(terminal);
	if (row >= scrollback) {
		vterm_screen_get_cell(terminal->screen, (VTermPos){row - scrollback, column}, cell);
		return;
	}
	ScrollbackLine *line = row >= 0 ? g_ptr_array_index(terminal->scrollback, row) : NULL;
	if (line && column < line->columns)
		*cell = line->cells[column];
	else
		get_blank_cell(cell);
}


static void get_colour(Terminal *terminal, VTermColor colour, gboolean is_foreground, GdkRGBA *result) {
	if (is_foreground ? VTERM_COLOR_IS_DEFAULT_FG(&colour) : VTERM_COLOR_IS_DEFAULT_BG(&colour)) {
		if (is_foreground)
			gtk_widget_get_color(GTK_WIDGET(terminal), result);
		else
			*result = has_theme_background ? theme_background
				: adw_style_manager_get_dark(adw_style_manager_get_default()) ? DARK_BACKGROUND : LIGHT_BACKGROUND;
		return;
	}
	vterm_screen_convert_color_to_rgb(terminal->screen, &colour);
	*result = (GdkRGBA){colour.rgb.red / 255.0f, colour.rgb.green / 255.0f, colour.rgb.blue / 255.0f, 1};
}


static void append_cell_text(GString *text, const VTermScreenCell *cell) {
	if (cell->chars[0] == CONTINUATION_CELL)
		return;
	if (cell->chars[0] == 0) {
		g_string_append_c(text, ' ');
		return;
	}
	for (int index = 0; index < VTERM_MAX_CHARS_PER_CELL && cell->chars[index]; index++)
		g_string_append_unichar(text, cell->chars[index]);
}


/* ==================================== Selection ==================================== */


static void order_selection(Terminal *terminal, BufferPosition *start, BufferPosition *end) {
	BufferPosition first = terminal->selection_anchor, second = terminal->selection_end;
	gboolean is_reversed = second.row < first.row || (second.row == first.row && second.column < first.column);
	*start = is_reversed ? second : first;
	*end = is_reversed ? first : second;
}


static gboolean is_selected(Terminal *terminal, int row, int column) {
	if (!terminal->has_selection)
		return FALSE;
	BufferPosition start, end;
	order_selection(terminal, &start, &end);
	if (row < start.row || row > end.row)
		return FALSE;
	return (row != start.row || column >= start.column) && (row != end.row || column <= end.column);
}


static char *get_selected_text(Terminal *terminal) {
	BufferPosition start, end;
	order_selection(terminal, &start, &end);
	GString *text = g_string_new(NULL);
	for (int row = start.row; row <= end.row; row++) {
		gsize line_start = text->len;
		int first = row == start.row ? start.column : 0;
		int last = row == end.row ? end.column : terminal->columns - 1;
		for (int column = first; column <= last; column++) {
			VTermScreenCell cell;
			get_cell(terminal, row, column, &cell);
			append_cell_text(text, &cell);
		}
		while (text->len > line_start && text->str[text->len - 1] == ' ')
			g_string_truncate(text, text->len - 1);
		if (row != end.row)
			g_string_append_c(text, '\n');
	}
	return g_string_free(text, FALSE);
}


static BufferPosition get_position_at(Terminal *terminal, double x, double y) {
	int row = (int)((y - PADDING) / terminal->cell_height);
	int column = (int)((x - PADDING) / terminal->cell_width);
	row = CLAMP(row, 0, terminal->rows - 1);
	column = CLAMP(column, 0, terminal->columns - 1);
	return (BufferPosition){get_scrollback_length(terminal) - terminal->scroll_offset + row, column};
}


/* ==================================== Fonts and glyphs ==================================== */


static void clear_fonts(Terminal *terminal) {
	for (int style = 0; style < STYLE_COUNT; style++)
		g_clear_object(&terminal->layouts[style]);
	g_hash_table_remove_all(terminal->glyphs);
	terminal->cell_width = 0;
}


static void free_cached_glyph(CachedGlyph *glyph) {
	g_clear_object(&glyph->font);
	g_clear_pointer(&glyph->glyphs, pango_glyph_string_free);
	g_free(glyph);
}


/* Cell size follows VTE, measuring each printable ASCII character so a proportional font cannot overlap */
static void measure_cells(Terminal *terminal) {
	PangoContext *context = gtk_widget_get_pango_context(GTK_WIDGET(terminal));
	const PangoFontDescription *base = pango_context_get_font_description(context);
	for (int style = 0; style < STYLE_COUNT; style++) {
		g_autoptr(PangoFontDescription) description = pango_font_description_copy(base);
		if (style & STYLE_BOLD)
			pango_font_description_set_weight(description, MIN(PANGO_WEIGHT_ULTRAHEAVY,
				MAX(PANGO_WEIGHT_BOLD, pango_font_description_get_weight(base) + BOLD_WEIGHT_STEP)));
		if (style & STYLE_ITALIC)
			pango_font_description_set_style(description, PANGO_STYLE_ITALIC);
		terminal->layouts[style] = pango_layout_new(context);
		pango_layout_set_font_description(terminal->layouts[style], description);
	}
	PangoLayout *layout = terminal->layouts[STYLE_NORMAL];
	int width = 1, height = 1;
	for (char character = 0x21; character < 0x7f; character++) {
		PangoRectangle logical;
		pango_layout_set_text(layout, &character, 1);
		pango_layout_get_extents(layout, NULL, &logical);
		width = MAX(width, PANGO_PIXELS_CEIL(logical.width));
		height = MAX(height, PANGO_PIXELS_CEIL(logical.height));
	}
	pango_layout_set_text(layout, SAMPLE_TEXT, -1);
	terminal->ascent = PANGO_PIXELS_CEIL(pango_layout_get_baseline(layout));
	/* The font's own metrics are better unless they report less height than measured, which cuts letters */
	g_autoptr(PangoFontMetrics) metrics = pango_context_get_metrics(context, NULL, NULL);
	int metrics_ascent = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(metrics));
	int metrics_height = PANGO_PIXELS_CEIL(pango_font_metrics_get_height(metrics));
	if (metrics_ascent > 0 && metrics_height >= height) {
		terminal->ascent = metrics_ascent;
		height = metrics_height;
	}
	terminal->cell_width = width;
	terminal->cell_height = height;
}


/* Shapes each character once per style and keeps the glyphs, so drawing a frame never shapes text */
static CachedGlyph *get_glyph(Terminal *terminal, const char *text, int style) {
	g_autofree char *key = g_strdup_printf("%d%s", style, text);
	CachedGlyph *glyph = g_hash_table_lookup(terminal->glyphs, key);
	if (glyph)
		return glyph;
	glyph = g_new0(CachedGlyph, 1);
	PangoLayout *layout = terminal->layouts[style];
	pango_layout_set_text(layout, text, -1);
	PangoLayoutLine *line = pango_layout_get_line_readonly(layout, 0);
	if (line && line->runs) {
		PangoGlyphItem *item = line->runs->data;
		glyph->font = item->item->analysis.font ? g_object_ref(item->item->analysis.font) : NULL;
		glyph->glyphs = pango_glyph_string_copy(item->glyphs);
	}
	g_hash_table_insert(terminal->glyphs, g_steal_pointer(&key), glyph);
	return glyph;
}


/* ==================================== Drawing ==================================== */


static int get_style(const VTermScreenCell *cell) {
	return (cell->attrs.bold ? STYLE_BOLD : 0) | (cell->attrs.italic ? STYLE_ITALIC : 0);
}


static void get_cell_colours(Terminal *terminal, const VTermScreenCell *cell, GdkRGBA *foreground,
	GdkRGBA *background) {
	get_colour(terminal, cell->fg, TRUE, foreground);
	get_colour(terminal, cell->bg, FALSE, background);
	if (!cell->attrs.reverse)
		return;
	GdkRGBA swapped = *foreground;
	*foreground = *background;
	*background = swapped;
}


/* Turns the collected glyphs into one text node, with positions made relative as VTE does for speed */
static void flush_glyphs(GtkSnapshot *snapshot, GlyphRun *run, double y) {
	if (run->glyphs->len == 0 || !run->font) {
		g_array_set_size(run->glyphs, 0);
		return;
	}
	PangoGlyphInfo *glyphs = (PangoGlyphInfo *)(gpointer)run->glyphs->data;
	int x = glyphs[0].geometry.x_offset;
	graphene_point_t offset = GRAPHENE_POINT_INIT((float)x / PANGO_SCALE, (float)y);
	x += glyphs[0].geometry.width;
	glyphs[0].geometry.x_offset = 0;
	for (guint index = 1; index < run->glyphs->len; index++) {
		glyphs[index].geometry.x_offset -= x;
		x += glyphs[index].geometry.width;
	}
	PangoGlyphString string = {(int)run->glyphs->len, glyphs, NULL, 0};
	GskRenderNode *node = gsk_text_node_new(run->font, &string, &run->colour, &offset);
	if (node) {
		gtk_snapshot_append_node(snapshot, node);
		gsk_render_node_unref(node);
	}
	g_array_set_size(run->glyphs, 0);
}


static void add_cell_glyphs(Terminal *terminal, GtkSnapshot *snapshot, GlyphRun *run, const VTermScreenCell *cell,
	int column, const GdkRGBA *colour, double y) {
	if (cell->chars[0] == 0 || cell->chars[0] == CONTINUATION_CELL || cell->chars[0] == ' ' || cell->attrs.conceal)
		return;
	g_autoptr(GString) text = g_string_new(NULL);
	append_cell_text(text, cell);
	CachedGlyph *glyph = get_glyph(terminal, text->str, get_style(cell));
	if (!glyph->glyphs)
		return;
	if (glyph->font != run->font || !gdk_rgba_equal(colour, &run->colour)) {
		flush_glyphs(snapshot, run, y);
		run->font = glyph->font;
		run->colour = *colour;
	}
	int x = (int)(PADDING + column * terminal->cell_width);
	for (int index = 0; index < glyph->glyphs->num_glyphs; index++) {
		PangoGlyphInfo info = glyph->glyphs->glyphs[index];
		info.geometry.x_offset += x * PANGO_SCALE;
		x += info.geometry.width / PANGO_SCALE;
		g_array_append_val(run->glyphs, info);
	}
}


static void draw_cell_lines(Terminal *terminal, GtkSnapshot *snapshot, const VTermScreenCell *cell, int column,
	const GdkRGBA *colour, double top) {
	double x = PADDING + column * terminal->cell_width;
	if (cell->attrs.underline)
		gtk_snapshot_append_color(snapshot, colour, &GRAPHENE_RECT_INIT(x, top + terminal->ascent + 1,
			terminal->cell_width, 1));
	if (cell->attrs.strike)
		gtk_snapshot_append_color(snapshot, colour, &GRAPHENE_RECT_INIT(x, top + terminal->ascent / 2.0 + 1,
			terminal->cell_width, 1));
}


/* Draws backgrounds as merged rectangles first, then all text of the row locked to the cell grid */
static void draw_row(Terminal *terminal, GtkSnapshot *snapshot, GlyphRun *run, int row, int screen_row) {
	double top = PADDING + screen_row * terminal->cell_height;
	int run_start = 0;
	GdkRGBA run_background = {0, 0, 0, 0}, foreground, background;
	gboolean has_background = FALSE;
	for (int column = 0; column <= terminal->columns; column++) {
		VTermScreenCell cell;
		gboolean has_cell_background = FALSE;
		if (column < terminal->columns) {
			get_cell(terminal, row, column, &cell);
			get_cell_colours(terminal, &cell, &foreground, &background);
			has_cell_background = cell.attrs.reverse || !VTERM_COLOR_IS_DEFAULT_BG(&cell.bg);
			if (cell.chars[0] == CONTINUATION_CELL || (has_cell_background == has_background
				&& (!has_background || gdk_rgba_equal(&background, &run_background))))
				continue;
		}
		if (has_background)
			gtk_snapshot_append_color(snapshot, &run_background, &GRAPHENE_RECT_INIT(PADDING + run_start
				* terminal->cell_width, top, (column - run_start) * terminal->cell_width, terminal->cell_height));
		run_start = column;
		run_background = background;
		has_background = has_cell_background;
	}
	for (int column = 0; column < terminal->columns; column++) {
		VTermScreenCell cell;
		get_cell(terminal, row, column, &cell);
		get_cell_colours(terminal, &cell, &foreground, &background);
		add_cell_glyphs(terminal, snapshot, run, &cell, column, &foreground, top + terminal->ascent);
		draw_cell_lines(terminal, snapshot, &cell, column, &foreground, top);
	}
	flush_glyphs(snapshot, run, top + terminal->ascent);
}


static void draw_selection(Terminal *terminal, GtkSnapshot *snapshot, int top) {
	GdkRGBA selection = {0.21f, 0.52f, 0.89f, 0.35f};
	for (int screen_row = 0; screen_row < terminal->rows && terminal->has_selection; screen_row++)
		for (int column = 0; column < terminal->columns; column++)
			if (is_selected(terminal, top + screen_row, column))
				gtk_snapshot_append_color(snapshot, &selection, &GRAPHENE_RECT_INIT(PADDING + column
					* terminal->cell_width, PADDING + screen_row * terminal->cell_height, terminal->cell_width,
					terminal->cell_height));
}


/* A solid block with the character drawn over it in the background colour, or an outline when unfocused */
static void draw_cursor(Terminal *terminal, GtkSnapshot *snapshot, GlyphRun *run) {
	if (!terminal->is_cursor_visible || terminal->scroll_offset > 0 || terminal->has_ended)
		return;
	VTermScreenCell cell;
	vterm_screen_get_cell(terminal->screen, terminal->cursor, &cell);
	GdkRGBA foreground, background;
	get_cell_colours(terminal, &cell, &foreground, &background);
	double x = PADDING + terminal->cursor.col * terminal->cell_width;
	double y = PADDING + terminal->cursor.row * terminal->cell_height;
	double width = terminal->cell_width * MAX(1, cell.width);
	graphene_rect_t area = GRAPHENE_RECT_INIT(x, y, width, terminal->cell_height);
	if (!terminal->is_focused) {
		GskRoundedRect outline;
		gsk_rounded_rect_init_from_rect(&outline, &area, 0);
		const float widths[4] = {1, 1, 1, 1};
		const GdkRGBA colours[4] = {foreground, foreground, foreground, foreground};
		gtk_snapshot_append_border(snapshot, &outline, widths, colours);
		return;
	}
	gtk_snapshot_append_color(snapshot, &foreground, &area);
	add_cell_glyphs(terminal, snapshot, run, &cell, terminal->cursor.col, &background, y + terminal->ascent);
	flush_glyphs(snapshot, run, y + terminal->ascent);
}


static void terminal_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
	Terminal *terminal = CRIMSON_TERMINAL(widget);
	GdkRGBA background;
	get_colour(terminal, (VTermColor){.type = VTERM_COLOR_DEFAULT_BG}, FALSE, &background);
	gtk_snapshot_append_color(snapshot, &background, &GRAPHENE_RECT_INIT(0, 0, gtk_widget_get_width(widget),
		gtk_widget_get_height(widget)));
	if (terminal->cell_width <= 0)
		return;
	GlyphRun run = {g_array_new(FALSE, FALSE, sizeof(PangoGlyphInfo)), NULL, {0, 0, 0, 0}};
	int top = get_scrollback_length(terminal) - terminal->scroll_offset;
	for (int screen_row = 0; screen_row < terminal->rows; screen_row++)
		draw_row(terminal, snapshot, &run, top + screen_row, screen_row);
	draw_selection(terminal, snapshot, top);
	draw_cursor(terminal, snapshot, &run);
	g_array_unref(run.glyphs);
}


/* ==================================== Screen events ==================================== */


static int on_damage(VTermRect rectangle, void *data) {
	gtk_widget_queue_draw(GTK_WIDGET(data));
	return 1;
}


static int on_cursor_moved(VTermPos position, VTermPos old_position, int is_visible, void *data) {
	Terminal *terminal = data;
	terminal->cursor = position;
	terminal->is_cursor_visible = is_visible;
	gtk_widget_queue_draw(GTK_WIDGET(terminal));
	return 1;
}


static int on_property_changed(VTermProp property, VTermValue *value, void *data) {
	Terminal *terminal = data;
	if (property == VTERM_PROP_ALTSCREEN)
		terminal->is_alternate_screen = value->boolean;
	else if (property == VTERM_PROP_CURSORVISIBLE)
		terminal->is_cursor_visible = value->boolean;
	return 1;
}


static int on_line_pushed(int columns, const VTermScreenCell *cells, void *data) {
	Terminal *terminal = data;
	ScrollbackLine *line = g_malloc(sizeof(ScrollbackLine) + columns * sizeof(VTermScreenCell));
	line->columns = columns;
	memcpy(line->cells, cells, columns * sizeof(VTermScreenCell));
	g_ptr_array_add(terminal->scrollback, line);
	guint maximum = (guint)get_number_setting("terminal-scrollback");
	if (terminal->scrollback->len > maximum) {
		g_ptr_array_remove_range(terminal->scrollback, 0, terminal->scrollback->len - maximum);
		terminal->has_selection = FALSE;
	}
	if (terminal->scroll_offset > 0)
		terminal->scroll_offset = MIN(terminal->scroll_offset + 1, get_scrollback_length(terminal));
	return 1;
}


/* Gives a scrolled off line back to the screen when the terminal grows taller */
static int on_line_popped(int columns, VTermScreenCell *cells, void *data) {
	Terminal *terminal = data;
	if (terminal->scrollback->len == 0)
		return 0;
	ScrollbackLine *line = g_ptr_array_steal_index(terminal->scrollback, terminal->scrollback->len - 1);
	for (int column = 0; column < columns; column++) {
		if (column < line->columns)
			cells[column] = line->cells[column];
		else
			get_blank_cell(&cells[column]);
	}
	g_free(line);
	terminal->scroll_offset = MIN(terminal->scroll_offset, get_scrollback_length(terminal));
	return 1;
}


static int on_scrollback_cleared(void *data) {
	Terminal *terminal = data;
	g_ptr_array_set_size(terminal->scrollback, 0);
	terminal->scroll_offset = 0;
	terminal->has_selection = FALSE;
	return 1;
}


static const VTermScreenCallbacks SCREEN_CALLBACKS = {
	.damage = on_damage,
	.movecursor = on_cursor_moved,
	.settermprop = on_property_changed,
	.sb_pushline = on_line_pushed,
	.sb_popline = on_line_popped,
	.sb_clear = on_scrollback_cleared,
};


/* ==================================== Shell process ==================================== */


static void write_to_shell(const char *bytes, size_t length, void *data) {
	Terminal *terminal = data;
	while (length > 0 && terminal->pty >= 0) {
		ssize_t written = write(terminal->pty, bytes, length);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return;
		bytes += written;
		length -= (size_t)written;
	}
}


static gboolean on_shell_output(int descriptor, GIOCondition condition, gpointer data) {
	Terminal *terminal = data;
	char buffer[READ_BUFFER_BYTES];
	ssize_t length = read(descriptor, buffer, sizeof buffer);
	if (length <= 0) {
		terminal->read_source = 0;
		return G_SOURCE_REMOVE;
	}
	vterm_input_write(terminal->vterm, buffer, (size_t)length);
	vterm_screen_flush_damage(terminal->screen);
	return G_SOURCE_CONTINUE;
}


static void on_shell_ended(GPid child, int status, gpointer data) {
	Terminal *terminal = data;
	terminal->child_watch = 0;
	terminal->child = 0;
	g_clear_handle_id(&terminal->read_source, g_source_remove);
	if (terminal->pty >= 0)
		close(terminal->pty);
	terminal->pty = -1;
	terminal->has_ended = TRUE;
	g_autofree char *notice = g_strdup_printf("\r\n\x1b[2m%s\x1b[0m\r\n", TEXT_TERMINAL_ENDED);
	vterm_input_write(terminal->vterm, notice, strlen(notice));
	vterm_screen_flush_damage(terminal->screen);
	g_spawn_check_wait_status(status, NULL);
	g_spawn_close_pid(child);
}


/* Everything the child needs is prepared before forking, since only plain system calls are safe after */
static void start_shell(Terminal *terminal) {
	const char *command = get_text_setting("terminal-shell");
	if (!*command)
		command = g_getenv("SHELL");
	g_auto(GStrv) arguments = NULL;
	if (!command || !*command || !g_shell_parse_argv(command, NULL, &arguments, NULL))
		arguments = g_strdupv((char *[]){"/bin/sh", NULL});
	g_autofree char *shell = g_path_is_absolute(arguments[0]) ? g_strdup(arguments[0])
		: g_find_program_in_path(arguments[0]);
	if (!shell) {
		g_autofree char *notice = g_strdup_printf("\r\n%s\r\n", TEXT_TERMINAL_NO_SHELL);
		vterm_input_write(terminal->vterm, notice, strlen(notice));
		terminal->has_ended = TRUE;
		return;
	}
	g_auto(GStrv) environment = g_get_environ();
	environment = g_environ_setenv(environment, "TERM", "xterm-256color", TRUE);
	environment = g_environ_setenv(environment, "COLORTERM", "truecolor", TRUE);
	environment = g_environ_unsetenv(environment, "COLUMNS");
	environment = g_environ_unsetenv(environment, "LINES");
	struct winsize size = {(unsigned short)terminal->rows, (unsigned short)terminal->columns, 0, 0};
	int pty = -1;
	pid_t child = forkpty(&pty, NULL, NULL, &size);
	if (child == 0) {
		/* Pipes to language servers and other open files must not leak into the shell */
		close_range(3, ~0u, 0);
		if (terminal->directory && chdir(terminal->directory) != 0)
			_exit(1);
		execve(shell, arguments, environment);
		_exit(127);
	}
	if (child < 0) {
		g_warning("The terminal shell did not start: %s", g_strerror(errno));
		return;
	}
	terminal->pty = pty;
	terminal->child = child;
	terminal->has_ended = FALSE;
	terminal->read_source = g_unix_fd_add(pty, G_IO_IN | G_IO_HUP | G_IO_ERR, on_shell_output, terminal);
	terminal->child_watch = g_child_watch_add(child, on_shell_ended, terminal);
}


static void resize_screen(Terminal *terminal, int rows, int columns) {
	if (rows == terminal->rows && columns == terminal->columns)
		return;
	terminal->rows = rows;
	terminal->columns = columns;
	vterm_set_size(terminal->vterm, rows, columns);
	vterm_screen_flush_damage(terminal->screen);
	struct winsize size = {(unsigned short)rows, (unsigned short)columns, 0, 0};
	if (terminal->pty >= 0)
		ioctl(terminal->pty, TIOCSWINSZ, &size);
}


/* ==================================== Input ==================================== */


static VTermModifier get_modifiers(GdkModifierType state) {
	int modifiers = VTERM_MOD_NONE;
	if (state & GDK_SHIFT_MASK)
		modifiers |= VTERM_MOD_SHIFT;
	if (state & GDK_ALT_MASK)
		modifiers |= VTERM_MOD_ALT;
	if (state & GDK_CONTROL_MASK)
		modifiers |= VTERM_MOD_CTRL;
	return (VTermModifier)modifiers;
}


static void restart_if_ended(Terminal *terminal) {
	if (!terminal->has_ended)
		return;
	vterm_screen_reset(terminal->screen, 1);
	start_shell(terminal);
}


static void on_text_committed(GtkIMContext *input_method, const char *text, Terminal *terminal) {
	restart_if_ended(terminal);
	terminal->scroll_offset = 0;
	for (const char *character = text; *character; character = g_utf8_next_char(character))
		vterm_keyboard_unichar(terminal->vterm, g_utf8_get_char(character), VTERM_MOD_NONE);
}


static void on_text_pasted(GObject *clipboard, GAsyncResult *result, gpointer data) {
	Terminal *terminal = data;
	g_autofree char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(clipboard), result, NULL);
	if (text && terminal->vterm) {
		vterm_keyboard_start_paste(terminal->vterm);
		for (const char *character = text; *character; character = g_utf8_next_char(character)) {
			gunichar unicode = g_utf8_get_char(character);
			if (unicode == '\r' && character[1] == '\n')
				continue;
			vterm_keyboard_unichar(terminal->vterm, unicode == '\n' ? '\r' : unicode, VTERM_MOD_NONE);
		}
		vterm_keyboard_end_paste(terminal->vterm);
	}
	g_object_unref(terminal);
}


static gboolean handle_clipboard_key(Terminal *terminal, guint key, GdkModifierType state) {
	if ((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) != (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
		return FALSE;
	GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(terminal));
	if ((key == GDK_KEY_C || key == GDK_KEY_c) && terminal->has_selection) {
		g_autofree char *text = get_selected_text(terminal);
		gdk_clipboard_set_text(clipboard, text);
		return TRUE;
	}
	if (key == GDK_KEY_V || key == GDK_KEY_v) {
		gdk_clipboard_read_text_async(clipboard, NULL, on_text_pasted, g_object_ref(terminal));
		return TRUE;
	}
	return FALSE;
}


static gboolean on_key_pressed(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType state,
	Terminal *terminal) {
	if (handle_clipboard_key(terminal, key, state))
		return TRUE;
	if ((state & GDK_SHIFT_MASK) && (key == GDK_KEY_Page_Up || key == GDK_KEY_Page_Down)) {
		int step = key == GDK_KEY_Page_Up ? terminal->rows : -terminal->rows;
		terminal->scroll_offset = CLAMP(terminal->scroll_offset + step, 0, get_scrollback_length(terminal));
		gtk_widget_queue_draw(GTK_WIDGET(terminal));
		return TRUE;
	}
	restart_if_ended(terminal);
	VTermModifier modifiers = get_modifiers(state);
	terminal->scroll_offset = 0;
	for (guint index = 0; index < G_N_ELEMENTS(SPECIAL_KEYS); index++) {
		if (SPECIAL_KEYS[index].key == key) {
			vterm_keyboard_key(terminal->vterm, SPECIAL_KEYS[index].terminal_key, modifiers);
			return TRUE;
		}
	}
	if (key >= GDK_KEY_F1 && key <= GDK_KEY_F12) {
		vterm_keyboard_key(terminal->vterm, VTERM_KEY_FUNCTION(key - GDK_KEY_F1 + 1), modifiers);
		return TRUE;
	}
	/* Caps Lock gives an upper case letter, which libvterm would read as Ctrl with Shift */
	gunichar character = gdk_keyval_to_unicode(state & GDK_SHIFT_MASK ? key : gdk_keyval_to_lower(key));
	if (!character || !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK)))
		return FALSE;
	/* Shift is already part of the character, and libvterm would otherwise send it as a separate modifier */
	vterm_keyboard_unichar(terminal->vterm, character, (VTermModifier)(modifiers & ~VTERM_MOD_SHIFT));
	return TRUE;
}


static gboolean on_scrolled(GtkEventControllerScroll *scroll, double dx, double dy, Terminal *terminal) {
	int lines = (int)(dy * WHEEL_LINES);
	if (terminal->is_alternate_screen) {
		for (int step = 0; step < ABS(lines); step++)
			vterm_keyboard_key(terminal->vterm, lines < 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
		return TRUE;
	}
	terminal->scroll_offset = CLAMP(terminal->scroll_offset - lines, 0, get_scrollback_length(terminal));
	gtk_widget_queue_draw(GTK_WIDGET(terminal));
	return TRUE;
}


static void on_drag_started(GtkGestureDrag *drag, double x, double y, Terminal *terminal) {
	gtk_widget_grab_focus(GTK_WIDGET(terminal));
	terminal->selection_anchor = terminal->selection_end = get_position_at(terminal, x, y);
	terminal->has_selection = FALSE;
	gtk_widget_queue_draw(GTK_WIDGET(terminal));
}


static void on_drag_updated(GtkGestureDrag *drag, double offset_x, double offset_y, Terminal *terminal) {
	double x = 0, y = 0;
	gtk_gesture_drag_get_start_point(drag, &x, &y);
	terminal->selection_end = get_position_at(terminal, x + offset_x, y + offset_y);
	terminal->has_selection = TRUE;
	gtk_widget_queue_draw(GTK_WIDGET(terminal));
}


static void on_drag_ended(GtkGestureDrag *drag, double offset_x, double offset_y, Terminal *terminal) {
	if (!terminal->has_selection)
		return;
	g_autofree char *text = get_selected_text(terminal);
	gdk_clipboard_set_text(gtk_widget_get_primary_clipboard(GTK_WIDGET(terminal)), text);
	if (get_switch_setting("copy-on-select"))
		gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(terminal)), text);
}


static void on_focus_changed(Terminal *terminal, gboolean is_focused) {
	terminal->is_focused = is_focused;
	if (is_focused) {
		gtk_im_context_focus_in(terminal->input_method);
		vterm_state_focus_in(vterm_obtain_state(terminal->vterm));
	} else {
		gtk_im_context_focus_out(terminal->input_method);
		vterm_state_focus_out(vterm_obtain_state(terminal->vterm));
	}
	gtk_widget_queue_draw(GTK_WIDGET(terminal));
}


static void on_focus_entered(GtkEventControllerFocus *focus, Terminal *terminal) {
	on_focus_changed(terminal, TRUE);
}


static void on_focus_left(GtkEventControllerFocus *focus, Terminal *terminal) {
	on_focus_changed(terminal, FALSE);
}


/* ==================================== Widget ==================================== */


static void terminal_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
	Terminal *terminal = CRIMSON_TERMINAL(widget);
	if (terminal->cell_width <= 0)
		measure_cells(terminal);
	int columns = MAX(MINIMUM_COLUMNS, (int)((width - 2 * PADDING) / terminal->cell_width));
	int rows = MAX(MINIMUM_ROWS, (int)((height - 2 * PADDING) / terminal->cell_height));
	resize_screen(terminal, rows, columns);
}


/* A new font or size needs new cell metrics and freshly shaped glyphs */
static void terminal_css_changed(GtkWidget *widget, GtkCssStyleChange *change) {
	GTK_WIDGET_CLASS(terminal_parent_class)->css_changed(widget, change);
	clear_fonts(CRIMSON_TERMINAL(widget));
	gtk_widget_queue_resize(widget);
}


static void terminal_dispose(GObject *object) {
	Terminal *terminal = CRIMSON_TERMINAL(object);
	g_clear_handle_id(&terminal->read_source, g_source_remove);
	g_clear_handle_id(&terminal->child_watch, g_source_remove);
	if (terminal->child > 0)
		kill(terminal->child, SIGHUP);
	terminal->child = 0;
	if (terminal->pty >= 0)
		close(terminal->pty);
	terminal->pty = -1;
	g_clear_object(&terminal->input_method);
	clear_fonts(terminal);
	G_OBJECT_CLASS(terminal_parent_class)->dispose(object);
}


static void terminal_finalize(GObject *object) {
	Terminal *terminal = CRIMSON_TERMINAL(object);
	vterm_free(terminal->vterm);
	g_ptr_array_unref(terminal->scrollback);
	g_hash_table_unref(terminal->glyphs);
	g_free(terminal->directory);
	G_OBJECT_CLASS(terminal_parent_class)->finalize(object);
}


static void terminal_class_init(TerminalClass *class) {
	G_OBJECT_CLASS(class)->dispose = terminal_dispose;
	G_OBJECT_CLASS(class)->finalize = terminal_finalize;
	GTK_WIDGET_CLASS(class)->snapshot = terminal_snapshot;
	GTK_WIDGET_CLASS(class)->size_allocate = terminal_size_allocate;
	GTK_WIDGET_CLASS(class)->css_changed = terminal_css_changed;
	gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(class), "terminal");
}


static void add_controllers(Terminal *terminal) {
	GtkWidget *widget = GTK_WIDGET(terminal);
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_key_set_im_context(GTK_EVENT_CONTROLLER_KEY(keys), terminal->input_method);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), terminal);
	gtk_widget_add_controller(widget, keys);
	GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL
		| GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
	g_signal_connect(scroll, "scroll", G_CALLBACK(on_scrolled), terminal);
	gtk_widget_add_controller(widget, scroll);
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_started), terminal);
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_updated), terminal);
	g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_ended), terminal);
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(drag));
	GtkEventController *focus = gtk_event_controller_focus_new();
	g_signal_connect(focus, "enter", G_CALLBACK(on_focus_entered), terminal);
	g_signal_connect(focus, "leave", G_CALLBACK(on_focus_left), terminal);
	gtk_widget_add_controller(widget, focus);
}


static void terminal_init(Terminal *terminal) {
	terminal->pty = -1;
	terminal->rows = 24;
	terminal->columns = 80;
	terminal->scrollback = g_ptr_array_new_with_free_func(g_free);
	terminal->glyphs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)free_cached_glyph);
	terminal->vterm = vterm_new(terminal->rows, terminal->columns);
	vterm_set_utf8(terminal->vterm, 1);
	vterm_output_set_callback(terminal->vterm, write_to_shell, terminal);
	terminal->screen = vterm_obtain_screen(terminal->vterm);
	vterm_screen_set_callbacks(terminal->screen, &SCREEN_CALLBACKS, terminal);
	vterm_screen_enable_altscreen(terminal->screen, 1);
	vterm_screen_enable_reflow(terminal->screen, true);
	vterm_screen_set_damage_merge(terminal->screen, VTERM_DAMAGE_SCROLL);
	vterm_screen_reset(terminal->screen, 1);
	terminal->input_method = gtk_im_multicontext_new();
	gtk_im_context_set_client_widget(terminal->input_method, GTK_WIDGET(terminal));
	g_signal_connect(terminal->input_method, "commit", G_CALLBACK(on_text_committed), terminal);
	gtk_widget_set_focusable(GTK_WIDGET(terminal), TRUE);
	gtk_widget_set_cursor_from_name(GTK_WIDGET(terminal), "text");
	gtk_widget_add_css_class(GTK_WIDGET(terminal), "code-text");
	add_controllers(terminal);
}


/* A size of zero follows the editor, whose style sheet already sets the code font for the terminal too */
static void apply_terminal_style(const char *key, gpointer data) {
	if (key && g_strcmp0(key, "terminal-text-size") != 0)
		return;
	if (!terminal_style) {
		terminal_style = gtk_css_provider_new();
		gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(terminal_style),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
	}
	int size = get_number_setting("terminal-text-size");
	g_autofree char *css = size > 0 ? g_strdup_printf("terminal.code-text { font-size: %dpx; }", size)
		: g_strdup("");
	gtk_css_provider_load_from_string(terminal_style, css);
}


void register_terminal_settings(void) {
	register_settings(TERMINAL_SETTINGS, G_N_ELEMENTS(TERMINAL_SETTINGS));
	apply_terminal_style(NULL, NULL);
	watch_settings(apply_terminal_style, NULL);
}


void set_terminal_background(const GdkRGBA *background) {
	has_theme_background = background != NULL;
	if (background)
		theme_background = *background;
}


GtkWidget *create_terminal(void) {
	return g_object_new(TERMINAL_TYPE, NULL);
}


void start_terminal(GtkWidget *widget, const char *directory) {
	Terminal *terminal = CRIMSON_TERMINAL(widget);
	if (terminal->pty >= 0)
		return;
	g_set_str(&terminal->directory, directory);
	if (terminal->has_ended)
		vterm_screen_reset(terminal->screen, 1);
	start_shell(terminal);
}


void run_terminal_command(GtkWidget *widget, const char *directory, const char *command) {
	Terminal *terminal = CRIMSON_TERMINAL(widget);
	start_terminal(widget, directory);
	terminal->scroll_offset = 0;
	write_to_shell(command, strlen(command), terminal);
	write_to_shell("\r", 1, terminal);
}
