#include <string.h>

#include <gtksourceview/gtksource.h>

#include "../lang/en.h"
#include "editor-hover.h"
#include "editor.h"
#include "markdown.h"

#define HOVER_DELAY_MILLISECONDS 400
#define TEXT_WIDTH_CHARACTERS 80
#define MAXIMUM_CARD_HEIGHT 420
#define MAXIMUM_DOCUMENTATION_LINES 60
#define MAXIMUM_DOCUMENTATION_BYTES 5000
#define CARD_KEY "hover-card"
#define HIDE_DELAY_MILLISECONDS 250
#define AREA_SLACK_PIXELS 4
#define MINIMUM_CARD_PIXELS 120
/* How close to the card's edge the pointer must be to resize it */
#define EDGE_PIXELS 12
/* Room kept between the card and the editor's sides, and between the card and its word */
#define CARD_MARGIN_PIXELS 6
#define CARD_GAP_PIXELS 4
#define FENCE "```"

/* Icons for each severity, matching the gutter */
static const char *const SEVERITY_ICONS[] = {
	NULL, "dialog-error-symbolic", "dialog-warning-symbolic", "dialog-information-symbolic",
	"dialog-information-symbolic",
};

/* ==================================== Problem cards ==================================== */


static GtkWidget *create_text_label(const char *markup, const char *style) {
	GtkWidget *label = gtk_label_new(NULL);
	/* Markup that does not read still shows its text rather than nothing */
	if (pango_parse_markup(markup, -1, 0, NULL, NULL, NULL, NULL))
		gtk_label_set_markup(GTK_LABEL(label), markup);
	else
		gtk_label_set_text(GTK_LABEL(label), markup);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_max_width_chars(GTK_LABEL(label), TEXT_WIDTH_CHARACTERS);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);
	if (style)
		gtk_widget_add_css_class(label, style);
	return label;
}


static GtkWidget *create_problem_card(const Diagnostic *problem) {
	GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_widget_add_css_class(card, "hover-problem");
	int severity = problem->severity < G_N_ELEMENTS(SEVERITY_ICONS) ? (int)problem->severity : 1;
	GtkWidget *icon = gtk_image_new_from_icon_name(SEVERITY_ICONS[severity]);
	gtk_widget_set_valign(icon, GTK_ALIGN_START);
	gtk_widget_add_css_class(icon, severity == 1 ? "error" : severity == 2 ? "warning" : "accent");
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	g_autofree char *message = g_markup_escape_text(problem->message, -1);
	gtk_box_append(GTK_BOX(text), create_text_label(message, NULL));
	if (problem->source || problem->code) {
		g_autofree char *origin = g_markup_printf_escaped("%s%s%s", problem->source ? problem->source : "",
			problem->source && problem->code ? " " : "", problem->code ? problem->code : "");
		gtk_box_append(GTK_BOX(text), create_text_label(origin, "dim-label"));
	}
	gtk_box_append(GTK_BOX(card), icon);
	gtk_box_append(GTK_BOX(card), text);
	return card;
}


/* ==================================== Documentation cards ==================================== */


/* Writes the look of the highlighting tags at a spot as Pango span attributes. Tags come lowest priority first and
 * often overlap, so each attribute takes the last tag that sets it, as Pango allows each only once in a span */
static void append_tag_attributes(GString *attributes, GtkTextIter *iter) {
	g_autoptr(GSList) tags = gtk_text_iter_get_tags(iter);
	GdkRGBA chosen_colour = {0};
	gboolean has_chosen_colour = FALSE, is_italic = FALSE;
	int chosen_weight = 0;
	for (GSList *item = tags; item; item = item->next) {
		gboolean has_colour = FALSE, has_weight = FALSE, has_style = FALSE;
		GdkRGBA *colour = NULL;
		int weight = 0;
		PangoStyle style = PANGO_STYLE_NORMAL;
		g_object_get(item->data, "foreground-set", &has_colour, "foreground-rgba", &colour, "weight-set", &has_weight,
			"weight", &weight, "style-set", &has_style, "style", &style, NULL);
		if (has_colour && colour) {
			chosen_colour = *colour;
			has_chosen_colour = TRUE;
		}
		if (has_weight)
			chosen_weight = weight;
		if (has_style)
			is_italic = style == PANGO_STYLE_ITALIC;
		if (colour)
			gdk_rgba_free(colour);
	}
	if (has_chosen_colour)
		g_string_append_printf(attributes, " foreground=\"#%02x%02x%02x\"", (int)(chosen_colour.red * 255),
			(int)(chosen_colour.green * 255), (int)(chosen_colour.blue * 255));
	if (chosen_weight)
		g_string_append_printf(attributes, " weight=\"%d\"", chosen_weight);
	if (is_italic)
		g_string_append(attributes, " style=\"italic\"");
}


/* Highlights the code with the editor's language and colours, then writes it as label markup, since a text view
 * lays out lazily and would be measured too short inside the card */
static char *highlight_code(const char *code, GtkSourceLanguage *language, GtkSourceStyleScheme *scheme) {
	/* A buffer with a language must be asked for differently from one without, which refuses a missing language */
	g_autoptr(GtkSourceBuffer) buffer = language ? gtk_source_buffer_new_with_language(language)
		: gtk_source_buffer_new(NULL);
	gtk_source_buffer_set_style_scheme(buffer, scheme);
	gtk_text_buffer_set_text(GTK_TEXT_BUFFER(buffer), code, -1);
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(buffer), &start, &end);
	gtk_source_buffer_ensure_highlight(buffer, &start, &end);
	GString *markup = g_string_new(NULL);
	for (GtkTextIter iter = start; !gtk_text_iter_is_end(&iter);) {
		GtkTextIter next = iter;
		if (!gtk_text_iter_forward_to_tag_toggle(&next, NULL))
			next = end;
		g_autofree char *text = gtk_text_iter_get_text(&iter, &next);
		g_autofree char *escaped = g_markup_escape_text(text, -1);
		g_autoptr(GString) attributes = g_string_new(NULL);
		append_tag_attributes(attributes, &iter);
		if (attributes->len)
			g_string_append_printf(markup, "<span%s>%s</span>", attributes->str, escaped);
		else
			g_string_append(markup, escaped);
		iter = next;
	}
	return g_string_free(markup, FALSE);
}


static GtkWidget *create_code_view(const char *code, const char *language_name, GtkSourceView *editor_view) {
	GtkSourceBuffer *editor_buffer = GTK_SOURCE_BUFFER(gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor_view)));
	GtkSourceLanguage *language = language_name && *language_name ? gtk_source_language_manager_get_language(
		gtk_source_language_manager_get_default(), language_name) : NULL;
	/* Only a sample without a named language takes the file's own, since highlighting man pages or other text as
	 * the file's language colours words that mean nothing there */
	gboolean is_named = language_name && *language_name;
	g_autofree char *markup = highlight_code(code, is_named ? language : gtk_source_buffer_get_language(editor_buffer),
		gtk_source_buffer_get_style_scheme(editor_buffer));
	GtkWidget *label = create_text_label(markup, "code-text");
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_CHAR);
	gtk_widget_add_css_class(label, "hover-code");
	return label;
}


static void flush_paragraph(GtkWidget *box, GString *paragraph) {
	g_strchomp(paragraph->str);
	if (paragraph->len > 0 && *paragraph->str)
		gtk_box_append(GTK_BOX(box), create_text_label(paragraph->str, NULL));
	g_string_truncate(paragraph, 0);
}


/* Lays out the answer as paragraphs and highlighted code, as editors show Markdown documentation */
static GtkWidget *create_documentation(const char *markdown, GtkSourceView *editor_view) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	g_auto(GStrv) lines = g_strsplit(markdown, "\n", -1);
	g_autoptr(GString) paragraph = g_string_new(NULL), code = g_string_new(NULL);
	g_autofree char *code_language = NULL;
	gboolean is_in_code = FALSE;
	for (int index = 0; lines[index]; index++) {
		const char *line = lines[index];
		const char *trimmed = line + strspn(line, " \t");
		if (g_str_has_prefix(trimmed, FENCE)) {
			if (is_in_code) {
				g_strchomp(code->str);
				gtk_box_append(GTK_BOX(box), create_code_view(code->str, code_language, editor_view));
				g_string_truncate(code, 0);
			} else {
				flush_paragraph(box, paragraph);
				g_free(code_language);
				code_language = g_strstrip(g_strdup(trimmed + strlen(FENCE)));
			}
			is_in_code = !is_in_code;
			continue;
		}
		if (is_in_code) {
			g_string_append_printf(code, "%s\n", line);
		} else if (!*trimmed || g_str_has_prefix(trimmed, "---")) {
			flush_paragraph(box, paragraph);
		} else {
			g_autofree char *markup = convert_markdown_line(line);
			g_string_append_printf(paragraph, "%s%s", paragraph->len ? " " : "", markup);
		}
	}
	if (is_in_code && code->len)
		gtk_box_append(GTK_BOX(box), create_code_view(code->str, code_language, editor_view));
	flush_paragraph(box, paragraph);
	return box;
}


/* Plain text answers are shown as written, line by line, without reading any Markdown marks into them */
static GtkWidget *create_plain_text(const char *text) {
	g_autofree char *escaped = g_markup_escape_text(text, -1);
	return create_text_label(escaped, NULL);
}


/* Documentation such as a whole manual page takes seconds to lay out, so only its first part is shown */
static char *shorten_documentation(const char *text) {
	const char *end = text;
	int lines = 0;
	while (*end && lines < MAXIMUM_DOCUMENTATION_LINES && end - text < MAXIMUM_DOCUMENTATION_BYTES) {
		const char *next = strchr(end, '\n');
		if (!next)
			next = end + strlen(end);
		if (next - text > MAXIMUM_DOCUMENTATION_BYTES)
			break;
		end = *next ? next + 1 : next;
		lines++;
	}
	if (!*end)
		return g_strdup(text);
	/* A single very long line is cut at a character boundary instead */
	if (end == text)
		end = g_utf8_offset_to_pointer(text, g_utf8_pointer_to_offset(text, text + MAXIMUM_DOCUMENTATION_BYTES));
	g_autofree char *kept = g_strndup(text, end - text);
	g_strchomp(kept);
	/* A code block cut in the middle is closed again, so the rest of the card does not turn into code */
	gboolean is_in_code = FALSE;
	g_auto(GStrv) kept_lines = g_strsplit(kept, "\n", -1);
	for (int index = 0; kept_lines[index]; index++)
		if (g_str_has_prefix(kept_lines[index] + strspn(kept_lines[index], " \t"), FENCE))
			is_in_code = !is_in_code;
	return g_strconcat(kept, is_in_code ? "\n" FENCE : "", "\n\n…", NULL);
}


/* Hover contents come as plain text, Markdown, a language and code pair or a list of those */
static void append_hover_contents(GtkWidget *box, const JsonValue *contents, GtkSourceView *view) {
	if (!contents)
		return;
	if (contents->kind == JSON_ARRAY) {
		for (guint index = 0; index < get_json_length(contents); index++)
			append_hover_contents(box, get_json_item(contents, index), view);
		return;
	}
	const char *language = get_json_string(get_json_member(contents, "language"), NULL);
	const char *kind = get_json_string(get_json_member(contents, "kind"), NULL);
	const char *value = contents->kind == JSON_STRING ? contents->string
		: get_json_string(get_json_member(contents, "value"), "");
	g_autofree char *whole = g_strstrip(g_strdup(value));
	if (!*whole)
		return;
	g_autofree char *trimmed = shorten_documentation(whole);
	if (language)
		gtk_box_append(GTK_BOX(box), create_code_view(trimmed, language, view));
	else if (g_strcmp0(kind, "plaintext") == 0)
		gtk_box_append(GTK_BOX(box), create_plain_text(trimmed));
	else
		gtk_box_append(GTK_BOX(box), create_documentation(trimmed, view));
}


/* ==================================== The card ==================================== */


typedef enum {
	EDGE_NONE = 0,
	EDGE_RIGHT = 1 << 0,
	EDGE_TOP = 1 << 1,
	EDGE_BOTTOM = 1 << 2,
} ResizeEdges;


/* One editor's hover card, shared with pending server answers so a late answer is harmless
 * It lies on a layer over the editor, opens beside the word where there is room and resizes by the far edges */
typedef struct {
	Editor *editor;
	GtkSourceView *view;
	GtkWidget *overlay, *frame, *content, *scroller;
	/* Where the card lies on the layer, and whether it is above the word */
	GdkRectangle rect;
	gboolean is_above;
	/* The size the person dragged the card to, kept for later cards, or zero to fit the contents */
	int chosen_width, chosen_height;
	/* Which edge the current drag moves, and how far from that edge it was taken */
	ResizeEdges drag_edges;
	double grab_x, grab_y;
	guint settle_source, hide_source;
	double pointer_x, pointer_y;
	/* What the card is about, in the view's coordinates, so moving within it keeps the card open */
	GdkRectangle area;
	gboolean is_over_card, is_resizing;
	/* Counts cards shown, so an answer meant for an earlier card is dropped */
	guint generation;
} HoverCard;

/* A pending documentation request and the card it was made for */
typedef struct {
	HoverCard *card;
	guint generation;
} DocumentationRequest;


static void clear_card(gpointer data) {
	HoverCard *card = data;
	g_clear_handle_id(&card->settle_source, g_source_remove);
	g_clear_handle_id(&card->hide_source, g_source_remove);
}


static void release_card(HoverCard *card) {
	g_rc_box_release_full(card, clear_card);
}


static gboolean is_card_shown(HoverCard *card) {
	return card->frame && gtk_widget_get_visible(card->frame);
}


static void hide_card(HoverCard *card) {
	g_clear_handle_id(&card->hide_source, g_source_remove);
	card->generation++;
	card->is_over_card = FALSE;
	if (card->frame)
		gtk_widget_set_visible(card->frame, FALSE);
}


/* GTK reports the pointer leaving when a drag lets go even while it stays over the card, so the pointer itself is
 * asked where it is. The card lies in the window's own surface, which answers on Wayland as well */
static gboolean is_pointer_over_card(HoverCard *card) {
	if (!is_card_shown(card))
		return FALSE;
	GtkNative *native = gtk_widget_get_native(card->overlay);
	GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
	GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(card->overlay));
	GdkDevice *pointer = seat ? gdk_seat_get_pointer(seat) : NULL;
	double x = 0, y = 0, surface_x = 0, surface_y = 0;
	if (!surface || !pointer || !gdk_surface_get_device_position(surface, pointer, &x, &y, NULL))
		return FALSE;
	gtk_native_get_surface_transform(native, &surface_x, &surface_y);
	graphene_point_t point;
	if (!gtk_widget_compute_point(GTK_WIDGET(native), card->overlay,
		&GRAPHENE_POINT_INIT((float)(x - surface_x), (float)(y - surface_y)), &point))
		return FALSE;
	return point.x >= card->rect.x && point.x < card->rect.x + card->rect.width && point.y >= card->rect.y
		&& point.y < card->rect.y + card->rect.height;
}


static gboolean hide_card_later(gpointer data) {
	HoverCard *card = data;
	card->hide_source = 0;
	if (!is_pointer_over_card(card) && !card->is_resizing)
		hide_card(card);
	return G_SOURCE_REMOVE;
}


static void queue_hide(HoverCard *card) {
	if (!card->hide_source && is_card_shown(card))
		card->hide_source = g_timeout_add(HIDE_DELAY_MILLISECONDS, hide_card_later, card);
}


static void on_card_entered(GtkEventControllerMotion *motion, double x, double y, HoverCard *card) {
	card->is_over_card = TRUE;
	g_clear_handle_id(&card->hide_source, g_source_remove);
}


static void on_card_left(GtkEventControllerMotion *motion, HoverCard *card) {
	card->is_over_card = FALSE;
	queue_hide(card);
}


/* ==================================== Placing and resizing ==================================== */


static int get_overlay_width(HoverCard *card) {
	return gtk_widget_get_width(card->overlay);
}


static int get_overlay_height(HoverCard *card) {
	return gtk_widget_get_height(card->overlay);
}


static gboolean get_child_position(GtkOverlay *overlay, GtkWidget *widget, GdkRectangle *allocation,
	HoverCard *card) {
	if (widget != card->frame)
		return FALSE;
	*allocation = card->rect;
	return TRUE;
}


/* The card takes its contents' own size, or the size the person chose, then the side with room for it, and the
 * room it has caps the height so it never spills off the editor or jumps to the other side */
static void place_card(HoverCard *card) {
	graphene_point_t anchor;
	if (!gtk_widget_compute_point(GTK_WIDGET(card->view), card->overlay,
		&GRAPHENE_POINT_INIT(card->area.x, card->area.y), &anchor))
		return;
	int overlay_width = get_overlay_width(card), overlay_height = get_overlay_height(card);
	int maximum_width = MAX(MINIMUM_CARD_PIXELS, overlay_width - 2 * CARD_MARGIN_PIXELS);
	int natural_width = 0, natural_height = 0;
	gtk_widget_measure(card->frame, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &natural_width, NULL, NULL);
	int width = CLAMP(card->chosen_width > 0 ? card->chosen_width : natural_width, MINIMUM_CARD_PIXELS,
		maximum_width);
	gtk_widget_measure(card->frame, GTK_ORIENTATION_VERTICAL, width, NULL, &natural_height, NULL, NULL);
	int height = card->chosen_height > 0 ? card->chosen_height : MIN(natural_height, MAXIMUM_CARD_HEIGHT);
	int above = (int)anchor.y - CARD_GAP_PIXELS - CARD_MARGIN_PIXELS;
	int below = overlay_height - (int)anchor.y - card->area.height - CARD_GAP_PIXELS - CARD_MARGIN_PIXELS;
	card->is_above = height <= above || above >= below;
	height = CLAMP(height, MINIMUM_CARD_PIXELS / 2, MAX(MINIMUM_CARD_PIXELS / 2, card->is_above ? above : below));
	int x = CLAMP((int)anchor.x, CARD_MARGIN_PIXELS, MAX(CARD_MARGIN_PIXELS, overlay_width - width
		- CARD_MARGIN_PIXELS));
	int y = card->is_above ? (int)anchor.y - CARD_GAP_PIXELS - height
		: (int)anchor.y + card->area.height + CARD_GAP_PIXELS;
	card->rect = (GdkRectangle){x, y, width, height};
	gtk_widget_queue_allocate(card->overlay);
}


/* Only the edges away from the word resize the card */
static ResizeEdges find_edges(HoverCard *card, double x, double y) {
	ResizeEdges edges = EDGE_NONE;
	if (x > card->rect.width - EDGE_PIXELS)
		edges |= EDGE_RIGHT;
	if (card->is_above && y < EDGE_PIXELS)
		edges |= EDGE_TOP;
	if (!card->is_above && y > card->rect.height - EDGE_PIXELS)
		edges |= EDGE_BOTTOM;
	return edges;
}


static const char *get_edge_cursor(ResizeEdges edges) {
	if (edges == EDGE_RIGHT)
		return "ew-resize";
	if (edges == EDGE_TOP || edges == EDGE_BOTTOM)
		return "ns-resize";
	if (edges == (EDGE_RIGHT | EDGE_TOP))
		return "nesw-resize";
	return edges == (EDGE_RIGHT | EDGE_BOTTOM) ? "nwse-resize" : NULL;
}


static void on_edge_hovered(GtkEventControllerMotion *motion, double x, double y, HoverCard *card) {
	gtk_widget_set_cursor_from_name(card->frame, get_edge_cursor(find_edges(card, x, y)));
}


/* The layer stays still while the card changes, so the pointer is read there and nothing feeds back into itself */
static gboolean get_layer_point(HoverCard *card, double x, double y, graphene_point_t *point) {
	return gtk_widget_compute_point(card->frame, card->overlay, &GRAPHENE_POINT_INIT((float)x, (float)y), point);
}


static void on_resize_started(GtkGestureDrag *drag, double x, double y, HoverCard *card) {
	graphene_point_t point;
	card->drag_edges = find_edges(card, x, y);
	if (card->drag_edges == EDGE_NONE || !get_layer_point(card, x, y, &point)) {
		gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_DENIED);
		return;
	}
	gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_CLAIMED);
	card->is_resizing = TRUE;
	/* The edge keeps the distance from the pointer it had when grabbed, so it does not jump to the pointer */
	card->grab_x = card->rect.x + card->rect.width - point.x;
	card->grab_y = card->is_above ? point.y - card->rect.y : card->rect.y + card->rect.height - point.y;
}


/* The pointer may end outside the card after a drag, which then closes as it would on leaving */
static void on_resize_ended(GtkGestureDrag *drag, double offset_x, double offset_y, HoverCard *card) {
	card->is_resizing = FALSE;
	if (!card->is_over_card)
		queue_hide(card);
}


static void on_resize_moved(GtkGestureDrag *drag, double offset_x, double offset_y, HoverCard *card) {
	double start_x = 0, start_y = 0;
	graphene_point_t point;
	gtk_gesture_drag_get_start_point(drag, &start_x, &start_y);
	if (!get_layer_point(card, start_x + offset_x, start_y + offset_y, &point))
		return;
	GdkRectangle *rect = &card->rect;
	if (card->drag_edges & EDGE_RIGHT) {
		int right = (int)(point.x + card->grab_x);
		rect->width = CLAMP(right - rect->x, MINIMUM_CARD_PIXELS, get_overlay_width(card) - rect->x
			- CARD_MARGIN_PIXELS);
	}
	if (card->drag_edges & EDGE_TOP) {
		int bottom = rect->y + rect->height, top = (int)(point.y - card->grab_y);
		rect->height = CLAMP(bottom - top, MINIMUM_CARD_PIXELS / 2, bottom - CARD_MARGIN_PIXELS);
		rect->y = bottom - rect->height;
	}
	if (card->drag_edges & EDGE_BOTTOM) {
		int bottom = (int)(point.y + card->grab_y);
		rect->height = CLAMP(bottom - rect->y, MINIMUM_CARD_PIXELS / 2, get_overlay_height(card) - rect->y
			- CARD_MARGIN_PIXELS);
	}
	card->chosen_width = rect->width;
	card->chosen_height = rect->height;
	gtk_widget_queue_allocate(card->overlay);
}


/* ==================================== Making and showing the card ==================================== */


static void create_frame(HoverCard *card) {
	card->overlay = get_editor_overlay(card->editor);
	card->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_widget_add_css_class(card->content, "hover-card");
	card->scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(card->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(card->scroller), TRUE);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(card->scroller), TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(card->scroller), card->content);
	gtk_widget_set_vexpand(card->scroller, TRUE);
	card->frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(card->frame, "hover-frame");
	gtk_widget_set_overflow(card->frame, GTK_OVERFLOW_HIDDEN);
	gtk_box_append(GTK_BOX(card->frame), card->scroller);
	gtk_widget_set_visible(card->frame, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(card->overlay), card->frame);
	g_signal_connect(card->overlay, "get-child-position", G_CALLBACK(get_child_position), card);
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "enter", G_CALLBACK(on_card_entered), card);
	g_signal_connect(motion, "leave", G_CALLBACK(on_card_left), card);
	g_signal_connect(motion, "motion", G_CALLBACK(on_edge_hovered), card);
	gtk_widget_add_controller(card->frame, motion);
	GtkGesture *drag = gtk_gesture_drag_new();
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drag), GTK_PHASE_CAPTURE);
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_resize_started), card);
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_resize_moved), card);
	g_signal_connect(drag, "drag-end", G_CALLBACK(on_resize_ended), card);
	gtk_widget_add_controller(card->frame, GTK_EVENT_CONTROLLER(drag));
}


static void present_card(HoverCard *card) {
	if (!gtk_widget_get_first_child(card->content))
		return;
	gtk_widget_set_visible(card->frame, TRUE);
	place_card(card);
}


static void start_card(HoverCard *card, const GdkRectangle *area) {
	if (!card->frame)
		create_frame(card);
	card->generation++;
	card->area = *area;
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(card->content)))
		gtk_box_remove(GTK_BOX(card->content), child);
}


/* ==================================== Filling the card ==================================== */


static void on_documentation_answered(JsonValue *result, gpointer data) {
	DocumentationRequest *request = data;
	HoverCard *card = request->card;
	if (card->editor && request->generation == card->generation) {
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
		append_hover_contents(box, get_json_member(result, "contents"), card->view);
		if (gtk_widget_get_first_child(box)) {
			if (gtk_widget_get_first_child(card->content))
				gtk_box_append(GTK_BOX(card->content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
			gtk_box_append(GTK_BOX(card->content), box);
			present_card(card);
		} else {
			g_object_ref_sink(box);
			g_object_unref(box);
		}
	}
	release_card(card);
	g_free(request);
}


static void ask_for_documentation(HoverCard *card, const GtkTextIter *iter) {
	LspClient *client = get_editor_language_server(card->editor);
	int line = 0, column = 0;
	if (!client || !get_editor_lsp_position(card->editor, iter, &line, &column))
		return;
	send_editor_changes(card->editor);
	DocumentationRequest *request = g_new0(DocumentationRequest, 1);
	request->card = g_rc_box_acquire(card);
	request->generation = card->generation;
	request_lsp(client, "textDocument/hover", make_lsp_position_params(get_editor_uri(card->editor), line, column),
		on_documentation_answered, request);
}


static void append_problems(HoverCard *card, GPtrArray *problems) {
	for (guint index = 0; index < problems->len; index++)
		gtk_box_append(GTK_BOX(card->content), create_problem_card(g_ptr_array_index(problems, index)));
}


/* The area of a whole line across the given width, in the view's coordinates */
static void get_line_area(HoverCard *card, const GtkTextIter *iter, int x, int width, GdkRectangle *area) {
	int y = 0, height = 0, window_x = 0, window_y = 0;
	gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(card->view), iter, &y, &height);
	gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(card->view), GTK_TEXT_WINDOW_WIDGET, 0, y, &window_x,
		&window_y);
	*area = (GdkRectangle){x, window_y, width, height};
}


static void show_line_card(HoverCard *card, const GtkTextIter *iter, int x, int width) {
	g_autoptr(GPtrArray) problems = find_editor_problems(card->editor, iter, TRUE);
	if (problems->len == 0)
		return;
	GdkRectangle area;
	get_line_area(card, iter, x, width, &area);
	start_card(card, &area);
	append_problems(card, problems);
	present_card(card);
}


/* The word under the pointer, so the card stays while the pointer moves along it */
static void get_word_area(HoverCard *card, const GtkTextIter *iter, GdkRectangle *area) {
	GtkTextIter start = *iter, end = *iter;
	if (gtk_text_iter_inside_word(iter) && !gtk_text_iter_starts_word(&start))
		gtk_text_iter_backward_word_start(&start);
	if (gtk_text_iter_inside_word(iter))
		gtk_text_iter_forward_word_end(&end);
	if (gtk_text_iter_equal(&start, &end))
		gtk_text_iter_forward_char(&end);
	GdkRectangle first, last;
	gtk_text_view_get_iter_location(GTK_TEXT_VIEW(card->view), &start, &first);
	gtk_text_iter_backward_char(&end);
	gtk_text_view_get_iter_location(GTK_TEXT_VIEW(card->view), &end, &last);
	int x = 0, y = 0;
	gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(card->view), GTK_TEXT_WINDOW_WIDGET, first.x, first.y, &x,
		&y);
	*area = (GdkRectangle){x, y, MAX(last.x + last.width - first.x, 1), first.height};
}


static void show_text_card(HoverCard *card, const GtkTextIter *iter) {
	GdkRectangle area;
	get_word_area(card, iter, &area);
	start_card(card, &area);
	g_autoptr(GPtrArray) problems = find_editor_problems(card->editor, iter, FALSE);
	append_problems(card, problems);
	present_card(card);
	ask_for_documentation(card, iter);
}


static int get_gutter_width(HoverCard *card) {
	return gtk_widget_get_width(GTK_WIDGET(gtk_source_view_get_gutter(card->view, GTK_TEXT_WINDOW_LEFT)));
}


/* Opens the card for whatever sits under the resting pointer, a gutter line or a word */
static gboolean on_pointer_settled(gpointer data) {
	HoverCard *card = data;
	card->settle_source = 0;
	if (!card->editor)
		return G_SOURCE_REMOVE;
	int buffer_x = 0, buffer_y = 0, gutter_width = get_gutter_width(card);
	gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(card->view), GTK_TEXT_WINDOW_WIDGET, (int)card->pointer_x,
		(int)card->pointer_y, &buffer_x, &buffer_y);
	GtkTextIter iter;
	if (card->pointer_x < gutter_width) {
		gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(card->view), &iter, buffer_y, NULL);
		show_line_card(card, &iter, 0, gutter_width);
	} else if (gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(card->view), &iter, buffer_x, buffer_y)) {
		show_text_card(card, &iter);
	}
	return G_SOURCE_REMOVE;
}


/* ==================================== Pointer and keys ==================================== */


static gboolean is_in_area(HoverCard *card, double x, double y) {
	return x >= card->area.x - AREA_SLACK_PIXELS && x <= card->area.x + card->area.width + AREA_SLACK_PIXELS
		&& y >= card->area.y - AREA_SLACK_PIXELS && y <= card->area.y + card->area.height + AREA_SLACK_PIXELS;
}


static void on_pointer_moved(GtkEventControllerMotion *motion, double x, double y, HoverCard *card) {
	/* Motion events repeated at the same spot come from widgets changing, not the person moving, and moving while
	 * resizing the card is not a move to another word */
	if ((x == card->pointer_x && y == card->pointer_y) || card->is_resizing)
		return;
	card->pointer_x = x;
	card->pointer_y = y;
	gboolean is_shown = is_card_shown(card);
	if (is_shown && is_in_area(card, x, y))
		return;
	if (is_shown)
		queue_hide(card);
	g_clear_handle_id(&card->settle_source, g_source_remove);
	card->settle_source = g_timeout_add(HOVER_DELAY_MILLISECONDS, on_pointer_settled, card);
}


static void on_pointer_left(GtkEventControllerMotion *motion, HoverCard *card) {
	g_clear_handle_id(&card->settle_source, g_source_remove);
	queue_hide(card);
}


static gboolean on_key_pressed(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType state,
	HoverCard *card) {
	g_clear_handle_id(&card->settle_source, g_source_remove);
	hide_card(card);
	return GDK_EVENT_PROPAGATE;
}


static gboolean on_scrolled(GtkEventControllerScroll *scroll, double dx, double dy, HoverCard *card) {
	hide_card(card);
	return GDK_EVENT_PROPAGATE;
}


/* ==================================== Attaching ==================================== */


void add_editor_hover(Editor *editor) {
	HoverCard *card = g_rc_box_new0(HoverCard);
	card->editor = editor;
	card->view = get_editor_view(editor);
	card->pointer_x = card->pointer_y = -1;
	GtkWidget *view = GTK_WIDGET(card->view);
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(on_pointer_moved), card);
	g_signal_connect(motion, "leave", G_CALLBACK(on_pointer_left), card);
	gtk_widget_add_controller(view, motion);
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), card);
	gtk_widget_add_controller(view, keys);
	GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
	g_signal_connect(scroll, "scroll", G_CALLBACK(on_scrolled), card);
	gtk_widget_add_controller(view, scroll);
	g_object_set_data(G_OBJECT(view), CARD_KEY, card);
}


void show_editor_line_hover(Editor *editor, int line, const GdkRectangle *area) {
	HoverCard *card = g_object_get_data(G_OBJECT(get_editor_view(editor)), CARD_KEY);
	GtkTextIter iter;
	gtk_text_buffer_get_iter_at_line(gtk_text_view_get_buffer(GTK_TEXT_VIEW(card->view)), &iter, line);
	g_clear_handle_id(&card->settle_source, g_source_remove);
	show_line_card(card, &iter, 0, get_gutter_width(card));
}


void show_editor_cursor_hover(Editor *editor) {
	HoverCard *card = g_object_get_data(G_OBJECT(get_editor_view(editor)), CARD_KEY);
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(card->view));
	GtkTextIter iter;
	gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
	g_clear_handle_id(&card->settle_source, g_source_remove);
	show_text_card(card, &iter);
}


/* The controllers stay on the view until GTK frees it, so they are cut off from the card here */
void remove_editor_hover(Editor *editor) {
	GtkWidget *view = GTK_WIDGET(get_editor_view(editor));
	HoverCard *card = g_object_steal_data(G_OBJECT(view), CARD_KEY);
	if (!card)
		return;
	g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(view);
	for (guint index = 0; index < g_list_model_get_n_items(controllers); index++) {
		g_autoptr(GObject) controller = g_list_model_get_item(controllers, index);
		g_signal_handlers_disconnect_by_data(controller, card);
	}
	hide_card(card);
	if (card->frame) {
		g_signal_handlers_disconnect_by_data(card->overlay, card);
		gtk_overlay_remove_overlay(GTK_OVERLAY(card->overlay), card->frame);
	}
	card->frame = card->content = NULL;
	card->editor = NULL;
	release_card(card);
}
