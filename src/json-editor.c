#include <string.h>

#include <adwaita.h>
#include <gtksourceview/gtksource.h>

#include "../lang/en.h"
#include "json-editor.h"
#include "build/json.h"
#include "common/loading.h"

#define EDITOR_KEY "json-editor"
#define PARTS_KEY "json-row-parts"
#define LEVEL_INDENT_PIXELS 18
#define FOLD_WIDTH_PIXELS 18
#define DEFAULT_INDENT "\t"
#define NO_PARENT G_MAXUINT

/* Hands the list view a placeholder for each shown row, since rows read their value by position
 * so a file with a million values only ever builds the few rows on screen */
#define TREE_ROWS_TYPE (tree_rows_get_type())
G_DECLARE_FINAL_TYPE(TreeRows, tree_rows, CRIMSON, TREE_ROWS, GObject)

struct _TreeRows {
	GObject parent_instance;
	guint count;
};

/* One shown line of the tree, pointing into the parsed value it came from */
typedef struct {
	JsonValue *value, *parent;
	guint index;
	/* The row of the group or list holding this value, or NO_PARENT at the top */
	guint parent_row;
	int depth;
	gboolean is_folded;
} TreeRow;

/* A compact tree with one line per value in the code font and the scheme's colours, edited in place with a click
 * The text stays the source, so comments, spacing and undo all survive */
typedef struct {
	GtkTextBuffer *buffer;
	/* The text the tree was read from, which the recorded byte offsets point into */
	char *text;
	JsonValue *root;
	/* The text and values the shown rows point into, kept until the next rows replace them */
	char *shown_text;
	JsonValue *shown_root;
	GArray *shown;
	TreeRows *model;
	GtkWidget *stack, *problem, *add_first;
	/* Paths of groups and lists the person folded, kept across rebuilds */
	GHashTable *collapsed;
	char *indent;
	guint rebuild_source;
	/* Set while this editor changes the text, so its own change does not read the text back too early */
	gboolean is_writing;
	/* Cancels the parse running on a worker, whose result is out of date once the text is read again */
	GCancellable *reading;
	JsonEditorReady on_ready;
	gpointer ready_data;
	/* Set while the rows come from the file on disk, as the buffer is still loading and offsets would not match */
	gboolean is_preview;
	/* The index path of the row being dragged */
	GArray *drag_path;
} JsonEditor;

/* The text a worker parses, or the file it reads it from first, handed back to the editor once it is done */
typedef struct {
	char *text;
	GFile *file;
} ParseJob;

/* The widgets of one reused row, which shows the value its list item is bound to */
typedef struct {
	JsonEditor *editor;
	GtkListItem *list_item;
	GtkWidget *row, *fold, *fold_space, *colon, *menu_button;
	/* Made the first time the row shows that kind, and NULL until then */
	GtkWidget *name, *position, *text_value, *number_value, *switch_value, *plain_value;
	/* Set while the row is filled in, so filling a check box does not count as a click */
	gboolean is_binding;
} RowParts;

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


static guint path_index(const GArray *path, guint position) {
	return g_array_index(path, guint, position);
}


static gboolean is_container_value(const JsonValue *value) {
	return value && (value->kind == JSON_OBJECT || value->kind == JSON_ARRAY);
}


static TreeRow *get_tree_row(GArray *rows, guint position) {
	return &g_array_index(rows, TreeRow, position);
}


static const char *get_row_name(const TreeRow *row) {
	return row->parent->names ? g_ptr_array_index(row->parent->names, row->index) : NULL;
}


/* The names and positions from the top down to a row, which is how folds are remembered across rebuilds */
static char *get_fold_key(GArray *rows, guint position) {
	TreeRow *row = get_tree_row(rows, position);
	g_autofree char *above = row->parent_row == NO_PARENT ? g_strdup("") : get_fold_key(rows, row->parent_row);
	const char *name = get_row_name(row);
	return name ? g_strconcat(above, "/", name, NULL) : g_strdup_printf("%s/%u", above, row->index);
}


/* ==================================== The row model ==================================== */


static GType tree_rows_get_item_type(GListModel *model) {
	return G_TYPE_OBJECT;
}


static guint tree_rows_get_n_items(GListModel *model) {
	return CRIMSON_TREE_ROWS(model)->count;
}


static gpointer tree_rows_get_item(GListModel *model, guint position) {
	return position < CRIMSON_TREE_ROWS(model)->count ? g_object_new(G_TYPE_OBJECT, NULL) : NULL;
}


static void tree_rows_list_model_init(GListModelInterface *interface) {
	interface->get_item_type = tree_rows_get_item_type;
	interface->get_n_items = tree_rows_get_n_items;
	interface->get_item = tree_rows_get_item;
}


G_DEFINE_FINAL_TYPE_WITH_CODE(TreeRows, tree_rows, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, tree_rows_list_model_init))


static void tree_rows_class_init(TreeRowsClass *class) {
}


static void tree_rows_init(TreeRows *rows) {
}


/* ==================================== Finding a row's value ==================================== */


/* The index path from the top to the value a row shows, or NULL while the row is not bound to one */
static GArray *get_row_path(RowParts *parts) {
	JsonEditor *editor = parts->editor;
	guint position = gtk_list_item_get_position(parts->list_item);
	if (position >= editor->shown->len)
		return NULL;
	GArray *path = g_array_new(FALSE, FALSE, sizeof(guint));
	for (guint at = position; at != NO_PARENT; at = get_tree_row(editor->shown, at)->parent_row)
		g_array_prepend_val(path, get_tree_row(editor->shown, at)->index);
	return path;
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


static char *get_buffer_text(JsonEditor *editor) {
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(editor->buffer, &start, &end);
	return gtk_text_buffer_get_text(editor->buffer, &start, &end, FALSE);
}


/* Takes over a newly read text and what it parsed to, while the shown rows keep pointing into the previous text
 * until show_rows replaces them */
static void take_text(JsonEditor *editor, char *text, JsonValue *root, const GError *error) {
	if (editor->text != editor->shown_text) {
		g_free(editor->text);
		g_clear_pointer(&editor->root, free_json);
	}
	editor->text = text;
	editor->root = root;
	if (!root) {
		g_autofree char *message = g_strdup_printf(TEXT_JSON_NOT_VALID, error ? error->message : "");
		gtk_label_set_text(GTK_LABEL(editor->problem), message);
	}
}


static void cancel_reading(JsonEditor *editor) {
	if (editor->reading)
		g_cancellable_cancel(editor->reading);
	g_clear_object(&editor->reading);
}


/* Reads and parses at once, for edits that need fresh offsets before their next step */
static void read_text(JsonEditor *editor) {
	cancel_reading(editor);
	char *text = get_buffer_text(editor);
	g_autoptr(GError) error = NULL;
	JsonValue *root = parse_json(text, -1, &error);
	take_text(editor, text, root, error);
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


static void on_name_edited(GObject *label, GParamSpec *property, RowParts *parts) {
	if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(label)))
		return;
	JsonEditor *editor = parts->editor;
	g_autoptr(GArray) path = get_row_path(parts);
	JsonValue *parent = NULL;
	JsonValue *value = path ? find_value(editor, path, &parent) : NULL;
	if (!value || !parent || !parent->names)
		return;
	guint index = path_index(path, path->len - 1);
	const char *text = gtk_editable_get_text(GTK_EDITABLE(label));
	if (strcmp(text, g_ptr_array_index(parent->names, index)) == 0)
		return;
	gsize start = get_item_start(parent, index);
	g_autofree char *quoted = quote(text);
	begin_change(editor);
	replace_bytes(editor, start, find_name_end(editor, start), quoted);
	end_change(editor);
}


/* Text keeps what was typed, and a number that does not read as one goes back to what it was */
static void on_value_edited(GObject *label, GParamSpec *property, RowParts *parts) {
	if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(label)))
		return;
	JsonEditor *editor = parts->editor;
	g_autoptr(GArray) path = get_row_path(parts);
	JsonValue *value = path ? find_value(editor, path, NULL) : NULL;
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


static void on_switch_toggled(GtkCheckButton *check, RowParts *parts) {
	if (parts->is_binding)
		return;
	JsonEditor *editor = parts->editor;
	g_autoptr(GArray) path = get_row_path(parts);
	JsonValue *value = path ? find_value(editor, path, NULL) : NULL;
	if (!value || value->kind != JSON_BOOLEAN)
		return;
	begin_change(editor);
	replace_bytes(editor, value->start, value->end, gtk_check_button_get_active(check) ? "true" : "false");
	end_change(editor);
}


static void show_rows(JsonEditor *editor);


static void on_fold_clicked(GtkButton *button, RowParts *parts) {
	JsonEditor *editor = parts->editor;
	guint position = gtk_list_item_get_position(parts->list_item);
	if (position >= editor->shown->len)
		return;
	g_autofree char *key = get_fold_key(editor->shown, position);
	if (!g_hash_table_remove(editor->collapsed, key))
		g_hash_table_add(editor->collapsed, g_steal_pointer(&key));
	show_rows(editor);
}


/* ==================================== The row menu ==================================== */


static void on_insert_after(GSimpleAction *action, GVariant *parameter, gpointer data) {
	JsonEditor *editor = ((RowParts *)data)->editor;
	g_autoptr(GArray) path = get_row_path(data);
	JsonValue *parent = NULL;
	if (!path || !find_value(editor, path, &parent) || !parent)
		return;
	begin_change(editor);
	insert_after(editor, parent, path_index(path, path->len - 1), "\"\"",
		parent->kind == JSON_OBJECT ? TEXT_JSON_NEW_FIELD : NULL);
	end_change(editor);
}


static void on_add_inside(GSimpleAction *action, GVariant *parameter, gpointer data) {
	RowParts *parts = data;
	JsonEditor *editor = parts->editor;
	g_autoptr(GArray) path = get_row_path(parts);
	JsonValue *value = path ? find_value(editor, path, NULL) : NULL;
	if (!is_container_value(value))
		return;
	g_autofree char *key = get_fold_key(editor->shown, gtk_list_item_get_position(parts->list_item));
	g_hash_table_remove(editor->collapsed, key);
	begin_change(editor);
	insert_into(editor, value, "\"\"");
	end_change(editor);
}


static void on_duplicate(GSimpleAction *action, GVariant *parameter, gpointer data) {
	JsonEditor *editor = ((RowParts *)data)->editor;
	g_autoptr(GArray) path = get_row_path(data);
	JsonValue *parent = NULL;
	JsonValue *value = path ? find_value(editor, path, &parent) : NULL;
	if (!value || !parent)
		return;
	guint index = path_index(path, path->len - 1);
	g_autofree char *copy = g_strndup(editor->text + value->start, value->end - value->start);
	const char *name = parent->names ? g_ptr_array_index(parent->names, index) : NULL;
	begin_change(editor);
	insert_after(editor, parent, index, copy, name);
	end_change(editor);
}


static void on_remove(GSimpleAction *action, GVariant *parameter, gpointer data) {
	JsonEditor *editor = ((RowParts *)data)->editor;
	g_autoptr(GArray) path = get_row_path(data);
	JsonValue *parent = NULL;
	if (!path || !find_value(editor, path, &parent) || !parent)
		return;
	begin_change(editor);
	remove_item(editor, parent, path_index(path, path->len - 1));
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
	JsonEditor *editor = ((RowParts *)data)->editor;
	g_autoptr(GArray) path = get_row_path(data);
	JsonValue *value = path ? find_value(editor, path, NULL) : NULL;
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


static GMenuModel *create_menu(gboolean is_container) {
	GMenu *menu = g_menu_new(), *adding = g_menu_new(), *kinds = g_menu_new(), *removing = g_menu_new();
	g_menu_append(adding, TEXT_JSON_INSERT_AFTER, "row.insert-after");
	if (is_container)
		g_menu_append(adding, TEXT_JSON_ADD_INSIDE, "row.add-inside");
	g_menu_append(adding, TEXT_JSON_DUPLICATE, "row.duplicate");
	for (guint index = 0; index < G_N_ELEMENTS(KINDS); index++) {
		g_autofree char *action = g_strdup_printf("row.set-kind::%s", KINDS[index].id);
		g_menu_append(kinds, KINDS[index].label, action);
	}
	g_menu_append(removing, TEXT_JSON_REMOVE, "row.remove");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(adding));
	g_menu_append_submenu(menu, TEXT_JSON_CHANGE_TO, G_MENU_MODEL(kinds));
	g_menu_append_section(menu, NULL, G_MENU_MODEL(removing));
	g_object_unref(adding);
	g_object_unref(kinds);
	g_object_unref(removing);
	return G_MENU_MODEL(menu);
}


/* Rows share one menu for groups and lists and one for plain values, since the actions find the row themselves */
static GMenuModel *get_menu(gboolean is_container) {
	static GMenuModel *container_menu, *value_menu;
	if (!container_menu) {
		container_menu = create_menu(TRUE);
		value_menu = create_menu(FALSE);
	}
	return is_container ? container_menu : value_menu;
}


static void add_row_actions(RowParts *parts) {
	static const GActionEntry ENTRIES[] = {
		{"insert-after", on_insert_after, NULL, NULL, NULL, {0}},
		{"add-inside", on_add_inside, NULL, NULL, NULL, {0}},
		{"duplicate", on_duplicate, NULL, NULL, NULL, {0}},
		{"remove", on_remove, NULL, NULL, NULL, {0}},
		{"set-kind", on_set_kind, "s", NULL, NULL, {0}},
	};
	GSimpleActionGroup *actions = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(actions), ENTRIES, G_N_ELEMENTS(ENTRIES), parts);
	gtk_widget_insert_action_group(parts->row, "row", G_ACTION_GROUP(actions));
	g_object_unref(actions);
}


/* The menu is built when first opened instead of for every row shown, since building one is slow */
static void on_menu_needed(GtkMenuButton *button, gpointer data) {
	RowParts *parts = data;
	JsonEditor *editor = parts->editor;
	guint position = gtk_list_item_get_position(parts->list_item);
	gboolean is_container = position < editor->shown->len
		&& is_container_value(get_tree_row(editor->shown, position)->value);
	gtk_menu_button_set_menu_model(button, get_menu(is_container));
}


/* A right click opens the same menu as the row's button, at the pointer */
static void on_secondary_click(GtkGestureClick *gesture, int presses, double x, double y, RowParts *parts) {
	GtkWidget *menu_button = parts->menu_button;
	on_menu_needed(GTK_MENU_BUTTON(menu_button), parts);
	GtkPopover *popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(menu_button));
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	graphene_point_t point;
	if (!popover || !gtk_widget_compute_point(row, menu_button, &GRAPHENE_POINT_INIT((float)x, (float)y), &point))
		return;
	gtk_popover_set_pointing_to(popover, &(GdkRectangle){(int)point.x, (int)point.y, 1, 1});
	gtk_menu_button_popup(GTK_MENU_BUTTON(menu_button));
}


/* ==================================== Dragging into order ==================================== */


static GdkContentProvider *on_drag_prepare(GtkDragSource *source, double x, double y, RowParts *parts) {
	JsonEditor *editor = parts->editor;
	g_clear_pointer(&editor->drag_path, g_array_unref);
	editor->drag_path = get_row_path(parts);
	if (!editor->drag_path)
		return NULL;
	g_autofree char *key = get_fold_key(editor->shown, gtk_list_item_get_position(parts->list_item));
	return gdk_content_provider_new_typed(G_TYPE_STRING, key);
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
static gboolean on_drop(GtkDropTarget *drop, const GValue *value, double x, double y, RowParts *parts) {
	JsonEditor *editor = parts->editor;
	GArray *from = editor->drag_path;
	g_autoptr(GArray) target_path = get_row_path(parts);
	if (!target_path || !is_sibling(from, target_path))
		return FALSE;
	guint from_index = path_index(from, from->len - 1), to_index = path_index(target_path, target_path->len - 1);
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


static GtkWidget *create_part(GtkWidget *widget, const char *css_class) {
	gtk_widget_add_css_class(widget, css_class);
	gtk_widget_set_valign(widget, GTK_ALIGN_CENTER);
	return widget;
}


static GtkWidget *create_label(const char *css_class) {
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	return create_part(label, css_class);
}


static GtkWidget *create_editable(const char *css_class, GCallback on_edited, RowParts *parts) {
	GtkWidget *label = gtk_editable_label_new("");
	/* The label and its entry share one size, which would otherwise be a blank entry's, spacing out the names,
	 * so the entry asks only for the width of its text */
	GtkEditable *entry = gtk_editable_get_delegate(GTK_EDITABLE(label));
	if (GTK_IS_TEXT(entry))
		gtk_text_set_propagate_text_width(GTK_TEXT(entry), TRUE);
	g_signal_connect(label, "notify::editing", on_edited, parts);
	return create_part(label, css_class);
}


static void add_drag_and_drop(RowParts *parts) {
	GtkDragSource *source = gtk_drag_source_new();
	gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
	g_signal_connect(source, "prepare", G_CALLBACK(on_drag_prepare), parts);
	g_signal_connect(source, "drag-begin", G_CALLBACK(on_drag_begin), parts->row);
	gtk_widget_add_controller(parts->row, GTK_EVENT_CONTROLLER(source));
	GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_MOVE);
	g_signal_connect(drop, "drop", G_CALLBACK(on_drop), parts);
	gtk_widget_add_controller(parts->row, GTK_EVENT_CONTROLLER(drop));
}


static GtkWidget *create_name_part(RowParts *parts) {
	return create_editable("json-name", G_CALLBACK(on_name_edited), parts);
}


static GtkWidget *create_position_part(RowParts *parts) {
	return create_label("json-index");
}


static GtkWidget *create_text_part(RowParts *parts) {
	return create_editable("json-string", G_CALLBACK(on_value_edited), parts);
}


static GtkWidget *create_number_part(RowParts *parts) {
	return create_editable("json-number", G_CALLBACK(on_value_edited), parts);
}


static GtkWidget *create_switch_part(RowParts *parts) {
	GtkWidget *check = create_part(gtk_check_button_new(), "json-constant");
	g_signal_connect(check, "toggled", G_CALLBACK(on_switch_toggled), parts);
	return check;
}


static GtkWidget *create_plain_part(RowParts *parts) {
	return create_label("json-constant");
}


/* Shows a row's name or value widget, making it the first time the row needs that kind, since most rows only
 * ever show one or two of the kinds and making all of them slowed down opening */
static GtkWidget *show_part(RowParts *parts, GtkWidget **part, GtkWidget *(*create)(RowParts *parts),
	GtkWidget *after) {
	if (!*part) {
		*part = create(parts);
		gtk_box_insert_child_after(GTK_BOX(parts->row), *part, after);
	}
	gtk_widget_set_visible(*part, TRUE);
	return *part;
}


/* One reused row with the fold arrow, the name or position, the value and a menu that shows on hover */
static void set_up_row(GtkSignalListItemFactory *factory, GtkListItem *list_item, JsonEditor *editor) {
	RowParts *parts = g_new0(RowParts, 1);
	parts->editor = editor;
	parts->list_item = list_item;
	parts->row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_add_css_class(parts->row, "json-row");
	parts->fold = gtk_button_new();
	gtk_widget_add_css_class(parts->fold, "flat");
	gtk_widget_add_css_class(parts->fold, "json-fold");
	g_signal_connect(parts->fold, "clicked", G_CALLBACK(on_fold_clicked), parts);
	parts->fold_space = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_size_request(parts->fold_space, FOLD_WIDTH_PIXELS, -1);
	parts->colon = create_label("json-colon");
	gtk_label_set_text(GTK_LABEL(parts->colon), ":");
	GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(spacer, TRUE);
	parts->menu_button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(parts->menu_button), "view-more-symbolic");
	gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(parts->menu_button), on_menu_needed, parts, NULL);
	gtk_widget_add_css_class(parts->menu_button, "flat");
	gtk_widget_add_css_class(parts->menu_button, "json-menu");
	GtkWidget *children[] = {parts->fold, parts->fold_space, parts->colon, spacer, parts->menu_button};
	for (guint index = 0; index < G_N_ELEMENTS(children); index++)
		gtk_box_append(GTK_BOX(parts->row), children[index]);
	add_row_actions(parts);
	GtkGesture *secondary = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
	g_signal_connect(secondary, "pressed", G_CALLBACK(on_secondary_click), parts);
	gtk_widget_add_controller(parts->row, GTK_EVENT_CONTROLLER(secondary));
	add_drag_and_drop(parts);
	g_object_set_data_full(G_OBJECT(parts->row), PARTS_KEY, parts, g_free);
	gtk_list_item_set_child(list_item, parts->row);
	gtk_list_item_set_activatable(list_item, FALSE);
}


static char *describe_container(const JsonValue *value) {
	guint count = value->items->len;
	if (value->kind == JSON_OBJECT)
		return g_strdup_printf(g_dngettext(NULL, TEXT_JSON_FIELDS_ONE, TEXT_JSON_FIELDS, count), count);
	return g_strdup_printf(g_dngettext(NULL, TEXT_JSON_ITEMS_ONE, TEXT_JSON_ITEMS, count), count);
}


/* Shows the one value widget that fits the value's kind and fills it in */
static void bind_value(RowParts *parts, JsonValue *value) {
	if (value->kind == JSON_STRING) {
		GtkWidget *text = show_part(parts, &parts->text_value, create_text_part, parts->colon);
		gtk_editable_set_text(GTK_EDITABLE(text), value->string);
	} else if (value->kind == JSON_NUMBER) {
		g_autofree char *source = g_strndup(parts->editor->shown_text + value->start, value->end - value->start);
		GtkWidget *number = show_part(parts, &parts->number_value, create_number_part, parts->colon);
		gtk_editable_set_text(GTK_EDITABLE(number), source);
	} else if (value->kind == JSON_BOOLEAN) {
		GtkWidget *check = show_part(parts, &parts->switch_value, create_switch_part, parts->colon);
		gtk_check_button_set_label(GTK_CHECK_BUTTON(check), value->boolean ? "true" : "false");
		gtk_check_button_set_active(GTK_CHECK_BUTTON(check), value->boolean);
	} else {
		GtkWidget *label = show_part(parts, &parts->plain_value, create_plain_part, parts->colon);
		g_autofree char *summary = value->kind == JSON_NULL ? g_strdup("null") : describe_container(value);
		gtk_label_set_text(GTK_LABEL(label), summary);
		gtk_widget_remove_css_class(label, value->kind == JSON_NULL ? "json-count" : "json-constant");
		gtk_widget_add_css_class(label, value->kind == JSON_NULL ? "json-constant" : "json-count");
	}
}


static void bind_row(GtkSignalListItemFactory *factory, GtkListItem *list_item, JsonEditor *editor) {
	RowParts *parts = g_object_get_data(G_OBJECT(gtk_list_item_get_child(list_item)), PARTS_KEY);
	guint position = gtk_list_item_get_position(list_item);
	if (position >= editor->shown->len)
		return;
	TreeRow *row = get_tree_row(editor->shown, position);
	gboolean is_container = is_container_value(row->value);
	const char *name = get_row_name(row);
	parts->is_binding = TRUE;
	gtk_widget_set_margin_start(parts->row, row->depth * LEVEL_INDENT_PIXELS);
	gtk_widget_set_visible(parts->fold, is_container);
	gtk_widget_set_visible(parts->fold_space, !is_container);
	gtk_button_set_icon_name(GTK_BUTTON(parts->fold), row->is_folded ? "pan-end-symbolic" : "pan-down-symbolic");
	GtkWidget *optional[] = {parts->name, parts->position, parts->text_value, parts->number_value,
		parts->switch_value, parts->plain_value};
	for (guint index = 0; index < G_N_ELEMENTS(optional); index++)
		if (optional[index])
			gtk_widget_set_visible(optional[index], FALSE);
	if (name) {
		GtkWidget *label = show_part(parts, &parts->name, create_name_part, parts->fold_space);
		gtk_editable_set_text(GTK_EDITABLE(label), name);
	} else {
		g_autofree char *index = g_strdup_printf("%u", row->index);
		GtkWidget *label = show_part(parts, &parts->position, create_position_part, parts->fold_space);
		gtk_label_set_text(GTK_LABEL(label), index);
	}
	bind_value(parts, row->value);
	/* A row whose menu was already opened keeps it, so it is swapped when the row now shows the other kind */
	GMenuModel *menu = gtk_menu_button_get_menu_model(GTK_MENU_BUTTON(parts->menu_button));
	if (menu && menu != get_menu(is_container))
		gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(parts->menu_button), get_menu(is_container));
	parts->is_binding = FALSE;
}


/* Lists every value that is not inside a folded group, in reading order, without making any widgets */
static void add_rows(JsonEditor *editor, GArray *rows, JsonValue *container, guint parent_row, int depth) {
	gboolean has_folds = g_hash_table_size(editor->collapsed) > 0;
	for (guint index = 0; index < container->items->len; index++) {
		JsonValue *value = g_ptr_array_index(container->items, index);
		TreeRow row = {value, container, index, parent_row, depth, FALSE};
		g_array_append_val(rows, row);
		if (!is_container_value(value))
			continue;
		guint position = rows->len - 1;
		if (has_folds) {
			g_autofree char *key = get_fold_key(rows, position);
			get_tree_row(rows, position)->is_folded = g_hash_table_contains(editor->collapsed, key);
		}
		if (!get_tree_row(rows, position)->is_folded)
			add_rows(editor, rows, value, position, depth + 1);
	}
}


/* Whether a shown row would look the same for the newly read value, so it need not be filled in again */
static gboolean is_same_row(JsonEditor *editor, const TreeRow *old, const TreeRow *new) {
	if (old->depth != new->depth || old->index != new->index || old->is_folded != new->is_folded
		|| old->value->kind != new->value->kind || g_strcmp0(get_row_name(old), get_row_name(new)) != 0)
		return FALSE;
	if (is_container_value(new->value))
		return old->value->items->len == new->value->items->len;
	gsize length = new->value->end - new->value->start;
	return old->value->end - old->value->start == length
		&& memcmp(editor->shown_text + old->value->start, editor->text + new->value->start, length) == 0;
}


/* Swaps in the rows of the text read last, telling the list only about the stretch that changed
 * so the scroll position and every unchanged row stay as they were */
static void show_rows(JsonEditor *editor) {
	GArray *rows = g_array_new(FALSE, FALSE, sizeof(TreeRow));
	if (is_container_value(editor->root))
		add_rows(editor, rows, editor->root, NO_PARENT, 0);
	GArray *old_rows = editor->shown;
	guint old_count = old_rows->len, new_count = rows->len, same_start = 0, same_end = 0;
	while (same_start < old_count && same_start < new_count
		&& is_same_row(editor, get_tree_row(old_rows, same_start), get_tree_row(rows, same_start)))
		same_start++;
	while (same_end < old_count - same_start && same_end < new_count - same_start
		&& is_same_row(editor, get_tree_row(old_rows, old_count - 1 - same_end),
			get_tree_row(rows, new_count - 1 - same_end)))
		same_end++;
	editor->shown = rows;
	g_array_unref(old_rows);
	if (editor->shown_text != editor->text) {
		g_clear_pointer(&editor->shown_root, free_json);
		g_free(editor->shown_text);
	}
	editor->shown_root = editor->root;
	editor->shown_text = editor->text;
	editor->model->count = new_count;
	g_list_model_items_changed(G_LIST_MODEL(editor->model), same_start, old_count - same_start - same_end,
		new_count - same_start - same_end);
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


/* Rows ignore the pointer and keyboard instead of greying out, so they look the same while they wait */
static void set_rows_usable(JsonEditor *editor, gboolean is_usable) {
	gtk_widget_set_can_target(editor->stack, is_usable);
	gtk_widget_set_can_focus(editor->stack, is_usable);
}


static void rebuild(JsonEditor *editor) {
	gboolean is_container = is_container_value(editor->root);
	if (!is_container && editor->root)
		gtk_label_set_text(GTK_LABEL(editor->problem), TEXT_JSON_NOT_CONTAINER);
	if (is_container)
		gtk_button_set_label(GTK_BUTTON(editor->add_first), editor->root->kind == JSON_OBJECT ? TEXT_JSON_ADD_FIELD
			: TEXT_JSON_ADD_ITEM);
	apply_part_colours(editor);
	show_rows(editor);
	const char *page = !is_container ? "problem" : editor->root->items->len == 0 ? "empty" : "rows";
	gtk_stack_set_visible_child_name(GTK_STACK(editor->stack), page);
	set_rows_usable(editor, !editor->is_preview);
}


static void free_parse_job(gpointer data) {
	ParseJob *job = data;
	g_free(job->text);
	g_clear_object(&job->file);
	g_free(job);
}


static void parse_in_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	ParseJob *job = data;
	g_autoptr(GError) error = NULL;
	if (job->file && !g_file_load_contents(job->file, cancellable, &job->text, NULL, NULL, &error)) {
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	JsonValue *root = parse_json(job->text, -1, &error);
	if (!root) {
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	g_task_return_pointer(task, root, (GDestroyNotify)free_json);
}


static void on_text_parsed(GObject *source, GAsyncResult *result, gpointer data) {
	GTask *task = G_TASK(result);
	if (g_cancellable_is_cancelled(g_task_get_cancellable(task)))
		return;
	JsonEditor *editor = data;
	g_clear_object(&editor->reading);
	g_autoptr(GError) error = NULL;
	JsonValue *root = g_task_propagate_pointer(task, &error);
	ParseJob *job = g_task_get_task_data(task);
	take_text(editor, job->text ? g_steal_pointer(&job->text) : g_strdup(""), root, error);
	rebuild(editor);
	editor->on_ready(editor->ready_data);
}


/* Big files take a moment to parse, so that runs on a worker while the rows already shown wait unclickable,
 * since their offsets point into the text as it was */
static void start_reading(JsonEditor *editor, ParseJob *job) {
	cancel_reading(editor);
	editor->reading = g_cancellable_new();
	if (editor->shown->len == 0)
		gtk_stack_set_visible_child_name(GTK_STACK(editor->stack), "loading");
	set_rows_usable(editor, FALSE);
	GTask *task = g_task_new(NULL, editor->reading, on_text_parsed, editor);
	g_task_set_task_data(task, job, free_parse_job);
	g_task_run_in_thread(task, parse_in_thread);
	g_object_unref(task);
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
	if (!editor->is_writing && !editor->is_preview && gtk_widget_get_mapped(editor->stack))
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


static GtkWidget *create_rows_view(JsonEditor *editor) {
	editor->model = g_object_new(TREE_ROWS_TYPE, NULL);
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(set_up_row), editor);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_row), editor);
	GtkWidget *view = gtk_list_view_new(GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(editor->model))),
		factory);
	gtk_widget_add_css_class(view, "json-tree");
	gtk_widget_add_css_class(view, "code-text");
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
	gtk_widget_set_vexpand(scroller, TRUE);
	return scroller;
}


static GtkWidget *create_empty_page(JsonEditor *editor) {
	editor->add_first = gtk_button_new();
	gtk_widget_add_css_class(editor->add_first, "flat");
	gtk_widget_set_halign(editor->add_first, GTK_ALIGN_START);
	gtk_widget_set_valign(editor->add_first, GTK_ALIGN_START);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(editor->add_first), "tree.add-first");
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(page, "json-tree");
	gtk_box_append(GTK_BOX(page), editor->add_first);
	return page;
}


GtkWidget *create_json_editor(GtkTextBuffer *buffer, JsonEditorReady on_ready, gpointer data) {
	JsonEditor *editor = g_new0(JsonEditor, 1);
	editor->buffer = buffer;
	editor->on_ready = on_ready;
	editor->ready_data = data;
	editor->indent = g_strdup(DEFAULT_INDENT);
	editor->collapsed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	editor->shown = g_array_new(FALSE, FALSE, sizeof(TreeRow));
	editor->problem = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(editor->problem), TRUE);
	gtk_widget_add_css_class(editor->problem, "dim-label");
	editor->stack = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(editor->stack), create_loading_spinner(), "loading");
	gtk_stack_add_named(GTK_STACK(editor->stack), create_rows_view(editor), "rows");
	gtk_stack_add_named(GTK_STACK(editor->stack), create_empty_page(editor), "empty");
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
	editor->is_preview = FALSE;
	ParseJob *job = g_new0(ParseJob, 1);
	job->text = get_buffer_text(editor);
	start_reading(editor, job);
}


void preview_json_editor_file(GtkWidget *widget, GFile *file) {
	JsonEditor *editor = get_editor(widget);
	g_clear_handle_id(&editor->rebuild_source, g_source_remove);
	editor->is_preview = TRUE;
	ParseJob *job = g_new0(ParseJob, 1);
	job->file = g_object_ref(file);
	start_reading(editor, job);
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
	cancel_reading(editor);
	/* The list lets go of its rows before the values they read are freed, and must hear of it or GTK aborts when
	 * the list is destroyed with rows its model no longer has */
	guint count = editor->model->count;
	editor->model->count = 0;
	g_list_model_items_changed(G_LIST_MODEL(editor->model), 0, count, 0);
	g_array_unref(editor->shown);
	if (editor->shown_text != editor->text) {
		g_clear_pointer(&editor->shown_root, free_json);
		g_free(editor->shown_text);
	}
	g_clear_pointer(&editor->root, free_json);
	g_clear_pointer(&editor->drag_path, g_array_unref);
	g_hash_table_unref(editor->collapsed);
	g_free(editor->text);
	g_free(editor->indent);
	g_free(editor);
}
