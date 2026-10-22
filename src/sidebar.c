#include "../lang/en.h"
#include "settings.h"
#include "sidebar.h"

/* Asking for the fast content type with the icon makes GIO pick icons from names without reading the files */
#define FILE_ATTRIBUTES "standard::name,standard::display-name,standard::type,standard::icon," \
	"standard::fast-content-type"
#define OPEN_FOLDER_ICON "folder-open"
#define FILE_ATTRIBUTE "standard::file"
#define ICON_SPACING 8

/* The tree view, the one filter each folder shares and where chosen files go */
typedef struct {
	GtkWidget *view;
	GtkFilter *filter;
	GFile *root;
	GtkTreeListModel *tree;
	/* Folders inside the project still to open once their parent is read, and the idle call that opens them */
	GHashTable *folders_to_open;
	guint open_source;
	/* Excluded name patterns, split once per change instead of once per file */
	GStrv excluded;
	FileChosenFunction on_chosen;
	gpointer data;
} Sidebar;

static const SettingDefinition SIDEBAR_SETTINGS[] = {
	{"show-hidden-files", "files", TEXT_GROUP_PROJECT_FILES, TEXT_SETTING_HIDDEN_FILES,
		TEXT_SETTING_HIDDEN_FILES_DETAIL, SETTING_SWITCH, "true", 0, 0, NULL, NULL, {NULL}},
	{"excluded-names", "files", TEXT_GROUP_PROJECT_FILES, TEXT_SETTING_EXCLUDED, TEXT_SETTING_EXCLUDED_DETAIL,
		SETTING_TEXT, ".git .hg .svn .goutputstream-*", 0, 0, NULL, NULL, {NULL}},
};


gboolean is_project_name_shown(const char *name, GStrv excluded, gboolean shows_hidden) {
	if (*name == '.' && !shows_hidden)
		return FALSE;
	for (int index = 0; excluded[index]; index++)
		if (*excluded[index] && g_pattern_match_simple(excluded[index], name))
			return FALSE;
	return TRUE;
}


GStrv list_excluded_project_names(void) {
	return g_strsplit_set(get_text_setting("excluded-names"), " ;,", -1);
}


static gboolean is_shown(gpointer item, gpointer data) {
	Sidebar *sidebar = data;
	return is_project_name_shown(g_file_info_get_name(G_FILE_INFO(item)), sidebar->excluded,
		get_switch_setting("show-hidden-files"));
}


static void read_excluded_names(Sidebar *sidebar) {
	g_strfreev(sidebar->excluded);
	sidebar->excluded = list_excluded_project_names();
}


static gboolean is_folder(GFileInfo *info) {
	return g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
}


/* Folders come first, then names in alphabetical order regardless of case */
static int compare_entries(gconstpointer first, gconstpointer second, gpointer data) {
	GFileInfo *first_info = G_FILE_INFO((gpointer)first), *second_info = G_FILE_INFO((gpointer)second);
	if (is_folder(first_info) != is_folder(second_info))
		return is_folder(first_info) ? -1 : 1;
	return g_ascii_strcasecmp(g_file_info_get_display_name(first_info), g_file_info_get_display_name(second_info));
}


static GListModel *create_folder_model(GFile *folder, Sidebar *sidebar) {
	GtkDirectoryList *list = gtk_directory_list_new(FILE_ATTRIBUTES, folder);
	GtkFilterListModel *shown = gtk_filter_list_model_new(G_LIST_MODEL(list), g_object_ref(sidebar->filter));
	GtkSortListModel *sorted = gtk_sort_list_model_new(G_LIST_MODEL(shown),
		GTK_SORTER(gtk_custom_sorter_new(compare_entries, NULL, NULL)));
	return G_LIST_MODEL(sorted);
}


static GFile *get_info_file(GFileInfo *info) {
	return G_FILE(g_file_info_get_attribute_object(info, FILE_ATTRIBUTE));
}


static GListModel *create_child_model(gpointer item, gpointer data) {
	GFileInfo *info = G_FILE_INFO(item);
	return is_folder(info) ? create_folder_model(get_info_file(info), data) : NULL;
}


static void set_up_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, ICON_SPACING);
	gtk_box_append(GTK_BOX(box), gtk_image_new());
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_box_append(GTK_BOX(box), label);
	GtkWidget *expander = gtk_tree_expander_new();
	gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
	gtk_list_item_set_child(item, expander);
}


/* Shows the file type icon from the icon theme, and an open folder while a folder is expanded */
static void show_row_icon(GtkTreeListRow *row, GParamSpec *property, GtkImage *icon) {
	g_autoptr(GFileInfo) info = gtk_tree_list_row_get_item(row);
	GIcon *file_icon = g_file_info_get_icon(info);
	if (is_folder(info) && gtk_tree_list_row_get_expanded(row)) {
		g_autoptr(GIcon) open_icon = g_themed_icon_new(OPEN_FOLDER_ICON);
		gtk_image_set_from_gicon(icon, open_icon);
	} else if (file_icon) {
		gtk_image_set_from_gicon(icon, file_icon);
	} else {
		gtk_image_set_from_icon_name(icon, is_folder(info) ? "folder" : "text-x-generic");
	}
}


static void bind_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	GtkTreeListRow *row = gtk_list_item_get_item(item);
	GtkWidget *expander = gtk_list_item_get_child(item);
	gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);
	g_autoptr(GFileInfo) info = gtk_tree_list_row_get_item(row);
	GtkWidget *box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander));
	GtkWidget *icon = gtk_widget_get_first_child(box);
	show_row_icon(row, NULL, GTK_IMAGE(icon));
	g_signal_connect(row, "notify::expanded", G_CALLBACK(show_row_icon), icon);
	gtk_label_set_text(GTK_LABEL(gtk_widget_get_next_sibling(icon)), g_file_info_get_display_name(info));
}


/* Rows are reused for other files while scrolling, so the old row must stop updating this icon */
static void unbind_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	GtkWidget *expander = gtk_list_item_get_child(item);
	GtkWidget *icon = gtk_widget_get_first_child(gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander)));
	g_signal_handlers_disconnect_by_data(gtk_list_item_get_item(item), icon);
}


static void on_row_activated(GtkListView *view, guint position, Sidebar *sidebar) {
	GListModel *model = G_LIST_MODEL(gtk_list_view_get_model(view));
	g_autoptr(GtkTreeListRow) row = g_list_model_get_item(model, position);
	g_autoptr(GFileInfo) info = gtk_tree_list_row_get_item(row);
	if (is_folder(info))
		gtk_tree_list_row_set_expanded(row, !gtk_tree_list_row_get_expanded(row));
	else
		sidebar->on_chosen(get_info_file(info), sidebar->data);
}


static char *get_relative_path(Sidebar *sidebar, GFileInfo *info) {
	return g_file_get_relative_path(sidebar->root, get_info_file(info));
}


static gboolean open_waiting_folders(gpointer data) {
	Sidebar *sidebar = data;
	sidebar->open_source = 0;
	for (guint position = 0; position < g_list_model_get_n_items(G_LIST_MODEL(sidebar->tree)); position++) {
		g_autoptr(GtkTreeListRow) row = gtk_tree_list_model_get_row(sidebar->tree, position);
		g_autoptr(GFileInfo) info = gtk_tree_list_row_get_item(row);
		g_autofree char *path = is_folder(info) ? get_relative_path(sidebar, info) : NULL;
		if (path && g_hash_table_remove(sidebar->folders_to_open, path))
			gtk_tree_list_row_set_expanded(row, TRUE);
	}
	return G_SOURCE_REMOVE;
}


/* Folders are read in the background, so each batch of new rows may hold folders to open */
/* Opening one changes the tree, which must wait until every view has heard of the current change */
static void on_tree_changed(GListModel *tree, guint position, guint removed, guint added, Sidebar *sidebar) {
	if (added > 0 && g_hash_table_size(sidebar->folders_to_open) > 0 && !sidebar->open_source)
		sidebar->open_source = g_idle_add(open_waiting_folders, sidebar);
}


static void forget_tree(Sidebar *sidebar) {
	g_clear_handle_id(&sidebar->open_source, g_source_remove);
	if (sidebar->tree)
		g_signal_handlers_disconnect_by_data(sidebar->tree, sidebar);
	g_clear_object(&sidebar->tree);
}


/* Each open folder uses the same filter, so one change refilters the whole tree */
static void on_setting_changed(const char *key, gpointer data) {
	Sidebar *sidebar = data;
	if (g_strcmp0(key, "show-hidden-files") != 0 && g_strcmp0(key, "excluded-names") != 0)
		return;
	read_excluded_names(sidebar);
	gtk_filter_changed(sidebar->filter, GTK_FILTER_CHANGE_DIFFERENT);
}


static void free_sidebar(gpointer data) {
	Sidebar *sidebar = data;
	unwatch_settings(sidebar);
	forget_tree(sidebar);
	g_clear_object(&sidebar->root);
	g_hash_table_unref(sidebar->folders_to_open);
	g_object_unref(sidebar->filter);
	g_strfreev(sidebar->excluded);
	g_free(sidebar);
}


void register_sidebar_settings(void) {
	register_settings(SIDEBAR_SETTINGS, G_N_ELEMENTS(SIDEBAR_SETTINGS));
}


GtkWidget *create_sidebar(FileChosenFunction on_chosen, gpointer data) {
	Sidebar *sidebar = g_new0(Sidebar, 1);
	read_excluded_names(sidebar);
	sidebar->folders_to_open = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	sidebar->filter = GTK_FILTER(gtk_custom_filter_new(is_shown, sidebar, NULL));
	watch_settings(on_setting_changed, sidebar);
	sidebar->on_chosen = on_chosen;
	sidebar->data = data;
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(set_up_row), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_row), NULL);
	g_signal_connect(factory, "unbind", G_CALLBACK(unbind_row), NULL);
	sidebar->view = gtk_list_view_new(NULL, factory);
	gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(sidebar->view), TRUE);
	gtk_widget_add_css_class(sidebar->view, "navigation-sidebar");
	g_signal_connect(sidebar->view, "activate", G_CALLBACK(on_row_activated), sidebar);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), sidebar->view);
	gtk_widget_add_css_class(scroller, "project-tree");
	g_object_set_data_full(G_OBJECT(scroller), "sidebar", sidebar, free_sidebar);
	return scroller;
}


void set_sidebar_folder(GtkWidget *sidebar_widget, GFile *folder, const char *const *open_folders) {
	Sidebar *sidebar = g_object_get_data(G_OBJECT(sidebar_widget), "sidebar");
	g_hash_table_remove_all(sidebar->folders_to_open);
	forget_tree(sidebar);
	g_set_object(&sidebar->root, folder);
	if (!folder) {
		gtk_list_view_set_model(GTK_LIST_VIEW(sidebar->view), NULL);
		return;
	}
	for (guint index = 0; open_folders && open_folders[index]; index++)
		g_hash_table_add(sidebar->folders_to_open, g_strdup(open_folders[index]));
	GtkTreeListModel *tree = gtk_tree_list_model_new(create_folder_model(folder, sidebar), FALSE, FALSE,
		create_child_model, sidebar, NULL);
	sidebar->tree = g_object_ref(tree);
	g_signal_connect(tree, "items-changed", G_CALLBACK(on_tree_changed), sidebar);
	GtkSingleSelection *selection = gtk_single_selection_new(G_LIST_MODEL(tree));
	gtk_single_selection_set_autoselect(selection, FALSE);
	gtk_single_selection_set_can_unselect(selection, TRUE);
	gtk_single_selection_set_selected(selection, GTK_INVALID_LIST_POSITION);
	gtk_list_view_set_model(GTK_LIST_VIEW(sidebar->view), GTK_SELECTION_MODEL(selection));
	g_object_unref(selection);
}


/* Folders waiting to be read stay listed, so closing early never forgets them */
GStrv list_sidebar_open_folders(GtkWidget *sidebar_widget) {
	Sidebar *sidebar = g_object_get_data(G_OBJECT(sidebar_widget), "sidebar");
	g_autoptr(GStrvBuilder) folders = g_strv_builder_new();
	for (guint position = 0; sidebar->tree && position < g_list_model_get_n_items(G_LIST_MODEL(sidebar->tree));
		position++) {
		g_autoptr(GtkTreeListRow) row = gtk_tree_list_model_get_row(sidebar->tree, position);
		g_autoptr(GFileInfo) info = gtk_tree_list_row_get_item(row);
		if (gtk_tree_list_row_get_expanded(row))
			g_strv_builder_take(folders, get_relative_path(sidebar, info));
	}
	GHashTableIter waiting;
	gpointer path = NULL;
	g_hash_table_iter_init(&waiting, sidebar->folders_to_open);
	while (g_hash_table_iter_next(&waiting, &path, NULL))
		g_strv_builder_add(folders, path);
	return g_strv_builder_end(folders);
}
