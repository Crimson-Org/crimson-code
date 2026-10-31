#include <gtksourceview/gtksource.h>

#include "editor-cursors.h"

#define CURSORS_KEY "extra-cursors"
#define SELECTION_TAG "crimson-extra-selection"
#define CARET_WIDTH_PIXELS 2
/* Extra selections take the scheme's selection colour, or a faint accent when it has none */
#define FALLBACK_SELECTION "rgba(53, 132, 228, 0.35)"

/* One extra cursor, where the selection runs from bound to insert and is empty when they meet */
typedef struct {
	GtkTextMark *insert, *bound;
} ExtraCursor;

/* What a deletion at the main cursor does, worked out before GTK deletes and done to the extras afterwards */
typedef enum {
	MIRROR_NONE,
	MIRROR_SELECTIONS,
	MIRROR_BACKWARD,
	MIRROR_FORWARD,
} MirrorKind;

typedef struct {
	GtkTextView *view;
	GtkTextBuffer *buffer;
	GPtrArray *cursors;
	GtkEventController *keys;
	GtkGesture *clicks;
	/* Set while edits are copied to the extras, so those edits are not copied again */
	gboolean is_mirroring;
	MirrorKind pending_kind;
	int pending_length;
	guint refresh_source;
} Cursors;


static Cursors *get_cursors(GtkTextView *view) {
	return g_object_get_data(G_OBJECT(view), CURSORS_KEY);
}


static void free_cursor(gpointer data) {
	ExtraCursor *cursor = data;
	GtkTextBuffer *buffer = gtk_text_mark_get_buffer(cursor->insert);
	if (buffer) {
		gtk_text_buffer_delete_mark(buffer, cursor->insert);
		gtk_text_buffer_delete_mark(buffer, cursor->bound);
	}
	g_object_unref(cursor->insert);
	g_object_unref(cursor->bound);
	g_free(cursor);
}


static GtkTextTag *get_selection_tag(Cursors *cursors) {
	GtkTextTagTable *table = gtk_text_buffer_get_tag_table(cursors->buffer);
	GtkTextTag *tag = gtk_text_tag_table_lookup(table, SELECTION_TAG);
	if (!tag)
		tag = gtk_text_buffer_create_tag(cursors->buffer, SELECTION_TAG, NULL);
	GtkSourceStyleScheme *scheme = gtk_source_buffer_get_style_scheme(GTK_SOURCE_BUFFER(cursors->buffer));
	GtkSourceStyle *style = scheme ? gtk_source_style_scheme_get_style(scheme, "selection") : NULL;
	g_autofree char *colour = NULL;
	gboolean is_set = FALSE;
	if (style)
		g_object_get(style, "background", &colour, "background-set", &is_set, NULL);
	g_object_set(tag, "background", is_set && colour ? colour : FALLBACK_SELECTION, NULL);
	return tag;
}


/* Cursors that moved onto the main cursor or onto each other become one, as in other editors */
static void merge_cursors(Cursors *cursors) {
	GtkTextIter main_insert;
	gtk_text_buffer_get_iter_at_mark(cursors->buffer, &main_insert, gtk_text_buffer_get_insert(cursors->buffer));
	for (guint index = cursors->cursors->len; index > 0; index--) {
		ExtraCursor *cursor = g_ptr_array_index(cursors->cursors, index - 1);
		GtkTextIter insert;
		gtk_text_buffer_get_iter_at_mark(cursors->buffer, &insert, cursor->insert);
		gboolean is_duplicate = gtk_text_iter_equal(&insert, &main_insert);
		for (guint other = 0; other < index - 1 && !is_duplicate; other++) {
			GtkTextIter other_insert;
			gtk_text_buffer_get_iter_at_mark(cursors->buffer, &other_insert,
				((ExtraCursor *)g_ptr_array_index(cursors->cursors, other))->insert);
			is_duplicate = gtk_text_iter_equal(&insert, &other_insert);
		}
		if (is_duplicate)
			g_ptr_array_remove_index(cursors->cursors, index - 1);
	}
}


/* Shows the extra selections and redraws the carets after anything changed them */
static void refresh(Cursors *cursors) {
	merge_cursors(cursors);
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(cursors->buffer, &start, &end);
	GtkTextTag *tag = get_selection_tag(cursors);
	gtk_text_buffer_remove_tag(cursors->buffer, tag, &start, &end);
	for (guint index = 0; index < cursors->cursors->len; index++) {
		ExtraCursor *cursor = g_ptr_array_index(cursors->cursors, index);
		gtk_text_buffer_get_iter_at_mark(cursors->buffer, &start, cursor->insert);
		gtk_text_buffer_get_iter_at_mark(cursors->buffer, &end, cursor->bound);
		gtk_text_buffer_apply_tag(cursors->buffer, tag, &start, &end);
	}
	gtk_widget_queue_draw(GTK_WIDGET(cursors->view));
}


static gboolean refresh_later(gpointer data) {
	Cursors *cursors = data;
	cursors->refresh_source = 0;
	refresh(cursors);
	return G_SOURCE_REMOVE;
}


static void clear_cursors(Cursors *cursors) {
	if (cursors->cursors->len == 0)
		return;
	g_ptr_array_set_size(cursors->cursors, 0);
	refresh(cursors);
}


/* Right gravity keeps both marks after text typed at the cursor, so the cursor moves on as the text grows */
static void add_cursor(Cursors *cursors, const GtkTextIter *insert, const GtkTextIter *bound) {
	ExtraCursor *cursor = g_new0(ExtraCursor, 1);
	cursor->insert = g_object_ref(gtk_text_buffer_create_mark(cursors->buffer, NULL, insert, FALSE));
	cursor->bound = g_object_ref(gtk_text_buffer_create_mark(cursors->buffer, NULL, bound, FALSE));
	g_ptr_array_add(cursors->cursors, cursor);
}


static void get_cursor_iters(Cursors *cursors, ExtraCursor *cursor, GtkTextIter *insert, GtkTextIter *bound) {
	gtk_text_buffer_get_iter_at_mark(cursors->buffer, insert, cursor->insert);
	gtk_text_buffer_get_iter_at_mark(cursors->buffer, bound, cursor->bound);
}


/* ==================================== Selecting matches ==================================== */


static gboolean is_word_character(gunichar character) {
	return g_unichar_isalnum(character) || character == '_';
}


static gboolean select_word_at_cursor(Cursors *cursors) {
	GtkTextIter start, end;
	gtk_text_buffer_get_iter_at_mark(cursors->buffer, &start, gtk_text_buffer_get_insert(cursors->buffer));
	end = start;
	while (!gtk_text_iter_starts_line(&start)) {
		GtkTextIter before = start;
		gtk_text_iter_backward_char(&before);
		if (!is_word_character(gtk_text_iter_get_char(&before)))
			break;
		start = before;
	}
	while (!gtk_text_iter_ends_line(&end) && is_word_character(gtk_text_iter_get_char(&end)))
		gtk_text_iter_forward_char(&end);
	if (gtk_text_iter_equal(&start, &end))
		return FALSE;
	gtk_text_buffer_select_range(cursors->buffer, &end, &start);
	return TRUE;
}


static gboolean is_selected(Cursors *cursors, const GtkTextIter *start) {
	for (guint index = 0; index < cursors->cursors->len; index++) {
		GtkTextIter insert, bound;
		get_cursor_iters(cursors, g_ptr_array_index(cursors->cursors, index), &insert, &bound);
		gtk_text_iter_order(&bound, &insert);
		if (gtk_text_iter_equal(&bound, start))
			return TRUE;
	}
	return FALSE;
}


/* Searches on from the main selection and round from the top, skipping matches that already have a cursor */
static gboolean find_next(Cursors *cursors, const char *text, const GtkTextIter *after, GtkTextIter *start,
	GtkTextIter *end) {
	GtkTextIter from = *after, top;
	gtk_text_buffer_get_start_iter(cursors->buffer, &top);
	for (int pass = 0; pass < 2; pass++) {
		while (gtk_text_iter_forward_search(&from, text, GTK_TEXT_SEARCH_TEXT_ONLY, start, end,
			pass == 0 ? NULL : after)) {
			if (!is_selected(cursors, start))
				return TRUE;
			from = *end;
		}
		from = top;
	}
	return FALSE;
}


void select_next_match(GtkTextView *view) {
	Cursors *cursors = get_cursors(view);
	GtkTextIter selection_start, selection_end, start, end;
	if (!cursors || !gtk_text_buffer_get_selection_bounds(cursors->buffer, &selection_start, &selection_end)) {
		if (cursors)
			select_word_at_cursor(cursors);
		return;
	}
	g_autofree char *text = gtk_text_iter_get_text(&selection_start, &selection_end);
	if (!find_next(cursors, text, &selection_end, &start, &end) || gtk_text_iter_equal(&start, &selection_start))
		return;
	/* The newest match becomes the main selection, as in other editors, and the old one stays as an extra */
	add_cursor(cursors, &selection_end, &selection_start);
	gtk_text_buffer_select_range(cursors->buffer, &end, &start);
	gtk_text_view_scroll_mark_onscreen(view, gtk_text_buffer_get_insert(cursors->buffer));
	refresh(cursors);
}


void select_all_matches(GtkTextView *view) {
	Cursors *cursors = get_cursors(view);
	GtkTextIter selection_start, selection_end;
	if (!cursors)
		return;
	if (!gtk_text_buffer_get_selection_bounds(cursors->buffer, &selection_start, &selection_end)
		&& !select_word_at_cursor(cursors))
		return;
	gtk_text_buffer_get_selection_bounds(cursors->buffer, &selection_start, &selection_end);
	g_autofree char *text = gtk_text_iter_get_text(&selection_start, &selection_end);
	GtkTextIter from, start, end;
	gtk_text_buffer_get_start_iter(cursors->buffer, &from);
	while (gtk_text_iter_forward_search(&from, text, GTK_TEXT_SEARCH_TEXT_ONLY, &start, &end, NULL)) {
		if (!gtk_text_iter_equal(&start, &selection_start) && !is_selected(cursors, &start))
			add_cursor(cursors, &end, &start);
		from = end;
	}
	refresh(cursors);
}


/* ==================================== Copying edits ==================================== */


static gboolean is_at_main_cursor(Cursors *cursors, const GtkTextIter *iter) {
	GtkTextIter insert;
	gtk_text_buffer_get_iter_at_mark(cursors->buffer, &insert, gtk_text_buffer_get_insert(cursors->buffer));
	return gtk_text_iter_equal(&insert, iter);
}


static gboolean is_copying(Cursors *cursors) {
	return !cursors->is_mirroring && cursors->cursors->len > 0 && gtk_widget_has_focus(GTK_WIDGET(cursors->view));
}


static void delete_selection(Cursors *cursors, ExtraCursor *cursor) {
	GtkTextIter insert, bound;
	get_cursor_iters(cursors, cursor, &insert, &bound);
	if (!gtk_text_iter_equal(&insert, &bound))
		gtk_text_buffer_delete(cursors->buffer, &insert, &bound);
}


/* Text typed at the main cursor replaces each extra selection, or goes in at each extra cursor */
static void on_text_inserted(GtkTextBuffer *buffer, GtkTextIter *location, char *text, int length, Cursors *cursors) {
	if (!is_copying(cursors) || !is_at_main_cursor(cursors, location))
		return;
	cursors->is_mirroring = TRUE;
	for (guint index = 0; index < cursors->cursors->len; index++) {
		ExtraCursor *cursor = g_ptr_array_index(cursors->cursors, index);
		delete_selection(cursors, cursor);
		GtkTextIter insert;
		gtk_text_buffer_get_iter_at_mark(buffer, &insert, cursor->insert);
		gtk_text_buffer_insert(buffer, &insert, text, length);
		gtk_text_buffer_move_mark(buffer, cursor->bound, &insert);
		gtk_text_buffer_move_mark(buffer, cursor->insert, &insert);
	}
	cursors->is_mirroring = FALSE;
	refresh(cursors);
}


/* GTK's iterators must stay valid until it has deleted, so this only works out what the extras should do */
static void on_delete_planned(GtkTextBuffer *buffer, GtkTextIter *start, GtkTextIter *end, Cursors *cursors) {
	cursors->pending_kind = MIRROR_NONE;
	if (!is_copying(cursors))
		return;
	GtkTextIter selection_start, selection_end;
	if (gtk_text_buffer_get_selection_bounds(buffer, &selection_start, &selection_end))
		cursors->pending_kind = gtk_text_iter_equal(start, &selection_start) && gtk_text_iter_equal(end, &selection_end)
			? MIRROR_SELECTIONS : MIRROR_NONE;
	else if (is_at_main_cursor(cursors, end))
		cursors->pending_kind = MIRROR_BACKWARD;
	else if (is_at_main_cursor(cursors, start))
		cursors->pending_kind = MIRROR_FORWARD;
	cursors->pending_length = gtk_text_iter_get_offset(end) - gtk_text_iter_get_offset(start);
}


static void on_deleted(GtkTextBuffer *buffer, GtkTextIter *start, GtkTextIter *end, Cursors *cursors) {
	MirrorKind kind = cursors->pending_kind;
	cursors->pending_kind = MIRROR_NONE;
	if (kind == MIRROR_NONE)
		return;
	cursors->is_mirroring = TRUE;
	for (guint index = 0; index < cursors->cursors->len; index++) {
		ExtraCursor *cursor = g_ptr_array_index(cursors->cursors, index);
		GtkTextIter insert, bound;
		get_cursor_iters(cursors, cursor, &insert, &bound);
		if (!gtk_text_iter_equal(&insert, &bound)) {
			delete_selection(cursors, cursor);
		} else if (kind == MIRROR_BACKWARD || kind == MIRROR_FORWARD) {
			GtkTextIter other = insert;
			if (kind == MIRROR_BACKWARD)
				gtk_text_iter_backward_chars(&other, cursors->pending_length);
			else
				gtk_text_iter_forward_chars(&other, cursors->pending_length);
			gtk_text_buffer_delete(buffer, &insert, &other);
		}
		gtk_text_buffer_get_iter_at_mark(buffer, &insert, cursor->insert);
		gtk_text_buffer_move_mark(buffer, cursor->bound, &insert);
	}
	cursors->is_mirroring = FALSE;
	refresh(cursors);
}


/* ==================================== Moving and clearing ==================================== */


static void move_iter(GtkTextIter *iter, guint key) {
	int column = gtk_text_iter_get_line_offset(iter);
	switch (key) {
	case GDK_KEY_Left: gtk_text_iter_backward_char(iter); break;
	case GDK_KEY_Right: gtk_text_iter_forward_char(iter); break;
	case GDK_KEY_Home: gtk_text_iter_set_line_offset(iter, 0); break;
	case GDK_KEY_End:
		if (!gtk_text_iter_ends_line(iter))
			gtk_text_iter_forward_to_line_end(iter);
		break;
	case GDK_KEY_Up:
	case GDK_KEY_Down:
		if (key == GDK_KEY_Up ? gtk_text_iter_backward_line(iter) : gtk_text_iter_forward_line(iter)) {
			int length = gtk_text_iter_get_chars_in_line(iter) - (gtk_text_iter_ends_line(iter) ? 0 : 1);
			gtk_text_iter_set_line_offset(iter, MIN(column, MAX(length, 0)));
		}
		break;
	default: break;
	}
}


/* Arrow keys, Home and End move every cursor, Shift extends every selection, and Escape goes back to one cursor */
static gboolean on_key_pressed(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType state,
	Cursors *cursors) {
	if (cursors->cursors->len == 0)
		return GDK_EVENT_PROPAGATE;
	if (key == GDK_KEY_Escape) {
		clear_cursors(cursors);
		return GDK_EVENT_STOP;
	}
	gboolean is_movement = key == GDK_KEY_Left || key == GDK_KEY_Right || key == GDK_KEY_Up || key == GDK_KEY_Down
		|| key == GDK_KEY_Home || key == GDK_KEY_End;
	if (!is_movement || (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)))
		return GDK_EVENT_PROPAGATE;
	gboolean is_extending = (state & GDK_SHIFT_MASK) != 0;
	for (guint index = 0; index < cursors->cursors->len; index++) {
		ExtraCursor *cursor = g_ptr_array_index(cursors->cursors, index);
		GtkTextIter insert, bound;
		get_cursor_iters(cursors, cursor, &insert, &bound);
		move_iter(&insert, key);
		gtk_text_buffer_move_mark(cursors->buffer, cursor->insert, &insert);
		if (!is_extending)
			gtk_text_buffer_move_mark(cursors->buffer, cursor->bound, &insert);
	}
	/* The main cursor moves only after this key goes on, so merging waits until then */
	if (!cursors->refresh_source)
		cursors->refresh_source = g_idle_add(refresh_later, cursors);
	return GDK_EVENT_PROPAGATE;
}


static void on_pressed(GtkGestureClick *gesture, int presses, double x, double y, Cursors *cursors) {
	clear_cursors(cursors);
}


/* ==================================== Drawing and lifetime ==================================== */


void draw_extra_cursors(GtkTextView *view, GtkSnapshot *snapshot) {
	Cursors *cursors = get_cursors(view);
	if (!cursors || cursors->cursors->len == 0)
		return;
	GdkRGBA colour;
	gtk_widget_get_color(GTK_WIDGET(view), &colour);
	for (guint index = 0; index < cursors->cursors->len; index++) {
		ExtraCursor *cursor = g_ptr_array_index(cursors->cursors, index);
		GtkTextIter insert;
		gtk_text_buffer_get_iter_at_mark(cursors->buffer, &insert, cursor->insert);
		GdkRectangle location;
		gtk_text_view_get_iter_location(view, &insert, &location);
		gtk_snapshot_append_color(snapshot, &colour, &GRAPHENE_RECT_INIT(location.x, location.y, CARET_WIDTH_PIXELS,
			location.height));
	}
}


void add_editor_cursors(GtkTextView *view, GtkWidget *scroller) {
	Cursors *cursors = g_new0(Cursors, 1);
	cursors->view = view;
	cursors->buffer = gtk_text_view_get_buffer(view);
	cursors->cursors = g_ptr_array_new_with_free_func(free_cursor);
	g_signal_connect_after(cursors->buffer, "insert-text", G_CALLBACK(on_text_inserted), cursors);
	g_signal_connect(cursors->buffer, "delete-range", G_CALLBACK(on_delete_planned), cursors);
	g_signal_connect_after(cursors->buffer, "delete-range", G_CALLBACK(on_deleted), cursors);
	cursors->keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(cursors->keys, GTK_PHASE_CAPTURE);
	g_signal_connect(cursors->keys, "key-pressed", G_CALLBACK(on_key_pressed), cursors);
	gtk_widget_add_controller(scroller, cursors->keys);
	cursors->clicks = gtk_gesture_click_new();
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(cursors->clicks), GTK_PHASE_CAPTURE);
	g_signal_connect(cursors->clicks, "pressed", G_CALLBACK(on_pressed), cursors);
	gtk_widget_add_controller(GTK_WIDGET(view), GTK_EVENT_CONTROLLER(cursors->clicks));
	g_object_set_data(G_OBJECT(view), CURSORS_KEY, cursors);
}


void remove_editor_cursors(GtkTextView *view) {
	Cursors *cursors = g_object_steal_data(G_OBJECT(view), CURSORS_KEY);
	if (!cursors)
		return;
	clear_cursors(cursors);
	g_clear_handle_id(&cursors->refresh_source, g_source_remove);
	g_signal_handlers_disconnect_by_data(cursors->buffer, cursors);
	g_signal_handlers_disconnect_by_data(cursors->keys, cursors);
	g_signal_handlers_disconnect_by_data(cursors->clicks, cursors);
	g_ptr_array_unref(cursors->cursors);
	g_free(cursors);
}
