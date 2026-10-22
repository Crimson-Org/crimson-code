#include <adwaita.h>

#include "../lang/en.h"
#include "comment-markers.h"
#include "markdown.h"
#include "plan-items.h"
#include "plan-scan.h"
#include "plan-sidebar.h"
#include "settings.h"

#define PLAN_KEY "plan"
#define ACTION_GROUP "plan"
#define NEW_PLAN_FILE "TODO"
#define ROW_SPACING 6
#define PLAN_FILE_ICON "checkbox-checked-symbolic"
#define SECONDARY_BUTTON 3

/* The parts of one row, found again when a reused row shows another item */
typedef struct {
	GtkWidget *box, *check, *icon, *tag, *label, *count, *menu;
	PlanItem *item;
} PlanRow;

/* The plan's widgets, its files and what is being scanned */
typedef struct {
	GtkWidget *root, *view, *pages;
	/* Plan files first and then source files, each sorted by name */
	GListStore *files;
	GtkTreeListModel *tree;
	GtkSingleSelection *selection;
	char *folder;
	/* Cancelled when the folder changes, so a slow scan never shows another project */
	GCancellable *cancellable;
	/* Set when all of the project needs scanning before the plan next shows */
	gboolean is_stale;
	/* Keys of rows to open once the first scan of the project shows them */
	GHashTable *rows_to_open;
	/* Plan files are watched for changes from other programs */
	GHashTable *monitors;
	/* Bound rows by item, so keyboard menus can point at the row */
	GHashTable *rows;
	/* The item a menu or the new task popover is for, and that popover while it shows */
	PlanItem *target;
	GtkWidget *popover;
	PlanEvents events;
} PlanSidebar;

/* A scan with the folder it was started for */
typedef struct {
	PlanSidebar *plan;
	char *only_path;
} ScanRequest;

static const SettingDefinition PLAN_SETTINGS[] = {
	{"plan-files", "plan", TEXT_GROUP_PLAN, TEXT_SETTING_PLAN_FILES, TEXT_SETTING_PLAN_FILES_DETAIL, SETTING_TEXT,
		"TODO TODO.md *.todo *.plan", 0, 0, NULL, NULL, {NULL}},
	{"comment-tags", "plan", TEXT_GROUP_PLAN, TEXT_SETTING_COMMENT_TAGS, TEXT_SETTING_COMMENT_TAGS_DETAIL,
		SETTING_TEXT, "TODO FIXME BUG HACK XXX", 0, 0, NULL, NULL, {NULL}},
};

static void scan_project(PlanSidebar *plan);
static void scan_file(PlanSidebar *plan, const char *path);


/* ==================================== Files in the list ==================================== */


static int compare_files(gconstpointer first, gconstpointer second, gpointer data) {
	const PlanItem *first_item = first, *second_item = second;
	if (first_item->kind != second_item->kind)
		return first_item->kind == PLAN_ITEM_PLAN_FILE ? -1 : 1;
	return g_utf8_collate(first_item->text, second_item->text);
}


static PlanItem *get_file(PlanSidebar *plan, guint position) {
	PlanItem *file = g_list_model_get_item(G_LIST_MODEL(plan->files), position);
	g_object_unref(file);
	return file;
}


/* The file item an item belongs to, as long as the item is from the file's latest reading */
static PlanItem *find_item_file(PlanSidebar *plan, PlanItem *item) {
	for (guint position = 0; position < g_list_model_get_n_items(G_LIST_MODEL(plan->files)); position++) {
		PlanItem *file = get_file(plan, position);
		if (g_file_equal(file->file, item->file))
			return contains_plan_item(file, item) ? file : NULL;
	}
	return NULL;
}


static void on_plan_file_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event,
	PlanSidebar *plan) {
	if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && event != G_FILE_MONITOR_EVENT_DELETED)
		return;
	g_autofree char *path = g_file_get_path(file);
	scan_file(plan, path);
}


static void watch_plan_file(PlanSidebar *plan, PlanItem *file) {
	if (file->kind != PLAN_ITEM_PLAN_FILE || g_hash_table_contains(plan->monitors, file->key))
		return;
	GFileMonitor *monitor = g_file_monitor_file(file->file, G_FILE_MONITOR_NONE, NULL, NULL);
	if (!monitor)
		return;
	g_signal_connect(monitor, "changed", G_CALLBACK(on_plan_file_changed), plan);
	g_hash_table_insert(plan->monitors, g_strdup(file->key), monitor);
}


static GHashTable *create_key_set(void) {
	return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}


/* Keys of the open rows, so a reading of a file can open the same rows again */
static GHashTable *list_expanded_keys(PlanSidebar *plan) {
	GHashTable *keys = create_key_set();
	for (guint index = 0; index < g_list_model_get_n_items(G_LIST_MODEL(plan->tree)); index++) {
		g_autoptr(GtkTreeListRow) row = gtk_tree_list_model_get_row(plan->tree, index);
		g_autoptr(PlanItem) item = gtk_tree_list_row_get_item(row);
		if (gtk_tree_list_row_get_expanded(row))
			g_hash_table_add(keys, g_strdup(item->key));
	}
	return keys;
}


static void expand_keys(PlanSidebar *plan, GHashTable *keys) {
	for (guint index = 0; index < g_list_model_get_n_items(G_LIST_MODEL(plan->tree)); index++) {
		g_autoptr(GtkTreeListRow) row = gtk_tree_list_model_get_row(plan->tree, index);
		g_autoptr(PlanItem) item = gtk_tree_list_row_get_item(row);
		if (g_hash_table_contains(keys, item->key))
			gtk_tree_list_row_set_expanded(row, TRUE);
	}
}


static void show_list_or_empty(PlanSidebar *plan) {
	gboolean is_empty = g_list_model_get_n_items(G_LIST_MODEL(plan->files)) == 0;
	gtk_stack_set_visible_child_name(GTK_STACK(plan->pages), is_empty ? "empty" : "list");
}


/* Brings the shown files in line with the wanted ones, both in list order, so files still wanted keep their rows */
/* New files go in together, since the list keeps its top row in view while rows are added above it one by one */
static void merge_files(PlanSidebar *plan, GPtrArray *wanted) {
	guint position = 0, index = 0;
	while (position < g_list_model_get_n_items(G_LIST_MODEL(plan->files)) || index < wanted->len) {
		PlanItem *shown = position < g_list_model_get_n_items(G_LIST_MODEL(plan->files)) ? get_file(plan, position)
			: NULL;
		PlanItem *fresh = index < wanted->len ? g_ptr_array_index(wanted, index) : NULL;
		int order = !shown ? 1 : !fresh ? -1 : compare_files(shown, fresh, NULL);
		if (order < 0) {
			g_hash_table_remove(plan->monitors, shown->key);
			g_list_store_remove(plan->files, position);
			continue;
		}
		if (order == 0) {
			if (shown != fresh)
				update_plan_file(shown, fresh);
			position++;
			index++;
			continue;
		}
		guint end = index;
		while (end < wanted->len && (!shown || compare_files(shown, g_ptr_array_index(wanted, end), NULL) > 0))
			end++;
		g_list_store_splice(plan->files, position, 0, &wanted->pdata[index], end - index);
		for (; index < end; index++, position++)
			watch_plan_file(plan, g_ptr_array_index(wanted, index));
	}
}


/* A scan of one file replaces only that file, while a scan of the project replaces every file */
static void show_scan(PlanSidebar *plan, GPtrArray *found, const char *only_path) {
	g_autoptr(GHashTable) expanded = list_expanded_keys(plan);
	g_autoptr(GPtrArray) wanted = g_ptr_array_new();
	for (guint position = 0; only_path && position < g_list_model_get_n_items(G_LIST_MODEL(plan->files)); position++) {
		PlanItem *file = get_file(plan, position);
		if (g_strcmp0(file->key, only_path) != 0)
			g_ptr_array_add(wanted, file);
	}
	g_ptr_array_extend(wanted, found, NULL, NULL);
	g_ptr_array_sort_values_with_data(wanted, compare_files, NULL);
	merge_files(plan, wanted);
	expand_keys(plan, expanded);
	if (!only_path) {
		expand_keys(plan, plan->rows_to_open);
		g_hash_table_remove_all(plan->rows_to_open);
	}
	show_list_or_empty(plan);
}


/* ==================================== Scanning ==================================== */


static void on_scanned(GObject *source, GAsyncResult *result, gpointer data) {
	ScanRequest *request = data;
	g_autoptr(GPtrArray) found = finish_plan_scan(result, NULL);
	if (found)
		show_scan(request->plan, found, request->only_path);
	g_free(request->only_path);
	g_free(request);
}


static void start_scan(PlanSidebar *plan, const char *only_path) {
	ScanRequest *request = g_new0(ScanRequest, 1);
	request->plan = plan;
	request->only_path = g_strdup(only_path);
	start_plan_scan(plan->folder, only_path, plan->cancellable, on_scanned, request);
}


/* A new scan of all the project makes any running scan useless, so those are cancelled first */
static void restart_cancellable(PlanSidebar *plan) {
	g_cancellable_cancel(plan->cancellable);
	g_object_unref(plan->cancellable);
	plan->cancellable = g_cancellable_new();
}


static void scan_project(PlanSidebar *plan) {
	plan->is_stale = TRUE;
	if (!plan->folder || !gtk_widget_get_mapped(plan->root))
		return;
	plan->is_stale = FALSE;
	restart_cancellable(plan);
	start_scan(plan, NULL);
}


static void scan_file(PlanSidebar *plan, const char *path) {
	if (plan->folder && !plan->is_stale)
		start_scan(plan, path);
}


static void on_mapped(GtkWidget *widget, PlanSidebar *plan) {
	if (plan->is_stale)
		scan_project(plan);
}


static void on_setting_changed(const char *key, gpointer data) {
	static const char *const KEYS[] = {"plan-files", "comment-tags", "excluded-names", "show-hidden-files"};
	for (guint index = 0; index < G_N_ELEMENTS(KEYS); index++)
		if (g_strcmp0(key, KEYS[index]) == 0)
			scan_project(data);
}


static void on_comment_markers_changed(gpointer data) {
	scan_project(data);
}


/* ==================================== Changing plan files ==================================== */


static void show_text(PlanSidebar *plan, PlanItem *file, const char *text) {
	g_autoptr(GPtrArray) found = g_ptr_array_new_with_free_func(g_object_unref);
	g_ptr_array_add(found, parse_plan_file(file->file, file->text, text));
	g_autofree char *path = g_strdup(file->key);
	show_scan(plan, found, path);
}


static char *read_current_text(PlanSidebar *plan, GFile *file) {
	char *text = plan->events.get_open_text(file, plan->events.data);
	if (text)
		return text;
	g_autofree char *contents = NULL;
	gsize length = 0;
	if (!g_file_load_contents(file, NULL, &contents, &length, NULL, NULL))
		return NULL;
	return g_utf8_make_valid(contents, length);
}


static gboolean write_text(PlanSidebar *plan, GFile *file, const char *text) {
	if (plan->events.change_open_text(file, text, plan->events.data))
		return TRUE;
	g_autoptr(GError) error = NULL;
	if (g_file_replace_contents(file, text, strlen(text), NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, &error))
		return TRUE;
	plan->events.failed(error->message, plan->events.data);
	return FALSE;
}


typedef char *(*PlanChangeFunction)(const char *text, PlanItem *file, PlanItem *item, const char *task);


/* Changes are made to the file's current text, and a file changed elsewhere is shown again instead */
static void change_plan(PlanSidebar *plan, PlanItem *item, PlanChangeFunction change, const char *task) {
	PlanItem *file = find_item_file(plan, item);
	if (!file)
		return;
	g_autoptr(PlanItem) keep = g_object_ref(file);
	g_autofree char *current = read_current_text(plan, file->file);
	g_autofree char *changed = current ? change(current, file, item, task) : NULL;
	if (!changed) {
		plan->events.failed(TEXT_PLAN_CHANGED, plan->events.data);
		if (current)
			show_text(plan, file, current);
		return;
	}
	if (write_text(plan, file->file, changed))
		show_text(plan, file, changed);
}


static char *toggle_task(const char *text, PlanItem *file, PlanItem *item, const char *task) {
	return toggle_plan_task(text, file, item);
}


static char *remove_item(const char *text, PlanItem *file, PlanItem *item, const char *task) {
	return remove_plan_item(text, file, item);
}


static char *archive_item(const char *text, PlanItem *file, PlanItem *item, const char *task) {
	return archive_plan_item(text, file, item);
}


static char *add_task(const char *text, PlanItem *file, PlanItem *item, const char *task) {
	return add_plan_task(text, file, item, task);
}


/* A project without a plan file gets a TODO file in its folder for the first task */
static void create_plan_file(PlanSidebar *plan, const char *task) {
	g_autofree char *path = g_build_filename(plan->folder, NEW_PLAN_FILE, NULL);
	g_autoptr(GFile) file = g_file_new_for_path(path);
	g_autofree char *text = g_strdup_printf("[ ] %s\n", task);
	g_autoptr(GError) error = NULL;
	if (!g_file_replace_contents(file, text, strlen(text), NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, &error)) {
		plan->events.failed(error->message, plan->events.data);
		return;
	}
	g_autoptr(PlanItem) fresh = parse_plan_file(file, NEW_PLAN_FILE, text);
	show_text(plan, fresh, text);
}


/* ==================================== Rows ==================================== */


/* Showing an item sets the box to the item's own state, which needs no change to the file */
static void on_check_toggled(GtkCheckButton *check, PlanRow *row) {
	PlanSidebar *plan = g_object_get_data(G_OBJECT(row->box), PLAN_KEY);
	if (row->item && gtk_check_button_get_active(check) != row->item->is_checked)
		change_plan(plan, row->item, toggle_task, NULL);
}


static void open_menu(PlanSidebar *plan, PlanRow *row, double x, double y);


static void on_row_pressed(GtkGestureClick *gesture, int presses, double x, double y, PlanRow *row) {
	PlanSidebar *plan = g_object_get_data(G_OBJECT(row->box), PLAN_KEY);
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	open_menu(plan, row, x, y);
}


static void on_menu_clicked(GtkButton *button, PlanRow *row) {
	PlanSidebar *plan = g_object_get_data(G_OBJECT(row->box), PLAN_KEY);
	open_menu(plan, row, -1, -1);
}


/* A cut off line shows in full while the pointer rests on it */
static gboolean show_full_text(GtkWidget *label, int x, int y, gboolean is_keyboard, GtkTooltip *tooltip,
	gpointer data) {
	if (!pango_layout_is_ellipsized(gtk_label_get_layout(GTK_LABEL(label))))
		return FALSE;
	if (gtk_label_get_use_markup(GTK_LABEL(label)))
		gtk_tooltip_set_markup(tooltip, gtk_label_get_label(GTK_LABEL(label)));
	else
		gtk_tooltip_set_text(tooltip, gtk_label_get_label(GTK_LABEL(label)));
	return TRUE;
}


static GtkWidget *create_row_label(const char *style) {
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	if (style)
		gtk_widget_add_css_class(label, style);
	return label;
}


static void set_up_row(GtkSignalListItemFactory *factory, GtkListItem *list_item, PlanSidebar *plan) {
	PlanRow *row = g_new0(PlanRow, 1);
	row->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, ROW_SPACING);
	g_object_set_data_full(G_OBJECT(row->box), "row", row, g_free);
	g_object_set_data(G_OBJECT(row->box), PLAN_KEY, plan);
	row->check = gtk_check_button_new();
	g_signal_connect(row->check, "toggled", G_CALLBACK(on_check_toggled), row);
	row->icon = gtk_image_new();
	row->tag = create_row_label("plan-tag");
	gtk_widget_add_css_class(row->tag, "accent");
	row->label = create_row_label(NULL);
	gtk_label_set_ellipsize(GTK_LABEL(row->label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_has_tooltip(row->label, TRUE);
	g_signal_connect(row->label, "query-tooltip", G_CALLBACK(show_full_text), NULL);
	gtk_widget_set_hexpand(row->label, TRUE);
	row->count = create_row_label("dim-label");
	gtk_widget_add_css_class(row->count, "numeric");
	row->menu = gtk_button_new_from_icon_name("view-more-symbolic");
	gtk_widget_add_css_class(row->menu, "flat");
	gtk_widget_add_css_class(row->menu, "plan-row-menu");
	gtk_widget_set_tooltip_text(row->menu, TEXT_PLAN_MORE);
	g_signal_connect(row->menu, "clicked", G_CALLBACK(on_menu_clicked), row);
	GtkWidget *parts[] = {row->check, row->icon, row->tag, row->label, row->count, row->menu};
	for (guint index = 0; index < G_N_ELEMENTS(parts); index++)
		gtk_box_append(GTK_BOX(row->box), parts[index]);
	GtkGesture *secondary = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), SECONDARY_BUTTON);
	g_signal_connect(secondary, "pressed", G_CALLBACK(on_row_pressed), row);
	gtk_widget_add_controller(row->box, GTK_EVENT_CONTROLLER(secondary));
	GtkWidget *expander = gtk_tree_expander_new();
	gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), row->box);
	gtk_list_item_set_child(list_item, expander);
}


static void set_markup_or_text(GtkWidget *label, const char *markup, const char *text) {
	gboolean is_markup = pango_parse_markup(markup, -1, 0, NULL, NULL, NULL, NULL);
	gtk_label_set_use_markup(GTK_LABEL(label), is_markup);
	gtk_label_set_label(GTK_LABEL(label), is_markup ? markup : text);
}


/* Files show their name with the folder dimmed after it, and lines show their Markdown */
static char *create_row_markup(PlanItem *item) {
	if (item->kind == PLAN_ITEM_PLAN_FILE || item->kind == PLAN_ITEM_SOURCE_FILE) {
		g_autofree char *name = g_path_get_basename(item->text), *folder = g_path_get_dirname(item->text);
		if (g_strcmp0(folder, ".") == 0)
			return g_markup_printf_escaped("%s", name);
		return g_markup_printf_escaped("%s  <span alpha=\"55%%\">%s</span>", name, folder);
	}
	if (item->kind == PLAN_ITEM_COMMENT)
		return g_markup_escape_text(item->text, -1);
	g_autofree char *markup = convert_markdown_line(item->text);
	if (item->kind == PLAN_ITEM_HEADING)
		return g_strconcat("<b>", markup, "</b>", NULL);
	return item->is_checked ? g_strconcat("<s>", markup, "</s>", NULL) : g_strdup(markup);
}


static void show_file_icon(PlanRow *row, PlanItem *item) {
	if (item->kind == PLAN_ITEM_PLAN_FILE) {
		gtk_image_set_from_icon_name(GTK_IMAGE(row->icon), PLAN_FILE_ICON);
		return;
	}
	g_autofree char *type = g_content_type_guess(item->text, NULL, 0, NULL);
	g_autoptr(GIcon) icon = g_content_type_get_icon(type);
	gtk_image_set_from_gicon(GTK_IMAGE(row->icon), icon);
}


static void fill_row(PlanRow *row) {
	PlanItem *item = row->item;
	gboolean is_file = item->kind == PLAN_ITEM_PLAN_FILE || item->kind == PLAN_ITEM_SOURCE_FILE;
	gtk_check_button_set_active(GTK_CHECK_BUTTON(row->check), item->is_checked);
	gtk_widget_set_visible(row->check, item->kind == PLAN_ITEM_TASK);
	gtk_widget_set_visible(row->icon, is_file);
	if (is_file)
		show_file_icon(row, item);
	gtk_widget_set_visible(row->tag, item->kind == PLAN_ITEM_COMMENT);
	gtk_label_set_text(GTK_LABEL(row->tag), item->tag ? item->tag : "");
	g_autofree char *markup = create_row_markup(item);
	set_markup_or_text(row->label, markup, item->text);
	if (item->is_checked || item->is_archived)
		gtk_widget_add_css_class(row->label, "dim-label");
	else
		gtk_widget_remove_css_class(row->label, "dim-label");
	g_autofree char *count = item->kind == PLAN_ITEM_COMMENT ? g_strdup_printf("%d", item->line + 1)
		: is_file && item->open_count ? g_strdup_printf("%u", item->open_count) : g_strdup("");
	gtk_label_set_text(GTK_LABEL(row->count), count);
	gtk_widget_set_visible(row->count, *count);
	gtk_widget_set_visible(row->menu, item->kind != PLAN_ITEM_COMMENT && item->kind != PLAN_ITEM_SOURCE_FILE);
	if (item->is_new)
		gtk_widget_add_css_class(row->box, "plan-new");
	item->is_new = FALSE;
}


static void on_item_changed(PlanItem *item, PlanRow *row) {
	fill_row(row);
}


static PlanRow *get_row(GtkListItem *list_item) {
	GtkWidget *expander = gtk_list_item_get_child(list_item);
	return g_object_get_data(G_OBJECT(gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander))), "row");
}


static void bind_row(GtkSignalListItemFactory *factory, GtkListItem *list_item, PlanSidebar *plan) {
	GtkTreeListRow *tree_row = gtk_list_item_get_item(list_item);
	gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(gtk_list_item_get_child(list_item)), tree_row);
	PlanRow *row = get_row(list_item);
	g_autoptr(PlanItem) item = gtk_tree_list_row_get_item(tree_row);
	row->item = item;
	fill_row(row);
	g_signal_connect(item, "changed", G_CALLBACK(on_item_changed), row);
	g_hash_table_insert(plan->rows, item, row);
}


/* Rows are reused for other items while scrolling, so the old item must stop updating this row */
static void unbind_row(GtkSignalListItemFactory *factory, GtkListItem *list_item, PlanSidebar *plan) {
	PlanRow *row = get_row(list_item);
	if (!row->item)
		return;
	g_signal_handlers_disconnect_by_data(row->item, row);
	gtk_widget_remove_css_class(row->box, "plan-new");
	if (g_hash_table_lookup(plan->rows, row->item) == row)
		g_hash_table_remove(plan->rows, row->item);
	row->item = NULL;
}


static GListModel *create_child_model(gpointer item, gpointer data) {
	PlanItem *plan_item = item;
	return plan_item->children ? g_object_ref(G_LIST_MODEL(plan_item->children)) : NULL;
}


/* Rows with children open and close, and the others show their line in the file */
static void on_row_activated(GtkListView *view, guint position, PlanSidebar *plan) {
	g_autoptr(GtkTreeListRow) row = gtk_tree_list_model_get_row(plan->tree, position);
	g_autoptr(PlanItem) item = gtk_tree_list_row_get_item(row);
	if (item->children && g_list_model_get_n_items(G_LIST_MODEL(item->children)) > 0)
		gtk_tree_list_row_set_expanded(row, !gtk_tree_list_row_get_expanded(row));
	else
		plan->events.spot_chosen(item->file, item->line, plan->events.data);
}


/* ==================================== Menus and new tasks ==================================== */


static void set_target(PlanSidebar *plan, PlanItem *item) {
	g_set_object(&plan->target, item);
}


static void close_popover(GtkPopover *popover, PlanSidebar *plan) {
	if (plan->popover == GTK_WIDGET(popover))
		plan->popover = NULL;
	g_idle_add_once((GSourceOnceFunc)gtk_widget_unparent, popover);
}


static void show_popover(PlanSidebar *plan, GtkWidget *popover, GtkWidget *parent) {
	gtk_widget_set_parent(popover, parent);
	g_signal_connect(popover, "closed", G_CALLBACK(close_popover), plan);
	plan->popover = popover;
	gtk_popover_popup(GTK_POPOVER(popover));
}


static GMenuModel *create_item_menu(PlanItem *item) {
	GMenu *menu = g_menu_new();
	if (item->kind == PLAN_ITEM_PLAN_FILE)
		g_menu_append(menu, TEXT_PLAN_NEW_TASK, ACTION_GROUP ".add");
	else if (!item->is_archived)
		g_menu_append(menu, TEXT_PLAN_NEW_SUBTASK, ACTION_GROUP ".add");
	g_menu_append(menu, TEXT_PLAN_SHOW_IN_FILE, ACTION_GROUP ".show");
	if (item->kind == PLAN_ITEM_PLAN_FILE)
		return G_MENU_MODEL(menu);
	GMenu *changes = g_menu_new();
	if (!item->is_archived)
		g_menu_append(changes, TEXT_PLAN_ARCHIVE, ACTION_GROUP ".archive");
	g_menu_append(changes, TEXT_PLAN_DELETE, ACTION_GROUP ".delete");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(changes));
	g_object_unref(changes);
	return G_MENU_MODEL(menu);
}


static void open_menu(PlanSidebar *plan, PlanRow *row, double x, double y) {
	if (!row->item || row->item->kind == PLAN_ITEM_COMMENT || row->item->kind == PLAN_ITEM_SOURCE_FILE)
		return;
	set_target(plan, row->item);
	g_autoptr(GMenuModel) model = create_item_menu(row->item);
	GtkWidget *popover = gtk_popover_menu_new_from_model(model);
	if (x >= 0)
		gtk_popover_set_pointing_to(GTK_POPOVER(popover), &(GdkRectangle){(int)x, (int)y, 1, 1});
	show_popover(plan, popover, x < 0 ? row->menu : row->box);
}


static void on_task_entered(GtkEntry *entry, PlanSidebar *plan) {
	g_autofree char *task = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));
	gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(entry), GTK_TYPE_POPOVER)));
	if (!*task)
		return;
	if (plan->target)
		change_plan(plan, plan->target, add_task, task);
	else
		create_plan_file(plan, task);
}


/* A small field under the row or button asks for the task, and Enter adds it */
static void ask_for_task(PlanSidebar *plan, GtkWidget *parent) {
	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), TEXT_PLAN_TASK_PLACEHOLDER);
	gtk_widget_set_size_request(entry, 240, -1);
	g_signal_connect(entry, "activate", G_CALLBACK(on_task_entered), plan);
	GtkWidget *popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(popover), entry);
	show_popover(plan, popover, parent);
	gtk_widget_grab_focus(entry);
}


static GtkWidget *find_target_widget(PlanSidebar *plan) {
	PlanRow *row = plan->target ? g_hash_table_lookup(plan->rows, plan->target) : NULL;
	return row ? row->box : plan->view;
}


static void add_to_target(GSimpleAction *action, GVariant *parameter, gpointer data) {
	PlanSidebar *plan = data;
	ask_for_task(plan, find_target_widget(plan));
}


static void show_target(GSimpleAction *action, GVariant *parameter, gpointer data) {
	PlanSidebar *plan = data;
	if (plan->target)
		plan->events.spot_chosen(plan->target->file, plan->target->line, plan->events.data);
}


static void archive_target(GSimpleAction *action, GVariant *parameter, gpointer data) {
	PlanSidebar *plan = data;
	if (plan->target)
		change_plan(plan, plan->target, archive_item, NULL);
}


static void delete_target(GSimpleAction *action, GVariant *parameter, gpointer data) {
	PlanSidebar *plan = data;
	if (plan->target)
		change_plan(plan, plan->target, remove_item, NULL);
}


static PlanItem *get_selected_item(PlanSidebar *plan) {
	GtkTreeListRow *row = gtk_single_selection_get_selected_item(plan->selection);
	return row ? gtk_tree_list_row_get_item(row) : NULL;
}


/* The header's button adds to the selected plan file, or the first one, or a new TODO file */
static void add_from_header(GtkButton *button, PlanSidebar *plan) {
	g_autoptr(PlanItem) selected = get_selected_item(plan);
	PlanItem *file = selected ? find_item_file(plan, selected) : NULL;
	if (!file || file->kind != PLAN_ITEM_PLAN_FILE) {
		g_autoptr(PlanItem) first = g_list_model_get_n_items(G_LIST_MODEL(plan->files)) > 0
			? g_list_model_get_item(G_LIST_MODEL(plan->files), 0) : NULL;
		file = first && first->kind == PLAN_ITEM_PLAN_FILE ? first : NULL;
	}
	set_target(plan, file);
	ask_for_task(plan, GTK_WIDGET(button));
}


static void refresh_from_header(GtkButton *button, PlanSidebar *plan) {
	scan_project(plan);
}


/* Space checks a task, and the menu key or Shift+F10 opens the row's menu, as a right click does */
static gboolean on_key_pressed(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType state,
	PlanSidebar *plan) {
	g_autoptr(PlanItem) item = get_selected_item(plan);
	PlanRow *row = item ? g_hash_table_lookup(plan->rows, item) : NULL;
	if (!row || plan->popover)
		return GDK_EVENT_PROPAGATE;
	if (key == GDK_KEY_space && item->kind == PLAN_ITEM_TASK) {
		change_plan(plan, item, toggle_task, NULL);
		return GDK_EVENT_STOP;
	}
	if (key == GDK_KEY_Menu || (key == GDK_KEY_F10 && state & GDK_SHIFT_MASK)) {
		open_menu(plan, row, -1, -1);
		return GDK_EVENT_STOP;
	}
	return GDK_EVENT_PROPAGATE;
}


/* ==================================== Creating ==================================== */


static GtkWidget *create_header_button(const char *icon, const char *tooltip, GCallback on_clicked,
	PlanSidebar *plan) {
	GtkWidget *button = gtk_button_new_from_icon_name(icon);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_set_tooltip_text(button, tooltip);
	g_signal_connect(button, "clicked", on_clicked, plan);
	return button;
}


static GtkWidget *create_header(PlanSidebar *plan) {
	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_add_css_class(header, "panel-header");
	GtkWidget *title = gtk_label_new(TEXT_PLAN);
	gtk_widget_add_css_class(title, "heading");
	gtk_label_set_xalign(GTK_LABEL(title), 0);
	gtk_widget_set_hexpand(title, TRUE);
	gtk_widget_set_margin_start(title, 6);
	gtk_box_append(GTK_BOX(header), title);
	gtk_box_append(GTK_BOX(header), create_header_button("list-add-symbolic", TEXT_PLAN_NEW_TASK,
		G_CALLBACK(add_from_header), plan));
	gtk_box_append(GTK_BOX(header), create_header_button("view-refresh-symbolic", TEXT_PLAN_REFRESH,
		G_CALLBACK(refresh_from_header), plan));
	return header;
}


static GtkWidget *create_list(PlanSidebar *plan) {
	plan->files = g_list_store_new(PLAN_TYPE_ITEM);
	plan->tree = gtk_tree_list_model_new(G_LIST_MODEL(g_object_ref(plan->files)), FALSE, FALSE, create_child_model,
		NULL, NULL);
	plan->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(plan->tree)));
	gtk_single_selection_set_autoselect(plan->selection, FALSE);
	gtk_single_selection_set_can_unselect(plan->selection, TRUE);
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(set_up_row), plan);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_row), plan);
	g_signal_connect(factory, "unbind", G_CALLBACK(unbind_row), plan);
	plan->view = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(plan->selection)), factory);
	gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(plan->view), TRUE);
	gtk_widget_add_css_class(plan->view, "navigation-sidebar");
	gtk_widget_add_css_class(plan->view, "plan-list");
	g_signal_connect(plan->view, "activate", G_CALLBACK(on_row_activated), plan);
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), plan);
	gtk_widget_add_controller(plan->view, keys);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), plan->view);
	gtk_widget_set_vexpand(scroller, TRUE);
	return scroller;
}


static GtkWidget *create_empty_page(void) {
	GtkWidget *label = gtk_label_new(TEXT_PLAN_EMPTY);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
	gtk_widget_add_css_class(label, "dim-label");
	gtk_widget_set_margin_start(label, 12);
	gtk_widget_set_margin_end(label, 12);
	return label;
}


static void install_actions(PlanSidebar *plan) {
	const GActionEntry ENTRIES[] = {
		{"add", add_to_target, NULL, NULL, NULL, {0}},
		{"show", show_target, NULL, NULL, NULL, {0}},
		{"archive", archive_target, NULL, NULL, NULL, {0}},
		{"delete", delete_target, NULL, NULL, NULL, {0}},
	};
	GSimpleActionGroup *actions = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(actions), ENTRIES, G_N_ELEMENTS(ENTRIES), plan);
	gtk_widget_insert_action_group(plan->root, ACTION_GROUP, G_ACTION_GROUP(actions));
	g_object_unref(actions);
}


static void free_plan(gpointer data) {
	PlanSidebar *plan = data;
	unwatch_settings(plan);
	unwatch_comment_markers(plan);
	g_cancellable_cancel(plan->cancellable);
	g_object_unref(plan->cancellable);
	g_hash_table_unref(plan->monitors);
	g_hash_table_unref(plan->rows);
	g_hash_table_unref(plan->rows_to_open);
	g_clear_object(&plan->target);
	g_object_unref(plan->selection);
	g_object_unref(plan->tree);
	g_object_unref(plan->files);
	g_free(plan->folder);
	g_free(plan);
}


void register_plan_settings(void) {
	register_settings(PLAN_SETTINGS, G_N_ELEMENTS(PLAN_SETTINGS));
	register_comment_markers();
}


GtkWidget *create_plan_sidebar(const PlanEvents *events) {
	PlanSidebar *plan = g_new0(PlanSidebar, 1);
	plan->events = *events;
	plan->cancellable = g_cancellable_new();
	plan->monitors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	plan->rows = g_hash_table_new(NULL, NULL);
	plan->rows_to_open = create_key_set();
	plan->pages = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(plan->pages), create_list(plan), "list");
	gtk_stack_add_named(GTK_STACK(plan->pages), create_empty_page(), "empty");
	gtk_widget_set_vexpand(plan->pages, TRUE);
	plan->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(plan->root, "project-tree");
	gtk_box_append(GTK_BOX(plan->root), create_header(plan));
	gtk_box_append(GTK_BOX(plan->root), plan->pages);
	install_actions(plan);
	g_signal_connect(plan->root, "map", G_CALLBACK(on_mapped), plan);
	watch_settings(on_setting_changed, plan);
	watch_comment_markers(on_comment_markers_changed, plan);
	g_object_set_data_full(G_OBJECT(plan->root), PLAN_KEY, plan, free_plan);
	show_list_or_empty(plan);
	return plan->root;
}


void set_plan_sidebar_folder(GtkWidget *widget, const char *folder, const char *const *open_rows) {
	PlanSidebar *plan = g_object_get_data(G_OBJECT(widget), PLAN_KEY);
	g_set_str(&plan->folder, folder);
	g_hash_table_remove_all(plan->rows_to_open);
	for (guint index = 0; open_rows && open_rows[index]; index++)
		g_hash_table_add(plan->rows_to_open, g_strdup(open_rows[index]));
	restart_cancellable(plan);
	g_hash_table_remove_all(plan->monitors);
	g_list_store_remove_all(plan->files);
	show_list_or_empty(plan);
	scan_project(plan);
}


/* Rows waiting for the plan's first scan stay listed, so a plan never shown keeps what was open */
GStrv list_plan_open_rows(GtkWidget *widget) {
	PlanSidebar *plan = g_object_get_data(G_OBJECT(widget), PLAN_KEY);
	g_autoptr(GHashTable) keys = list_expanded_keys(plan);
	GHashTableIter waiting;
	gpointer key = NULL;
	g_hash_table_iter_init(&waiting, plan->rows_to_open);
	while (g_hash_table_iter_next(&waiting, &key, NULL))
		g_hash_table_add(keys, g_strdup(key));
	g_autoptr(GStrvBuilder) rows = g_strv_builder_new();
	g_hash_table_iter_init(&waiting, keys);
	while (g_hash_table_iter_next(&waiting, &key, NULL))
		g_strv_builder_add(rows, key);
	return g_strv_builder_end(rows);
}


void refresh_plan_sidebar_file(GtkWidget *widget, GFile *file) {
	PlanSidebar *plan = g_object_get_data(G_OBJECT(widget), PLAN_KEY);
	g_autofree char *path = file ? g_file_get_path(file) : NULL;
	if (path)
		scan_file(plan, path);
}
