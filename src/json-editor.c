#include <string.h>

#include <adwaita.h>
#include <gtksourceview/gtksource.h>

#include "../lang/en.h"
#include "json-editor.h"
#include "build/json.h"

#define EDITOR_KEY "json-editor"
#define TARGET_KEY "json-target"
#define LEVEL_INDENT_PIXELS 18
#define FOLD_WIDTH_PIXELS 18
#define DEFAULT_INDENT "\t"

/* A compact tree with one line per value in the code font and the scheme's colours, edited in place with a click
 * The text stays the source, so comments, spacing and undo all survive */
typedef struct {
	GtkTextBuffer *buffer;
	/* The text the tree was read from, which the recorded byte offsets point into */
	char *text;
	JsonValue *root;
	GtkWidget *stack, *rows, *problem;
	/* Paths of groups and lists the person folded, kept across rebuilds */
	GHashTable *collapsed;
	char *indent;
	guint rebuild_source;
	/* Set while this editor changes the text, so its own change does not read the text back too early */
	gboolean is_writing;
	/* The index path of the row being dragged */
	GArray *drag_path;
} JsonEditor;

/* A row's place in the tree as indices from the top, which stays meaningful after the text is read again */
typedef struct {
	JsonEditor *editor;
	GArray *path;
	char *fold_key;
} RowTarget;

static const struct {
	const char *id, *label, *literal;
	JsonKind kind;
} KINDS[] = {
	{"text", TEXT_JSON_TEXT, "\"\"", JSON_STRING}, {"number", TEXT_JSON_NUMBER, "0", JSON_NUMBER},
	{"switch", TEXT_JSON_SWITCH, "false", JSON_BOOLEAN}, {"list", TEXT_JSON_LIST, "[]", JSON_ARRAY},
	{"group", TEXT_JSON_GROUP, "{}", JSON_OBJECT}, {"empty", TEXT_JSON_EMPTY, "null", JSON_NULL},
};

/* Scheme styles tried in order for each part, since schemes name them differently */
static const struct {
	const char *css_class;
	const char *styles[4];
} PART_STYLES[] = {
	{"json-name", {"json:keyname", "def:identifier", "def:type", NULL}},
	{"json-string", {"json:string", "def:string", NULL}},
	{"json-number", {"json:number", "def:number", "def:decimal", NULL}},
	{"json-constant", {"json:special-constant", "def:special-constant", "def:boolean", NULL}},
};

static GtkCssProvider *part_style;

static void schedule_rebuild(JsonEditor *editor);


static JsonEditor *get_editor(GtkWidget *widget) {
	return g_object_get_data(G_OBJECT(widget), EDITOR_KEY);
}


static void free_target(gpointer data) {
	RowTarget *target = data;
	g_array_unref(target->path);
	g_free(target->fold_key);
	g_free(target);
}


static guint path_index(const GArray *path, guint position) {
	return g_array_index(path, guint, position);
}


/* The value at a path and its parent, or NULL when the text changed so the path no longer leads anywhere */
static JsonValue *find_value(JsonEditor *editor, const GArray *path, JsonValue **parent) {
	JsonValue *value = editor->root, *above = NULL;
	for (guint position = 0; value && position < path->len; position++) {
		guint index = path_index(path, position);
		if (!value->items || index >= value->items->len)
			return NULL;
		above = value;
		value = g_ptr_array_index(value->items, index);
	}
	if (parent)
		*parent = above;
	return value;
}


/* ==================================== Reading and patching the text ==================================== */


static void read_text(JsonEditor *editor) {
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(editor->buffer, &start, &end);
	g_free(editor->text);
	editor->text = gtk_text_buffer_get_text(editor->buffer, &start, &end, FALSE);
	g_clear_pointer(&editor->root, free_json);
	g_autoptr(GError) error = NULL;
	editor->root = parse_json(editor->text, -1, &error);
	if (!editor->root) {
		g_autofree char *message = g_strdup_printf(TEXT_JSON_NOT_VALID, error ? error->message : "");
		gtk_label_set_text(GTK_LABEL(editor->problem), message);
	}
}


static void get_iter_at_byte(JsonEditor *editor, GtkTextIter *iter, gsize byte) {
	glong offset = g_utf8_pointer_to_offset(editor->text, editor->text + byte);
	gtk_text_buffer_get_iter_at_offset(editor->buffer, iter, (int)offset);
}


/* Replaces bytes of the text read last, then reads it again, so offsets are always fresh for the next change */
static void replace_bytes(JsonEditor *editor, gsize start, gsize end, const char *replacement) {
	GtkTextIter from, to;
	get_iter_at_byte(editor, &from, start);
	get_iter_at_byte(editor, &to, end);
	editor->is_writing = TRUE;
	gtk_text_buffer_delete(editor->buffer, &from, &to);
	gtk_text_buffer_insert(editor->buffer, &from, replacement, -1);
	editor->is_writing = FALSE;
	read_text(editor);
}


static void begin_change(JsonEditor *editor) {
	gtk_text_buffer_begin_user_action(editor->buffer);
}


static void end_change(JsonEditor *editor) {
	gtk_text_buffer_end_user_action(editor->buffer);
	schedule_rebuild(editor);
}


/* The end of a quoted name that starts at the given byte */
static gsize find_name_end(JsonEditor *editor, gsize start) {
	const char *character = editor->text + start + 1;
	while (*character && *character != '"')
		character += *character == '\\' && character[1] ? 2 : 1;
	return (gsize)(character - editor->text) + (*character ? 1 : 0);
}


static gsize get_item_start(const JsonValue *parent, guint index) {
	if (parent->name_starts && index < parent->name_starts->len)
		return g_array_index(parent->name_starts, gsize, index);
	return ((JsonValue *)g_ptr_array_index(parent->items, index))->start;
}


static gboolean is_blank(char character) {
	return character == ' ' || character == '\t' || character == '\r';
}


/* The spaces before a value when it starts its line, used to line up new members with their siblings */
static char *get_line_indent(JsonEditor *editor, gsize position) {
	gsize line_start = position;
	while (line_start > 0 && editor->text[line_start - 1] != '\n')
		line_start--;
	for (gsize at = line_start; at < position; at++)
		if (!is_blank(editor->text[at]))
			return NULL;
	return g_strndup(editor->text + line_start, position - line_start);
}


static char *quote(const char *text) {
	g_autoptr(JsonValue) value = new_json_string(text);
	return write_json(value);
}


/* New fields get a name no sibling has, so the file stays valid */
static char *make_unique_name(const JsonValue *object, const char *base) {
	for (int number = 1;; number++) {
		g_autofree char *name = number == 1 ? g_strdup(base) : g_strdup_printf("%s %d", base, number);
		if (!get_json_member(object, name))
			return g_steal_pointer(&name);
	}
}


/* Inserts a member or item after another one, on its own line with the same indentation */
static void insert_after(JsonEditor *editor, JsonValue *parent, guint index, const char *value_text,
	const char *name_base) {
	JsonValue *item = g_ptr_array_index(parent->items, index);
	g_autofree char *indent = get_line_indent(editor, get_item_start(parent, index));
	g_autofree char *name = name_base ? make_unique_name(parent, name_base) : NULL;
	g_autofree char *quoted = name ? quote(name) : NULL;
	g_autofree char *inserted = g_strdup_printf(indent ? ",\n%s%s%s%s" : ", %s%s%s%s", indent ? indent : "",
		quoted ? quoted : "", quoted ? ": " : "", value_text);
	replace_bytes(editor, item->end, item->end, inserted);
}


/* An empty group or list gets its first member on a line of its own, one level further in */
static void insert_into(JsonEditor *editor, JsonValue *container, const char *value_text) {
	if (container->items->len > 0) {
		insert_after(editor, container, container->items->len - 1, value_text,
			container->kind == JSON_OBJECT ? TEXT_JSON_NEW_FIELD : NULL);
		return;
	}
	g_autofree char *outer = get_line_indent(editor, container->start);
	const char *base = outer ? outer : "";
	g_autofree char *name = container->kind == JSON_OBJECT ? quote(TEXT_JSON_NEW_FIELD) : NULL;
	g_autofree char *inserted = g_strdup_printf("\n%s%s%s%s%s\n%s", base, editor->indent, name ? name : "",
		name ? ": " : "", value_text, base);
	replace_bytes(editor, container->start + 1, container->end - 1, inserted);
}


/* Takes a member out with its comma, and its whole line when nothing else shares it */
static void remove_item(JsonEditor *editor, JsonValue *parent, guint index) {
	JsonValue *item = g_ptr_array_index(parent->items, index);
	gsize start = get_item_start(parent, index), end = item->end;
	const char *text = editor->text;
	gsize after = end;
	while (is_blank(text[after]))
		after++;
	if (text[after] == ',') {
		end = after + 1;
	} else {
		gsize before = start;
		while (before > 0 && (is_blank(text[before - 1]) || text[before - 1] == '\n'))
			before--;
		if (before > 0 && text[before - 1] == ',')
			start = before - 1;
	}
	gsize line_end = end;
	while (is_blank(text[line_end]))
		line_end++;
	gsize line_start = start;
	while (line_start > 0 && is_blank(text[line_start - 1]))
		line_start--;
	if (text[line_end] == '\n' && (line_start == 0 || text[line_start - 1] == '\n')) {
		start = line_start;
		end = line_end + 1;
	}
	replace_bytes(editor, start, end, "");
}


/* ==================================== Editing in place ==================================== */


static void on_name_edited(GObject *label, GParamSpec *property, RowTarget *target) {
	if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(label)))
		return;
	JsonEditor *editor = target->editor;
	JsonValue *parent = NULL;
	JsonValue *value = find_value(editor, target->path, &parent);
	guint index = path_index(target->path, target->path->len - 1);
	const char *text = gtk_editable_get_text(GTK_EDITABLE(label));
	if (!value || !parent || !parent->names || strcmp(text, g_ptr_array_index(parent->names, index)) == 0)
		return;
	gsize start = get_item_start(parent, index);
	g_autofree char *quoted = quote(text);
	begin_change(editor);
	replace_bytes(editor, start, find_name_end(editor, start), quoted);
	end_change(editor);
}


/* Text keeps what was typed, and a number that does not read as one goes back to what it was */
static void on_value_edited(GObject *label, GParamSpec *property, RowTarget *target) {
	if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(label)))
		return;
	JsonEditor *editor = target->editor;
	JsonValue *value = find_value(editor, target->path, NULL);
	if (!value)
		return;
	const char *text = gtk_editable_get_text(GTK_EDITABLE(label));
	g_autofree char *literal = NULL;
	if (value->kind == JSON_STRING) {
		if (strcmp(text, value->string) == 0)
			return;
		literal = quote(text);
	} else {
		char *end = NULL;
		g_ascii_strtod(text, &end);
		g_autofree char *old = g_strndup(editor->text + value->start, value->end - value->start);
		if (!*text || !end || *end || strcmp(text, old) == 0) {
			gtk_editable_set_text(GTK_EDITABLE(label), old);
			return;
		}
		literal = g_strdup(text);
	}
	begin_change(editor);
	replace_bytes(editor, value->start, value->end, literal);
	end_change(editor);
}


static void on_switch_toggled(GtkCheckButton *check, RowTarget *target) {
	JsonEditor *editor = target->editor;
	JsonValue *value = find_value(editor, target->path, NULL);
	if (!value || value->kind != JSON_BOOLEAN)
		return;
	begin_change(editor);
	replace_bytes(editor, value->start, value->end, gtk_check_button_get_active(check) ? "true" : "false");
	end_change(editor);
}


static void on_fold_clicked(GtkButton *button, RowTarget *target) {
	JsonEditor *editor = target->editor;
	if (!g_hash_table_remove(editor->collapsed, target->fold_key))
		g_hash_table_add(editor->collapsed, g_strdup(target->fold_key));
	schedule_rebuild(editor);
}


/* ==================================== The row menu ==================================== */


static void on_insert_after(GSimpleAction *action, GVariant *parameter, gpointer data) {
	RowTarget *target = data;
	JsonEditor *editor = target->editor;
	JsonValue *parent = NULL;
	if (!find_value(editor, target->path, &parent) || !parent)
		return;
	begin_change(editor);
	insert_after(editor, parent, path_index(target->path, target->path->len - 1), "\"\"",
		parent->kind == JSON_OBJECT ? TEXT_JSON_NEW_FIELD : NULL);
	end_change(editor);
}


static void on_add_inside(GSimpleAction *action, GVariant *parameter, gpointer data) {
	RowTarget *target = data;
	JsonEditor *editor = target->editor;
	JsonValue *value = find_value(editor, target->path, NULL);
	if (!value || (value->kind != JSON_OBJECT && value->kind != JSON_ARRAY))
		return;
	g_hash_table_remove(editor->collapsed, target->fold_key);
	begin_change(editor);
	insert_into(editor, value, "\"\"");
	end_change(editor);
}


static void on_duplicate(GSimpleAction *action, GVariant *parameter, gpointer data) {
	RowTarget *target = data;
	JsonEditor *editor = target->editor;
	JsonValue *parent = NULL;
	JsonValue *value = find_value(editor, target->path, &parent);
	if (!value || !parent)
		return;
	guint index = path_index(target->path, target->path->len - 1);
	g_autofree char *copy = g_strndup(editor->text + value->start, value->end - value->start);
	const char *name = parent->names ? g_ptr_array_index(parent->names, index) : NULL;
	begin_change(editor);
	insert_after(editor, parent, index, copy, name);
	end_change(editor);
}


static void on_remove(GSimpleAction *action, GVariant *parameter, gpointer data) {
	RowTarget *target = data;
	JsonEditor *editor = target->editor;
	JsonValue *parent = NULL;
	if (!find_value(editor, target->path, &parent) || !parent)
		return;
	begin_change(editor);
	remove_item(editor, parent, path_index(target->path, target->path->len - 1));
	end_change(editor);
}


/* Text that reads as a number keeps its value when it becomes a number, and the other way round */
static char *convert_literal(JsonEditor *editor, const JsonValue *value, guint kind_index) {
	JsonKind kind = KINDS[kind_index].kind;
	g_autofree char *source = g_strndup(editor->text + value->start, value->end - value->start);
	if (kind == JSON_NUMBER && value->kind == JSON_STRING) {
		char *end = NULL;
		g_ascii_strtod(value->string, &end);
		if (*value->string && end && !*end)
			return g_strdup(value->string);
	}
	if (kind == JSON_STRING && (value->kind == JSON_NUMBER || value->kind == JSON_BOOLEAN))
		return quote(source);
	return g_strdup(KINDS[kind_index].literal);
}


static void on_set_kind(GSimpleAction *action, GVariant *parameter, gpointer data) {
	RowTarget *target = data;
	JsonEditor *editor = target->editor;
	JsonValue *value = find_value(editor, target->path, NULL);
	const char *id = g_variant_get_string(parameter, NULL);
	for (guint index = 0; value && index < G_N_ELEMENTS(KINDS); index++) {
		if (strcmp(KINDS[index].id, id) != 0 || value->kind == KINDS[index].kind)
			continue;
		g_autofree char *literal = convert_literal(editor, value, index);
		begin_change(editor);
		replace_bytes(editor, value->start, value->end, literal);
		end_change(editor);
		return;
	}
}


static GMenuModel *create_menu(gboolean is_container, gboolean has_parent) {
	GMenu *menu = g_menu_new(), *adding = g_menu_new(), *kinds = g_menu_new(), *removing = g_menu_new();
	if (has_parent)
		g_menu_append(adding, TEXT_JSON_INSERT_AFTER, "row.insert-after");
	if (is_container)
		g_menu_append(adding, TEXT_JSON_ADD_INSIDE, "row.add-inside");
	if (has_parent)
		g_menu_append(adding, TEXT_JSON_DUPLICATE, "row.duplicate");
	for (guint index = 0; has_parent && index < G_N_ELEMENTS(KINDS); index++) {
		g_autofree char *action = g_strdup_printf("row.set-kind::%s", KINDS[index].id);
		g_menu_append(kinds, KINDS[index].label, action);
	}
	if (has_parent)
		g_menu_append(removing, TEXT_JSON_REMOVE, "row.remove");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(adding));
	if (has_parent)
		g_menu_append_submenu(menu, TEXT_JSON_CHANGE_TO, G_MENU_MODEL(kinds));
	g_menu_append_section(menu, NULL, G_MENU_MODEL(removing));
	g_object_unref(adding);
	g_object_unref(kinds);
	g_object_unref(removing);
	return G_MENU_MODEL(menu);
}


static void add_row_actions(GtkWidget *row, RowTarget *target) {
	static const GActionEntry ENTRIES[] = {
		{"insert-after", on_insert_after, NULL, NULL, NULL, {0}},
		{"add-inside", on_add_inside, NULL, NULL, NULL, {0}},
		{"duplicate", on_duplicate, NULL, NULL, NULL, {0}},
		{"remove", on_remove, NULL, NULL, NULL, {0}},
		{"set-kind", on_set_kind, "s", NULL, NULL, {0}},
	};
	GSimpleActionGroup *actions = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(actions), ENTRIES, G_N_ELEMENTS(ENTRIES), target);
	gtk_widget_insert_action_group(row, "row", G_ACTION_GROUP(actions));
	g_object_unref(actions);
}


/* A right click opens the same menu as the row's button, at the pointer */
static void on_secondary_click(GtkGestureClick *gesture, int presses, double x, double y, GtkWidget *menu_button) {
	GtkPopover *popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(menu_button));
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	graphene_point_t point;
	if (!popover || !gtk_widget_compute_point(row, menu_button, &GRAPHENE_POINT_INIT((float)x, (float)y), &point))
		return;
	gtk_popover_set_pointing_to(popover, &(GdkRectangle){(int)point.x, (int)point.y, 1, 1});
	gtk_menu_button_popup(GTK_MENU_BUTTON(menu_button));
}


/* ==================================== Dragging into order ==================================== */


static GdkContentProvider *on_drag_prepare(GtkDragSource *source, double x, double y, RowTarget *target) {
	g_clear_pointer(&target->editor->drag_path, g_array_unref);
	target->editor->drag_path = g_array_ref(target->path);
	return gdk_content_provider_new_typed(G_TYPE_STRING, target->fold_key);
}


static void on_drag_begin(GtkDragSource *source, GdkDrag *drag, GtkWidget *row) {
	GdkPaintable *paintable = gtk_widget_paintable_new(row);
	gtk_drag_source_set_icon(source, paintable, 0, 0);
	g_object_unref(paintable);
}


static gboolean is_sibling(const GArray *first, const GArray *second) {
	if (!first || first->len != second->len || first->len == 0)
		return FALSE;
	for (guint position = 0; position + 1 < first->len; position++)
		if (path_index(first, position) != path_index(second, position))
			return FALSE;
	return TRUE;
}


/* A dropped member moves to just after the row it is dropped on, among the same siblings only */
static gboolean on_drop(GtkDropTarget *drop, const GValue *value, double x, double y, RowTarget *target) {
	JsonEditor *editor = target->editor;
	GArray *from = editor->drag_path;
	if (!is_sibling(from, target->path))
		return FALSE;
	guint from_index = path_index(from, from->len - 1), to_index = path_index(target->path, target->path->len - 1);
	JsonValue *parent = NULL;
	JsonValue *moved = find_value(editor, from, &parent);
	if (!moved || !parent || from_index == to_index)
		return FALSE;
	gsize start = get_item_start(parent, from_index);
	g_autofree char *member = g_strndup(editor->text + start, moved->end - start);
	g_autofree char *moved_value = g_strndup(editor->text + moved->start, moved->end - moved->start);
	const char *name = parent->names ? g_ptr_array_index(parent->names, from_index) : NULL;
	g_autofree char *name_copy = g_strdup(name);
	begin_change(editor);
	remove_item(editor, parent, from_index);
	JsonValue *new_parent = NULL;
	g_autoptr(GArray) target_path = g_array_copy(target->path);
	guint adjusted = from_index < to_index ? to_index - 1 : to_index;
	g_array_index(target_path, guint, target_path->len - 1) = adjusted;
	/* After removal the dropped-on row may have moved up one, and the member keeps its own name */
	if (find_value(editor, target_path, &new_parent) && new_parent) {
		g_autofree char *quoted = name_copy ? quote(name_copy) : NULL;
		JsonValue *anchor = g_ptr_array_index(new_parent->items, adjusted);
		g_autofree char *indent = get_line_indent(editor, get_item_start(new_parent, adjusted));
		g_autofree char *inserted = g_strdup_printf(indent ? ",\n%s%s%s%s" : ", %s%s%s%s", indent ? indent : "",
			quoted ? quoted : "", quoted ? ": " : "", moved_value);
		replace_bytes(editor, anchor->end, anchor->end, inserted);
	}
	end_change(editor);
	g_clear_pointer(&editor->drag_path, g_array_unref);
	return TRUE;
}


/* ==================================== Building the rows ==================================== */


static void build_container(JsonEditor *editor, JsonValue *container, GArray *path, const char *fold_key,
	int depth);


static GtkWidget *create_part(GtkWidget *widget, const char *css_class) {
	gtk_widget_add_css_class(widget, css_class);
	gtk_widget_set_valign(widget, GTK_ALIGN_CENTER);
	return widget;
}


static GtkWidget *create_label(const char *text, const char *css_class) {
	GtkWidget *label = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	return create_part(label, css_class);
}


static GtkWidget *create_editable(const char *text, const char *css_class, GCallback on_edited, RowTarget *target) {
	GtkWidget *label = gtk_editable_label_new(text);
	/* The label and its entry share one size, which would otherwise be a blank entry's, spacing out the names,
	 * so the entry asks only for the width of its text */
	GtkEditable *entry = gtk_editable_get_delegate(GTK_EDITABLE(label));
	if (GTK_IS_TEXT(entry))
		gtk_text_set_propagate_text_width(GTK_TEXT(entry), TRUE);
	g_signal_connect(label, "notify::editing", on_edited, target);
	return create_part(label, css_class);
}


static char *describe_container(const JsonValue *value) {
	guint count = value->items->len;
	if (value->kind == JSON_OBJECT)
		return g_strdup_printf(g_dngettext(NULL, TEXT_JSON_FIELDS_ONE, TEXT_JSON_FIELDS, count), count);
	return g_strdup_printf(g_dngettext(NULL, TEXT_JSON_ITEMS_ONE, TEXT_JSON_ITEMS, count), count);
}


static GtkWidget *create_value_part(JsonEditor *editor, JsonValue *value, RowTarget *target) {
	switch (value->kind) {
	case JSON_STRING:
		return create_editable(value->string, "json-string", G_CALLBACK(on_value_edited), target);
	case JSON_NUMBER: {
		g_autofree char *source = g_strndup(editor->text + value->start, value->end - value->start);
		return create_editable(source, "json-number", G_CALLBACK(on_value_edited), target);
	}
	case JSON_BOOLEAN: {
		GtkWidget *check = gtk_check_button_new_with_label(value->boolean ? "true" : "false");
		gtk_check_button_set_active(GTK_CHECK_BUTTON(check), value->boolean);
		g_signal_connect(check, "toggled", G_CALLBACK(on_switch_toggled), target);
		return create_part(check, "json-constant");
	}
	case JSON_NULL:
		return create_label("null", "json-constant");
	default: {
		g_autofree char *summary = describe_container(value);
		return create_label(summary, "json-count");
	}
	}
}


/* One row with the fold arrow, the name or position, the value and a menu that shows on hover */
static void add_row(JsonEditor *editor, JsonValue *parent, guint index, GArray *parent_path,
	const char *parent_key, int depth) {
	JsonValue *value = g_ptr_array_index(parent->items, index);
	const char *name = parent->names ? g_ptr_array_index(parent->names, index) : NULL;
	RowTarget *target = g_new0(RowTarget, 1);
	target->editor = editor;
	target->path = g_array_copy(parent_path);
	g_array_append_val(target->path, index);
	target->fold_key = name ? g_strconcat(parent_key, "/", name, NULL) : g_strdup_printf("%s/%u", parent_key, index);
	gboolean is_container = value->kind == JSON_OBJECT || value->kind == JSON_ARRAY;
	gboolean is_folded = g_hash_table_contains(editor->collapsed, target->fold_key);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_add_css_class(row, "json-row");
	gtk_widget_set_margin_start(row, depth * LEVEL_INDENT_PIXELS);
	if (is_container) {
		GtkWidget *fold = gtk_button_new_from_icon_name(is_folded ? "pan-end-symbolic" : "pan-down-symbolic");
		gtk_widget_add_css_class(fold, "flat");
		gtk_widget_add_css_class(fold, "json-fold");
		g_signal_connect(fold, "clicked", G_CALLBACK(on_fold_clicked), target);
		gtk_box_append(GTK_BOX(row), fold);
	} else {
		GtkWidget *space = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_widget_set_size_request(space, FOLD_WIDTH_PIXELS, -1);
		gtk_box_append(GTK_BOX(row), space);
	}
	if (name) {
		gtk_box_append(GTK_BOX(row), create_editable(name, "json-name", G_CALLBACK(on_name_edited), target));
	} else {
		g_autofree char *position = g_strdup_printf("%u", index);
		gtk_box_append(GTK_BOX(row), create_label(position, "json-index"));
	}
	gtk_box_append(GTK_BOX(row), create_label(":", "json-colon"));
	gtk_box_append(GTK_BOX(row), create_value_part(editor, value, target));
	GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(spacer, TRUE);
	gtk_box_append(GTK_BOX(row), spacer);
	GtkWidget *menu_button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "view-more-symbolic");
	g_autoptr(GMenuModel) menu = create_menu(is_container, TRUE);
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), menu);
	gtk_widget_add_css_class(menu_button, "flat");
	gtk_widget_add_css_class(menu_button, "json-menu");
	gtk_box_append(GTK_BOX(row), menu_button);
	add_row_actions(row, target);
	GtkGesture *secondary = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
	g_signal_connect(secondary, "pressed", G_CALLBACK(on_secondary_click), menu_button);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(secondary));
	GtkDragSource *source = gtk_drag_source_new();
	gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
	g_signal_connect(source, "prepare", G_CALLBACK(on_drag_prepare), target);
	g_signal_connect(source, "drag-begin", G_CALLBACK(on_drag_begin), row);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(source));
	GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_MOVE);
	g_signal_connect(drop, "drop", G_CALLBACK(on_drop), target);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(drop));
	g_object_set_data_full(G_OBJECT(row), TARGET_KEY, target, free_target);
	gtk_box_append(GTK_BOX(editor->rows), row);
	if (is_container && !is_folded)
		build_container(editor, value, target->path, target->fold_key, depth + 1);
}


static void build_container(JsonEditor *editor, JsonValue *container, GArray *path, const char *fold_key,
	int depth) {
	for (guint index = 0; index < container->items->len; index++)
		add_row(editor, container, index, path, fold_key, depth);
}


/* Reads the scheme's colours for names, text, numbers and constants, so the tree looks like the code */
static void apply_part_colours(JsonEditor *editor) {
	GtkSourceStyleScheme *scheme = gtk_source_buffer_get_style_scheme(GTK_SOURCE_BUFFER(editor->buffer));
	g_autoptr(GString) css = g_string_new(NULL);
	for (guint part = 0; scheme && part < G_N_ELEMENTS(PART_STYLES); part++)
		for (guint index = 0; PART_STYLES[part].styles[index]; index++) {
			GtkSourceStyle *style = gtk_source_style_scheme_get_style(scheme, PART_STYLES[part].styles[index]);
			g_autofree char *colour = NULL;
			gboolean is_set = FALSE;
			if (style)
				g_object_get(style, "foreground", &colour, "foreground-set", &is_set, NULL);
			if (!is_set || !colour)
				continue;
			g_string_append_printf(css, ".json-tree .%s { color: %s; }\n", PART_STYLES[part].css_class, colour);
			break;
		}
	if (!part_style) {
		part_style = gtk_css_provider_new();
		gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(part_style),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
	}
	gtk_css_provider_load_from_string(part_style, css->str);
}


static void rebuild(JsonEditor *editor) {
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(editor->rows)))
		gtk_box_remove(GTK_BOX(editor->rows), child);
	gboolean is_container = editor->root && (editor->root->kind == JSON_OBJECT || editor->root->kind == JSON_ARRAY);
	if (!is_container && editor->root)
		gtk_label_set_text(GTK_LABEL(editor->problem), TEXT_JSON_NOT_CONTAINER);
	gtk_stack_set_visible_child_name(GTK_STACK(editor->stack), is_container ? "rows" : "problem");
	if (!is_container)
		return;
	apply_part_colours(editor);
	g_autoptr(GArray) path = g_array_new(FALSE, FALSE, sizeof(guint));
	build_container(editor, editor->root, path, "", 0);
	if (editor->root->items->len == 0) {
		GtkWidget *add = gtk_button_new_with_label(editor->root->kind == JSON_OBJECT ? TEXT_JSON_ADD_FIELD
			: TEXT_JSON_ADD_ITEM);
		gtk_widget_add_css_class(add, "flat");
		gtk_widget_set_halign(add, GTK_ALIGN_START);
		gtk_actionable_set_action_name(GTK_ACTIONABLE(add), "tree.add-first");
		gtk_box_append(GTK_BOX(editor->rows), add);
	}
}


static gboolean on_rebuild_due(gpointer data) {
	JsonEditor *editor = data;
	editor->rebuild_source = 0;
	read_text(editor);
	rebuild(editor);
	return G_SOURCE_REMOVE;
}


/* Rows are rebuilt after the click, edit or drop that changed the text has finished with its own row */
static void schedule_rebuild(JsonEditor *editor) {
	if (!editor->rebuild_source)
		editor->rebuild_source = g_idle_add(on_rebuild_due, editor);
}


/* Undo, reloading or the text view change the text, which the rows then follow */
static void on_text_changed(GtkTextBuffer *buffer, JsonEditor *editor) {
	if (!editor->is_writing && gtk_widget_get_mapped(editor->stack))
		schedule_rebuild(editor);
}


static void on_add_first(GSimpleAction *action, GVariant *parameter, gpointer data) {
	JsonEditor *editor = data;
	if (!editor->root)
		return;
	begin_change(editor);
	insert_into(editor, editor->root, "\"\"");
	end_change(editor);
}


/* ==================================== Lifetime ==================================== */


GtkWidget *create_json_editor(GtkTextBuffer *buffer) {
	JsonEditor *editor = g_new0(JsonEditor, 1);
	editor->buffer = buffer;
	editor->indent = g_strdup(DEFAULT_INDENT);
	editor->collapsed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	editor->rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(editor->rows, "json-tree");
	gtk_widget_add_css_class(editor->rows, "code-text");
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), editor->rows);
	gtk_widget_set_vexpand(scroller, TRUE);
	editor->problem = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(editor->problem), TRUE);
	gtk_widget_add_css_class(editor->problem, "dim-label");
	editor->stack = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(editor->stack), scroller, "rows");
	gtk_stack_add_named(GTK_STACK(editor->stack), editor->problem, "problem");
	gtk_widget_add_css_class(editor->stack, "json-page");
	static const GActionEntry ENTRIES[] = {{"add-first", on_add_first, NULL, NULL, NULL, {0}}};
	GSimpleActionGroup *actions = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(actions), ENTRIES, G_N_ELEMENTS(ENTRIES), editor);
	gtk_widget_insert_action_group(editor->stack, "tree", G_ACTION_GROUP(actions));
	g_object_unref(actions);
	g_object_set_data(G_OBJECT(editor->stack), EDITOR_KEY, editor);
	g_signal_connect(buffer, "changed", G_CALLBACK(on_text_changed), editor);
	return editor->stack;
}


void reload_json_editor(GtkWidget *widget) {
	JsonEditor *editor = get_editor(widget);
	g_clear_handle_id(&editor->rebuild_source, g_source_remove);
	read_text(editor);
	rebuild(editor);
}


void set_json_editor_indent(GtkWidget *widget, const char *indent) {
	JsonEditor *editor = get_editor(widget);
	g_free(editor->indent);
	editor->indent = g_strdup(indent);
}


void free_json_editor(GtkWidget *widget) {
	JsonEditor *editor = g_object_steal_data(G_OBJECT(widget), EDITOR_KEY);
	if (!editor)
		return;
	g_signal_handlers_disconnect_by_data(editor->buffer, editor);
	g_clear_handle_id(&editor->rebuild_source, g_source_remove);
	g_clear_pointer(&editor->root, free_json);
	g_clear_pointer(&editor->drag_path, g_array_unref);
	g_hash_table_unref(editor->collapsed);
	g_free(editor->text);
	g_free(editor->indent);
	g_free(editor);
}
