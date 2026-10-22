#include <string.h>

#include <gtksourceview/gtksource.h>

#include "../lang/en.h"
#include "editor-completion.h"
#include "editor.h"
#include "settings.h"

#define POPUP_KEY "completion-popup"
#define TYPING_DELAY_MILLISECONDS 150
#define POPUP_WIDTH 420
#define POPUP_HEIGHT 300
#define PAGE_ROWS 8
/* The protocol's kind for plain text, used for words taken from the file */
#define WORD_KIND 1
#define WORD_MINIMUM_LENGTH 3
/* Typing offers words only after this many letters, while Ctrl+Space offers them at once */
#define WORD_MINIMUM_TYPED 2

/* Names for the protocol's completion item kinds, numbered from one */
static const char *const KIND_NAMES[] = {
	NULL, "text", "method", "function", "constructor", "field", "variable", "class", "interface", "module",
	"property", "unit", "value", "enum", "keyword", "snippet", "colour", "file", "reference", "folder", "member",
	"constant", "struct", "event", "operator", "type",
};

static const SettingDefinition COMPLETION_SETTINGS[] = {
	{"complete-while-typing", "editor", TEXT_GROUP_COMPLETION, TEXT_SETTING_COMPLETE_TYPING,
		TEXT_SETTING_COMPLETE_TYPING_DETAIL, SETTING_SWITCH, "true", 0, 0, NULL, NULL, {NULL}},
};

/* One suggestion from the language server, or a word from the file when there is none */
struct _CompletionItem {
	GObject parent_instance;
	char *label, *detail, *documentation, *insert_text, *filter_text, *sort_text;
	int kind;
	/* Characters between a word from the file and the cursor, so the nearest words come first */
	int distance;
};

enum {
	PROPERTY_FILTER_TEXT = 1,
};

#define COMPLETION_ITEM_TYPE (completion_item_get_type())
G_DECLARE_FINAL_TYPE(CompletionItem, completion_item, CRIMSON, COMPLETION_ITEM, GObject)
G_DEFINE_FINAL_TYPE(CompletionItem, completion_item, G_TYPE_OBJECT)


/* ==================================== Items ==================================== */


static void completion_item_finalize(GObject *object) {
	CompletionItem *item = CRIMSON_COMPLETION_ITEM(object);
	g_free(item->label);
	g_free(item->detail);
	g_free(item->documentation);
	g_free(item->insert_text);
	g_free(item->filter_text);
	g_free(item->sort_text);
	G_OBJECT_CLASS(completion_item_parent_class)->finalize(object);
}


static void completion_item_get_property(GObject *object, guint id, GValue *value, GParamSpec *specification) {
	if (id == PROPERTY_FILTER_TEXT)
		g_value_set_string(value, CRIMSON_COMPLETION_ITEM(object)->filter_text);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, specification);
}


/* The filter text property lets a GtkStringFilter narrow the list as more is typed */
static void completion_item_class_init(CompletionItemClass *class) {
	G_OBJECT_CLASS(class)->finalize = completion_item_finalize;
	G_OBJECT_CLASS(class)->get_property = completion_item_get_property;
	g_object_class_install_property(G_OBJECT_CLASS(class), PROPERTY_FILTER_TEXT, g_param_spec_string("filter-text",
		NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}


static void completion_item_init(CompletionItem *item) {
}


static char *read_documentation(const JsonValue *documentation) {
	if (!documentation)
		return NULL;
	if (documentation->kind == JSON_STRING)
		return g_strdup(documentation->string);
	return g_strdup(get_json_string(get_json_member(documentation, "value"), NULL));
}


/* The server's edit text wins over insert text, which wins over the label shown in the list */
static CompletionItem *create_item(const JsonValue *value) {
	CompletionItem *item = g_object_new(COMPLETION_ITEM_TYPE, NULL);
	/* clangd marks suggestions that would add an include with a leading bullet, which only clutters the list */
	const char *label = get_json_string(get_json_member(value, "label"), "");
	if (g_str_has_prefix(label, "\u2022"))
		label += strlen("\u2022");
	item->label = g_strstrip(g_strdup(label));
	item->detail = g_strdup(get_json_string(get_json_member(value, "detail"), NULL));
	item->documentation = read_documentation(get_json_member(value, "documentation"));
	const char *insert = get_json_string(find_json_path(value, "textEdit.newText"), NULL);
	insert = insert ? insert : get_json_string(get_json_member(value, "insertText"), NULL);
	item->insert_text = g_strdup(insert ? insert : item->label);
	item->filter_text = g_strdup(get_json_string(get_json_member(value, "filterText"), item->label));
	item->sort_text = g_strdup(get_json_string(get_json_member(value, "sortText"), item->label));
	item->kind = (int)get_json_number(get_json_member(value, "kind"), 0);
	return item;
}


/* ==================================== The popup ==================================== */


/* One editor's suggestion list, shared by reference with pending server answers so a late answer is harmless */
typedef struct {
	Editor *editor;
	GtkSourceView *view;
	GtkWidget *popover, *list, *source_label;
	GListStore *items;
	GtkFilterListModel *filtered;
	GtkSingleSelection *selection;
	GtkStringFilter *filter;
	guint typing_source, focus_source, generation;
	/* Watches clicks anywhere in the window, which the popup does not take the focus for */
	GtkWidget *window;
	GtkGesture *outside_clicks;
	/* The server said its list was cut short, so more typing asks again rather than only narrowing */
	gboolean is_incomplete;
	/* Set while a suggestion is being inserted, so that edit does not start another request */
	gboolean is_inserting;
} CompletionPopup;

/* A pending request and the popup state it was made for */
typedef struct {
	CompletionPopup *popup;
	guint generation;
} CompletionRequest;


static void clear_popup(gpointer data) {
	CompletionPopup *popup = data;
	g_clear_handle_id(&popup->typing_source, g_source_remove);
	g_clear_handle_id(&popup->focus_source, g_source_remove);
	g_clear_object(&popup->items);
}


static void release_popup(CompletionPopup *popup) {
	g_rc_box_release_full(popup, clear_popup);
}


static gboolean is_word_character(gunichar character) {
	return g_unichar_isalnum(character) || character == '_';
}


/* The start of the word being typed, which the suggestions replace */
static void get_word_start(CompletionPopup *popup, GtkTextIter *start, GtkTextIter *cursor) {
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(popup->view));
	gtk_text_buffer_get_iter_at_mark(buffer, cursor, gtk_text_buffer_get_insert(buffer));
	*start = *cursor;
	while (!gtk_text_iter_starts_line(start)) {
		GtkTextIter before = *start;
		gtk_text_iter_backward_char(&before);
		if (!is_word_character(gtk_text_iter_get_char(&before)))
			break;
		*start = before;
	}
}


static char *get_typed_word(CompletionPopup *popup) {
	GtkTextIter start, cursor;
	get_word_start(popup, &start, &cursor);
	return gtk_text_iter_get_text(&start, &cursor);
}


static gboolean is_shown(CompletionPopup *popup) {
	return popup->popover && gtk_widget_get_visible(popup->popover);
}


static void hide_popup(CompletionPopup *popup) {
	popup->generation++;
	if (popup->popover)
		gtk_popover_popdown(GTK_POPOVER(popup->popover));
}


static void insert_item(CompletionPopup *popup, CompletionItem *item) {
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(popup->view));
	GtkTextIter start, cursor;
	get_word_start(popup, &start, &cursor);
	popup->is_inserting = TRUE;
	gtk_text_buffer_begin_user_action(buffer);
	gtk_text_buffer_delete(buffer, &start, &cursor);
	gtk_text_buffer_insert(buffer, &start, item->insert_text, -1);
	gtk_text_buffer_end_user_action(buffer);
	popup->is_inserting = FALSE;
	hide_popup(popup);
}


static void insert_selected(CompletionPopup *popup) {
	CompletionItem *item = gtk_single_selection_get_selected_item(popup->selection);
	if (item)
		insert_item(popup, item);
}


static void on_row_activated(GtkListView *list, guint position, CompletionPopup *popup) {
	g_autoptr(CompletionItem) item = g_list_model_get_item(G_LIST_MODEL(popup->selection), position);
	if (item)
		insert_item(popup, item);
}


static void set_up_row(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer data) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *label = gtk_label_new(NULL), *detail = gtk_label_new(NULL), *kind = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_xalign(GTK_LABEL(detail), 0);
	gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(detail, TRUE);
	gtk_widget_add_css_class(detail, "dim-label");
	gtk_widget_add_css_class(kind, "dim-label");
	gtk_widget_add_css_class(label, "code-text");
	gtk_box_append(GTK_BOX(box), label);
	gtk_box_append(GTK_BOX(box), detail);
	gtk_box_append(GTK_BOX(box), kind);
	gtk_list_item_set_child(list_item, box);
}


static void bind_row(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer data) {
	CompletionItem *item = gtk_list_item_get_item(list_item);
	GtkWidget *label = gtk_widget_get_first_child(gtk_list_item_get_child(list_item));
	GtkWidget *detail = gtk_widget_get_next_sibling(label);
	gtk_label_set_text(GTK_LABEL(label), item->label);
	gtk_label_set_text(GTK_LABEL(detail), item->detail ? item->detail : "");
	gtk_label_set_text(GTK_LABEL(gtk_widget_get_next_sibling(detail)),
		item->kind > 0 && item->kind < (int)G_N_ELEMENTS(KIND_NAMES) ? KIND_NAMES[item->kind] : "");
	gtk_widget_set_tooltip_text(gtk_list_item_get_child(list_item), item->documentation);
}


static gboolean on_key_pressed(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType state,
	CompletionPopup *popup);


/* A click on the popup's own surface picks a suggestion, while one anywhere else closes the list */
static void on_window_pressed(GtkGestureClick *gesture, int presses, double x, double y, CompletionPopup *popup) {
	GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
	if (!is_shown(popup) || !event)
		return;
	if (gdk_event_get_surface(event) != gtk_native_get_surface(GTK_NATIVE(popup->popover)))
		hide_popup(popup);
}


static void watch_outside_clicks(CompletionPopup *popup) {
	GtkWidget *window = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(popup->view)));
	if (!window || window == popup->window)
		return;
	popup->window = window;
	popup->outside_clicks = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(popup->outside_clicks), 0);
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(popup->outside_clicks), GTK_PHASE_CAPTURE);
	g_signal_connect(popup->outside_clicks, "pressed", G_CALLBACK(on_window_pressed), popup);
	gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(popup->outside_clicks));
}


/* The popover belongs to the editor's box, whose layout manager places it as GTK requires of popover parents */
static void create_popover(CompletionPopup *popup) {
	popup->items = g_list_store_new(COMPLETION_ITEM_TYPE);
	GtkExpression *expression = gtk_property_expression_new(COMPLETION_ITEM_TYPE, NULL, "filter-text");
	popup->filter = gtk_string_filter_new(expression);
	gtk_string_filter_set_match_mode(popup->filter, GTK_STRING_FILTER_MATCH_MODE_PREFIX);
	popup->filtered = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(popup->items)), GTK_FILTER(popup->filter));
	popup->selection = gtk_single_selection_new(G_LIST_MODEL(popup->filtered));
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(set_up_row), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_row), NULL);
	popup->list = gtk_list_view_new(GTK_SELECTION_MODEL(popup->selection), factory);
	gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(popup->list), TRUE);
	gtk_widget_set_focusable(popup->list, FALSE);
	gtk_widget_add_css_class(popup->list, "completion-list");
	g_signal_connect(popup->list, "activate", G_CALLBACK(on_row_activated), popup);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), popup->list);
	/* A short list keeps the popup short, while a long one scrolls */
	gtk_widget_set_size_request(scroller, POPUP_WIDTH, -1);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), POPUP_HEIGHT);
	/* A small heading names where the suggestions come from, the language server or the file's own words */
	popup->source_label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(popup->source_label), 0);
	gtk_widget_add_css_class(popup->source_label, "completion-source");
	gtk_widget_add_css_class(popup->source_label, "dim-label");
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(box), popup->source_label);
	gtk_box_append(GTK_BOX(box), scroller);
	popup->popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(popup->popover), box);
	gtk_popover_set_autohide(GTK_POPOVER(popup->popover), FALSE);
	/* Typing continues in the editor, so nothing in the list may take the focus from it */
	gtk_widget_set_can_focus(popup->popover, FALSE);
	gtk_popover_set_has_arrow(GTK_POPOVER(popup->popover), FALSE);
	gtk_popover_set_position(GTK_POPOVER(popup->popover), GTK_POS_BOTTOM);
	gtk_widget_set_halign(popup->popover, GTK_ALIGN_START);
	gtk_widget_set_parent(popup->popover, get_editor_widget(popup->editor));
	/* Keys can arrive at the popup's own surface instead of the editor, so the list listens there too */
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), popup);
	gtk_widget_add_controller(popup->popover, keys);
}


/* Narrows the list to the typed word, closing it when nothing is left */
static void refilter(CompletionPopup *popup) {
	g_autofree char *word = get_typed_word(popup);
	gtk_string_filter_set_search(popup->filter, word);
	if (g_list_model_get_n_items(G_LIST_MODEL(popup->filtered)) == 0) {
		hide_popup(popup);
		return;
	}
	gtk_single_selection_set_selected(popup->selection, 0);
}


static void present_popup(CompletionPopup *popup) {
	GtkTextIter start, cursor;
	get_word_start(popup, &start, &cursor);
	GdkRectangle location;
	gtk_text_view_get_iter_location(GTK_TEXT_VIEW(popup->view), &start, &location);
	int x = 0, y = 0;
	gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(popup->view), GTK_TEXT_WINDOW_WIDGET, location.x, location.y,
		&x, &y);
	graphene_point_t origin;
	if (!gtk_widget_compute_point(GTK_WIDGET(popup->view), get_editor_widget(popup->editor),
		&GRAPHENE_POINT_INIT(x, y), &origin))
		return;
	GdkRectangle area = {(int)origin.x, (int)origin.y, 1, location.height};
	gtk_popover_set_pointing_to(GTK_POPOVER(popup->popover), &area);
	watch_outside_clicks(popup);
	gtk_popover_popup(GTK_POPOVER(popup->popover));
}


static int compare_sort_texts(gconstpointer first, gconstpointer second) {
	const CompletionItem *first_item = *(CompletionItem *const *)first, *second_item = *(CompletionItem *const *)second;
	return g_strcmp0(first_item->sort_text, second_item->sort_text);
}


static int compare_distances(gconstpointer first, gconstpointer second) {
	const CompletionItem *first_item = *(CompletionItem *const *)first, *second_item = *(CompletionItem *const *)second;
	return first_item->distance - second_item->distance;
}


static void on_completion_answered(JsonValue *result, gpointer data) {
	CompletionRequest *request = data;
	CompletionPopup *popup = request->popup;
	if (popup->editor && request->generation == popup->generation) {
		/* The answer is either a list of items or an object holding them */
		const JsonValue *items = result && result->kind == JSON_ARRAY ? result : get_json_member(result, "items");
		popup->is_incomplete = get_json_boolean(get_json_member(result, "isIncomplete"), FALSE);
		g_autoptr(GPtrArray) created = g_ptr_array_new_with_free_func(g_object_unref);
		for (guint index = 0; index < get_json_length(items); index++)
			g_ptr_array_add(created, create_item(get_json_item(items, index)));
		/* The protocol leaves the order to the client, which sorts by the server's sort text */
		g_ptr_array_sort(created, compare_sort_texts);
		g_list_store_splice(popup->items, 0, g_list_model_get_n_items(G_LIST_MODEL(popup->items)), created->pdata,
			created->len);
		LspClient *client = get_editor_language_server(popup->editor);
		gtk_label_set_text(GTK_LABEL(popup->source_label), client ? get_lsp_client_name(client) : "");
		refilter(popup);
			if (g_list_model_get_n_items(G_LIST_MODEL(popup->filtered)) > 0)
			present_popup(popup);
	}
	release_popup(popup);
	g_free(request);
}


static CompletionItem *create_word_item(const char *word) {
	CompletionItem *item = g_object_new(COMPLETION_ITEM_TYPE, NULL);
	item->label = g_strdup(word);
	item->insert_text = g_strdup(word);
	item->filter_text = g_strdup(word);
	item->kind = WORD_KIND;
	return item;
}


/* Keeps each word once at its distance nearest the cursor, skipping the word being typed, which would only
 * suggest itself */
static void add_word(GHashTable *words, const char *typed, const char *start, const char *end, int distance) {
	g_autofree char *word = g_strndup(start, (gsize)(end - start));
	if (g_utf8_strlen(word, -1) < WORD_MINIMUM_LENGTH || strcmp(word, typed) == 0 || g_ascii_isdigit(*word))
		return;
	CompletionItem *known = g_hash_table_lookup(words, word);
	if (known) {
		known->distance = MIN(known->distance, distance);
		return;
	}
	CompletionItem *item = create_word_item(word);
	item->distance = distance;
	g_hash_table_insert(words, g_steal_pointer(&word), item);
}


/* Without a language server the words already in the file are the suggestions, nearest to the cursor first, as
 * most editors offer */
static void suggest_words(CompletionPopup *popup, gboolean is_asked) {
	g_autofree char *typed = get_typed_word(popup);
	if (!is_asked && g_utf8_strlen(typed, -1) < WORD_MINIMUM_TYPED)
		return;
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(popup->view));
	GtkTextIter start, end, cursor;
	gtk_text_buffer_get_bounds(buffer, &start, &end);
	gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
	int cursor_offset = gtk_text_iter_get_offset(&cursor);
	g_autofree char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
	g_autoptr(GHashTable) words = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	const char *word_start = NULL;
	int offset = 0, word_offset = 0;
	for (const char *walker = text;; walker = g_utf8_next_char(walker), offset++) {
		gboolean is_end = *walker == '\0';
		gboolean is_word = !is_end && is_word_character(g_utf8_get_char(walker));
		if (is_word && !word_start) {
			word_start = walker;
			word_offset = offset;
		}
		if (!is_word && word_start) {
			add_word(words, typed, word_start, walker, ABS(word_offset - cursor_offset));
			word_start = NULL;
		}
		if (is_end)
			break;
	}
	g_autoptr(GPtrArray) items = g_hash_table_get_values_as_ptr_array(words);
	g_ptr_array_sort(items, compare_distances);
	g_list_store_splice(popup->items, 0, g_list_model_get_n_items(G_LIST_MODEL(popup->items)), items->pdata,
		items->len);
	gtk_label_set_text(GTK_LABEL(popup->source_label), TEXT_SUGGESTIONS_FROM_WORDS);
	popup->is_incomplete = FALSE;
	refilter(popup);
	if (g_list_model_get_n_items(G_LIST_MODEL(popup->filtered)) > 0)
		present_popup(popup);
}


static void request_suggestions(CompletionPopup *popup, gboolean is_asked) {
	LspClient *client = get_editor_language_server(popup->editor);
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(popup->view));
	GtkTextIter cursor;
	gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
	int line = 0, column = 0;
	if (!popup->popover)
		create_popover(popup);
	if (!client || !get_editor_lsp_position(popup->editor, &cursor, &line, &column)) {
		popup->generation++;
		suggest_words(popup, is_asked);
		return;
	}
	send_editor_changes(popup->editor);
	CompletionRequest *request = g_new0(CompletionRequest, 1);
	request->popup = g_rc_box_acquire(popup);
	request->generation = ++popup->generation;
	request_lsp(client, "textDocument/completion", make_lsp_position_params(get_editor_uri(popup->editor), line,
		column), on_completion_answered, request);
}


static gboolean on_typing_paused(gpointer data) {
	CompletionPopup *popup = data;
	popup->typing_source = 0;
	request_suggestions(popup, FALSE);
	return G_SOURCE_REMOVE;
}


/* ==================================== Typing and keys ==================================== */


static gboolean is_trigger(const GtkTextIter *end, gunichar character) {
	if (character == '.')
		return TRUE;
	GtkTextIter before = *end;
	if (!gtk_text_iter_backward_chars(&before, 2))
		return FALSE;
	gunichar previous = gtk_text_iter_get_char(&before);
	return (character == '>' && previous == '-') || (character == ':' && previous == ':');
}


/* A typed word or member access asks for suggestions after a short pause, while an open list narrows at once and
 * only asks again when the server said its list was cut short */
static void on_text_inserted(GtkTextBuffer *buffer, GtkTextIter *location, char *text, int length,
	CompletionPopup *popup) {
	if (popup->is_inserting || !gtk_widget_has_focus(GTK_WIDGET(popup->view)) || g_utf8_strlen(text, length) != 1)
		return;
	gunichar character = g_utf8_get_char(text);
	if (is_shown(popup) && is_word_character(character)) {
		refilter(popup);
		if (!popup->is_incomplete)
			return;
	}
	gboolean is_word = is_word_character(character) && get_switch_setting("complete-while-typing");
	if (!is_word && !is_trigger(location, character)) {
		hide_popup(popup);
		return;
	}
	g_clear_handle_id(&popup->typing_source, g_source_remove);
	popup->typing_source = g_timeout_add(TYPING_DELAY_MILLISECONDS, on_typing_paused, popup);
}


static void on_text_deleted(GtkTextBuffer *buffer, GtkTextIter *start, GtkTextIter *end, CompletionPopup *popup) {
	if (is_shown(popup) && !popup->is_inserting)
		refilter(popup);
}


static void move_selection(CompletionPopup *popup, int step) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(popup->filtered));
	if (count == 0)
		return;
	int position = (int)gtk_single_selection_get_selected(popup->selection) + step;
	guint selected = (guint)CLAMP(position, 0, (int)count - 1);
	gtk_single_selection_set_selected(popup->selection, selected);
	gtk_list_view_scroll_to(GTK_LIST_VIEW(popup->list), selected, GTK_LIST_SCROLL_NONE, NULL);
}


/* While the list is open these keys belong to it, ahead of the editor and the Vim engine */
static gboolean on_key_pressed(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType state,
	CompletionPopup *popup) {
	if (!is_shown(popup))
		return GDK_EVENT_PROPAGATE;
	switch (key) {
	case GDK_KEY_Up: move_selection(popup, -1); return GDK_EVENT_STOP;
	case GDK_KEY_Down: move_selection(popup, 1); return GDK_EVENT_STOP;
	case GDK_KEY_Page_Up: move_selection(popup, -PAGE_ROWS); return GDK_EVENT_STOP;
	case GDK_KEY_Page_Down: move_selection(popup, PAGE_ROWS); return GDK_EVENT_STOP;
	case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_Tab: insert_selected(popup); return GDK_EVENT_STOP;
	case GDK_KEY_Escape: hide_popup(popup); return GDK_EVENT_STOP;
	case GDK_KEY_Left: case GDK_KEY_Right: case GDK_KEY_Home: case GDK_KEY_End: hide_popup(popup); break;
	default: break;
	}
	return GDK_EVENT_PROPAGATE;
}


/* Showing the popup's own surface briefly takes the focus on some systems, so only a lasting loss closes the list */
static gboolean check_focus(gpointer data) {
	CompletionPopup *popup = data;
	popup->focus_source = 0;
	if (popup->editor && !gtk_widget_has_focus(GTK_WIDGET(popup->view)))
		hide_popup(popup);
	return G_SOURCE_REMOVE;
}


static void on_focus_left(GtkEventControllerFocus *focus, CompletionPopup *popup) {
	if (!popup->focus_source)
		popup->focus_source = g_idle_add(check_focus, popup);
}


/* ==================================== Attaching ==================================== */


void register_completion_settings(void) {
	register_settings(COMPLETION_SETTINGS, G_N_ELEMENTS(COMPLETION_SETTINGS));
}


void add_editor_completion(Editor *editor) {
	CompletionPopup *popup = g_rc_box_new0(CompletionPopup);
	popup->editor = editor;
	popup->view = get_editor_view(editor);
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(popup->view));
	g_signal_connect_after(buffer, "insert-text", G_CALLBACK(on_text_inserted), popup);
	g_signal_connect_after(buffer, "delete-range", G_CALLBACK(on_text_deleted), popup);
	GtkEventController *focus = gtk_event_controller_focus_new();
	g_signal_connect(focus, "leave", G_CALLBACK(on_focus_left), popup);
	gtk_widget_add_controller(GTK_WIDGET(popup->view), focus);
	g_object_set_data(G_OBJECT(popup->view), POPUP_KEY, popup);
}


/* Keys reach the scroller before the view and its Vim engine, so the list takes them there */
void add_editor_completion_keys(Editor *editor, GtkWidget *scroller) {
	CompletionPopup *popup = g_object_get_data(G_OBJECT(get_editor_view(editor)), POPUP_KEY);
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), popup);
	gtk_widget_add_controller(scroller, keys);
}


void show_editor_suggestions(Editor *editor) {
	CompletionPopup *popup = g_object_get_data(G_OBJECT(get_editor_view(editor)), POPUP_KEY);
	if (popup)
		request_suggestions(popup, TRUE);
}


void remove_editor_completion(Editor *editor) {
	GtkWidget *view = GTK_WIDGET(get_editor_view(editor));
	CompletionPopup *popup = g_object_steal_data(G_OBJECT(view), POPUP_KEY);
	if (!popup)
		return;
	g_signal_handlers_disconnect_by_data(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), popup);
	g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(view);
	for (guint index = 0; index < g_list_model_get_n_items(controllers); index++) {
		g_autoptr(GObject) controller = g_list_model_get_item(controllers, index);
		g_signal_handlers_disconnect_by_data(controller, popup);
	}
	GtkWidget *scroller = gtk_widget_get_parent(view);
	g_autoptr(GListModel) scroller_controllers = gtk_widget_observe_controllers(scroller);
	for (guint index = 0; index < g_list_model_get_n_items(scroller_controllers); index++) {
		g_autoptr(GObject) controller = g_list_model_get_item(scroller_controllers, index);
		g_signal_handlers_disconnect_by_data(controller, popup);
	}
	hide_popup(popup);
	if (popup->outside_clicks)
		gtk_widget_remove_controller(popup->window, GTK_EVENT_CONTROLLER(popup->outside_clicks));
	popup->outside_clicks = NULL;
	popup->window = NULL;
	g_clear_handle_id(&popup->typing_source, g_source_remove);
	g_clear_handle_id(&popup->focus_source, g_source_remove);
	if (popup->popover)
		gtk_widget_unparent(popup->popover);
	g_clear_object(&popup->selection);
	popup->popover = popup->list = NULL;
	popup->editor = NULL;
	release_popup(popup);
}
