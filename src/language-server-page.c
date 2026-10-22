#include <adwaita.h>

#include "../lang/en.h"
#include "language-server-page.h"
#include "language-servers.h"
#include "settings-dialog.h"

#define PAGE_ID "language-servers"

#define FIELD_KEY "field"

/* The settings window showing the page, its two lists and whether a server is being edited unsaved */
typedef struct {
	AdwPreferencesDialog *dialog;
	GtkWidget *ready_group, *missing_group;
	GPtrArray *rows;
	gboolean is_changed;
} ServerDialog;

static void fill_lists(ServerDialog *dialog);


/* ==================================== Editing one server ==================================== */


static void on_field_changed(GtkEditable *entry, ServerDialog *dialog) {
	char **field = g_object_get_data(G_OBJECT(entry), FIELD_KEY);
	g_set_str(field, gtk_editable_get_text(entry));
	dialog->is_changed = TRUE;
}


static GtkWidget *create_field(ServerDialog *dialog, const char *title, char **field) {
	GtkWidget *row = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
	gtk_editable_set_text(GTK_EDITABLE(row), *field);
	g_object_set_data(G_OBJECT(row), FIELD_KEY, field);
	g_signal_connect(row, "changed", G_CALLBACK(on_field_changed), dialog);
	return row;
}


static void on_program_chosen(GObject *chooser, GAsyncResult *result, gpointer data) {
	GtkEditable *entry = data;
	g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(chooser), result, NULL);
	g_autofree char *path = file ? g_file_get_path(file) : NULL;
	if (path) {
		g_autofree char *quoted = g_shell_quote(path);
		gtk_editable_set_text(entry, quoted);
	}
	g_object_unref(entry);
}


static void choose_program(GtkButton *button, GtkWidget *entry) {
	g_autoptr(GtkFileDialog) chooser = gtk_file_dialog_new();
	gtk_file_dialog_set_title(chooser, TEXT_CHOOSE_PROGRAM);
	gtk_file_dialog_open(chooser, GTK_WINDOW(gtk_widget_get_root(entry)), NULL, on_program_chosen,
		g_object_ref(entry));
}


static void remove_server(AdwButtonRow *row, ServerDialog *dialog) {
	remove_language_server(g_object_get_data(G_OBJECT(row), "server"));
	dialog->is_changed = TRUE;
	adw_preferences_dialog_pop_subpage(dialog->dialog);
}


static GtkWidget *create_editing_page(ServerDialog *dialog, LanguageServer *server) {
	GtkWidget *page = adw_preferences_page_new();
	GtkWidget *basics = adw_preferences_group_new();
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(basics), TEXT_SERVER_HELP);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(basics), create_field(dialog, TEXT_SERVER_NAME, &server->name));
	GtkWidget *command = create_field(dialog, TEXT_SERVER_COMMAND, &server->command);
	GtkWidget *browse = gtk_button_new_from_icon_name("document-open-symbolic");
	gtk_widget_set_valign(browse, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(browse, TEXT_CHOOSE_PROGRAM);
	gtk_widget_add_css_class(browse, "flat");
	g_signal_connect(browse, "clicked", G_CALLBACK(choose_program), command);
	adw_entry_row_add_suffix(ADW_ENTRY_ROW(command), browse);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(basics), command);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(basics), create_field(dialog, TEXT_SERVER_PATTERNS,
		&server->patterns));
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(basics));

	GtkWidget *advanced = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(advanced), TEXT_ADVANCED);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(advanced), create_field(dialog, TEXT_SERVER_LANGUAGE,
		&server->language_id));
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(advanced), create_field(dialog, TEXT_SERVER_OPTIONS,
		&server->initialization_options));
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(advanced));

	GtkWidget *actions = adw_preferences_group_new();
	GtkWidget *remove = adw_button_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(remove), server->is_builtin ? TEXT_RESET_SERVER
		: TEXT_REMOVE_SERVER);
	gtk_widget_add_css_class(remove, "destructive-action");
	g_object_set_data(G_OBJECT(remove), "server", server);
	g_signal_connect(remove, "activated", G_CALLBACK(remove_server), dialog);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), remove);
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(actions));
	return page;
}


/* Edits are kept until the person closes the server's page, so servers do not restart on each key */
static void finish_editing(ServerDialog *dialog) {
	/* A server added and left without a command is dropped instead of kept as an empty entry */
	GPtrArray *servers = get_language_servers();
	for (guint index = servers->len; index > 0; index--) {
		LanguageServer *server = g_ptr_array_index(servers, index - 1);
		if (!server->is_builtin && !*g_strstrip(server->command))
			remove_language_server(server);
	}
	if (dialog->is_changed) {
		save_language_servers();
		dialog->is_changed = FALSE;
	}
	fill_lists(dialog);
}


static void edit_server(ServerDialog *dialog, LanguageServer *server) {
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), create_editing_page(dialog, server));
	AdwNavigationPage *page = adw_navigation_page_new(toolbar, *server->name ? server->name : TEXT_NEW_SERVER);
	g_signal_connect_swapped(page, "hidden", G_CALLBACK(finish_editing), dialog);
	adw_preferences_dialog_push_subpage(dialog->dialog, page);
}


/* ==================================== Server lists ==================================== */


static void on_row_activated(AdwActionRow *row, ServerDialog *dialog) {
	edit_server(dialog, g_object_get_data(G_OBJECT(row), "server"));
}


static void on_enabled_changed(GtkSwitch *toggle, GParamSpec *property, ServerDialog *dialog) {
	LanguageServer *server = g_object_get_data(G_OBJECT(toggle), "server");
	server->is_enabled = gtk_switch_get_active(toggle);
	save_language_servers();
}


static GtkWidget *create_server_row(ServerDialog *dialog, LanguageServer *server, const char *program) {
	GtkWidget *row = adw_action_row_new();
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), *server->name ? server->name : TEXT_NEW_SERVER);
	g_autofree char *subtitle = program ? g_strdup_printf(TEXT_SERVER_FOUND, program, server->patterns)
		: g_strdup_printf(TEXT_SERVER_MISSING, server->command, server->patterns);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
	GtkWidget *toggle = gtk_switch_new();
	gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
	gtk_switch_set_active(GTK_SWITCH(toggle), server->is_enabled);
	gtk_widget_set_tooltip_text(toggle, TEXT_SERVER_ENABLED);
	g_object_set_data(G_OBJECT(toggle), "server", server);
	g_signal_connect(toggle, "notify::active", G_CALLBACK(on_enabled_changed), dialog);
	adw_action_row_add_suffix(ADW_ACTION_ROW(row), toggle);
	adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
	g_object_set_data(G_OBJECT(row), "server", server);
	g_signal_connect(row, "activated", G_CALLBACK(on_row_activated), dialog);
	return row;
}


static void fill_lists(ServerDialog *dialog) {
	for (guint index = 0; index < dialog->rows->len; index++) {
		GtkWidget *row = g_ptr_array_index(dialog->rows, index);
		adw_preferences_group_remove(ADW_PREFERENCES_GROUP(gtk_widget_get_ancestor(row, ADW_TYPE_PREFERENCES_GROUP)),
			row);
	}
	g_ptr_array_set_size(dialog->rows, 0);
	GPtrArray *servers = get_language_servers();
	for (guint index = 0; index < servers->len; index++) {
		LanguageServer *server = g_ptr_array_index(servers, index);
		g_autofree char *program = find_language_server_program(server);
		GtkWidget *row = create_server_row(dialog, server, program);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(program ? dialog->ready_group : dialog->missing_group), row);
		g_ptr_array_add(dialog->rows, row);
	}
}


static void add_server(ServerDialog *dialog) {
	dialog->is_changed = TRUE;
	edit_server(dialog, add_language_server());
}


static void free_dialog(gpointer data) {
	ServerDialog *dialog = data;
	g_ptr_array_unref(dialog->rows);
	g_free(dialog);
}


static void fill_page(AdwPreferencesDialog *settings, AdwPreferencesPage *page) {
	ServerDialog *dialog = g_new0(ServerDialog, 1);
	dialog->dialog = settings;
	dialog->rows = g_ptr_array_new();
	dialog->ready_group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(dialog->ready_group), TEXT_SERVERS_READY);
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(dialog->ready_group), TEXT_SERVERS_READY_DETAIL);
	GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic");
	gtk_widget_set_tooltip_text(add, TEXT_ADD_SERVER);
	gtk_widget_add_css_class(add, "flat");
	g_signal_connect_swapped(add, "clicked", G_CALLBACK(add_server), dialog);
	adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(dialog->ready_group), add);
	dialog->missing_group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(dialog->missing_group), TEXT_SERVERS_MISSING);
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(dialog->missing_group),
		TEXT_SERVERS_MISSING_DETAIL);
	adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(dialog->ready_group));
	adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(dialog->missing_group));
	g_object_set_data_full(G_OBJECT(page), "server-page", dialog, free_dialog);
	fill_lists(dialog);
}


void register_language_server_page(void) {
	add_settings_page_content(PAGE_ID, fill_page);
}
