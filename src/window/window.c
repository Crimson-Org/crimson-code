#include <string.h>

#include "lang/en.h"
#include "app/commands.h"
#include "common/app/identity.h"
#include "common/app/startup.h"
#include "editor/hex-view.h"
#include "lsp/problems-panel.h"
#include "app/menu-placement.h"
#include "app/settings.h"
#include "plan/plan-sidebar.h"
#include "sidebar.h"
#include "terminal/task-menu.h"
#include "build/tasks.h"
#include "terminal/terminal-tabs.h"
#include "terminal/terminal.h"
#include "window-private.h"
#include "common/widgets/spacing.h"
#include "common/window/file-drop.h"

/* The interface text size in pixels, which may go below the default for small screens */
#define DEFAULT_INTERFACE_TEXT "16"
#define SMALLEST_INTERFACE_TEXT 10
#define LARGEST_INTERFACE_TEXT 32
#define DOUBLE_CLICK_PRESSES 2
#define BOTTOM_PANEL_MINIMUM_HEIGHT 120
#define SIDEBAR_MINIMUM_WIDTH 160
#define PAGE_FADE_MILLISECONDS 150
/* A line to move to once a file opened from the plan has loaded, numbered from 1 so 0 means none */
#define PENDING_LINE_KEY "pending-line"

static const SettingChoice STYLE_CHOICES[] = {
	{"system", TEXT_STYLE_SYSTEM}, {"light", TEXT_STYLE_LIGHT}, {"dark", TEXT_STYLE_DARK}, {NULL, NULL},
};

/* How glyphs are fitted to pixels, with the GTK hint style each choice asks for */
static const SettingChoice RENDERING_CHOICES[] = {
	{"crisp", TEXT_RENDERING_CRISP}, {"sharp", TEXT_RENDERING_SHARP}, {"smooth", TEXT_RENDERING_SMOOTH}, {NULL, NULL},
};

static const SettingChoice TAB_BAR_CHOICES[] = {
	{"automatic", TEXT_TAB_BAR_AUTOMATIC}, {"always", TEXT_TAB_BAR_ALWAYS}, {NULL, NULL},
};

static const SettingDefinition WINDOW_SETTINGS[] = {
	{"style", "appearance", TEXT_GROUP_STYLE, TEXT_SETTING_STYLE, NULL, SETTING_CHOICE, "system", 0, 0,
		STYLE_CHOICES, NULL, {"background", "bg"}},
	{"interface-text-size", "appearance", TEXT_GROUP_STYLE, TEXT_SETTING_INTERFACE_SIZE,
		TEXT_SETTING_INTERFACE_SIZE_DETAIL, SETTING_NUMBER, DEFAULT_INTERFACE_TEXT, SMALLEST_INTERFACE_TEXT,
		LARGEST_INTERFACE_TEXT, NULL, NULL, {NULL}},
	{"font-rendering", "appearance", TEXT_GROUP_STYLE, TEXT_SETTING_RENDERING, TEXT_SETTING_RENDERING_DETAIL,
		SETTING_CHOICE, "crisp", 0, 0, RENDERING_CHOICES, NULL, {NULL}},
	{"tab-bar", "appearance", TEXT_GROUP_LAYOUT, TEXT_SETTING_TAB_BAR, NULL, SETTING_CHOICE, "automatic", 0, 0,
		TAB_BAR_CHOICES, NULL, {NULL}},
	{"show-status-bar", "appearance", TEXT_GROUP_LAYOUT, TEXT_SETTING_STATUS_BAR, NULL, SETTING_SWITCH, "true", 0, 0,
		NULL, NULL, {"laststatus", "ls"}},
	{"restore-project", "files", TEXT_GROUP_STARTUP, TEXT_SETTING_RESTORE, TEXT_SETTING_RESTORE_DETAIL,
		SETTING_SWITCH, "true", 0, 0, NULL, NULL, {NULL}},
	{"visual-editors", "files", TEXT_GROUP_STARTUP, TEXT_SETTING_VISUAL_EDITORS, TEXT_SETTING_VISUAL_EDITORS_DETAIL,
		SETTING_SWITCH, "true", 0, 0, NULL, NULL, {NULL}},
	{"restore-files", "files", TEXT_GROUP_STARTUP, TEXT_SETTING_RESTORE_FILES, TEXT_SETTING_RESTORE_DETAIL,
		SETTING_SWITCH, "false", 0, 0, NULL, NULL, {NULL}},
};

static GtkCssProvider *interface_style;


/* ==================================== Tabs ==================================== */


static void update_title(Window *window) {
	Editor *editor = get_current_editor(window);
	g_autofree char *project_name = window->project ? g_path_get_basename(window->project) : NULL;
	g_autofree char *name = editor ? get_editor_title(editor) : NULL;
	/* The tab bar hides with one file, so the title also shows unsaved changes */
	g_autofree char *file_name = name && is_editor_modified(editor) ? g_strconcat(MODIFIED_MARK, name, NULL)
		: g_strdup(name);
	adw_window_title_set_title(ADW_WINDOW_TITLE(window->title), file_name ? file_name
		: project_name ? project_name : TEXT_APPLICATION_NAME);
	adw_window_title_set_subtitle(ADW_WINDOW_TITLE(window->title), file_name && project_name ? project_name : "");
	/* Task bars and window lists read the real window title, not the header widget */
	g_autofree char *window_title = file_name || project_name ? g_strdup_printf("%s - %s", file_name ? file_name
		: project_name, TEXT_APPLICATION_NAME) : g_strdup(TEXT_APPLICATION_NAME);
	gtk_window_set_title(GTK_WINDOW(window->window), window_title);
}


/* ==================================== Status bar ==================================== */


static void set_count(GtkWidget *label, int count) {
	g_autofree char *text = g_strdup_printf("%d", count);
	gtk_label_set_text(GTK_LABEL(label), text);
	gtk_widget_set_visible(gtk_widget_get_parent(label), count > 0);
}


/* A closed revealer still takes the box's spacing, so it hides once its slide has finished */
static void on_revealer_done(GtkRevealer *revealer, GParamSpec *property, gpointer data) {
	if (!gtk_revealer_get_reveal_child(revealer))
		gtk_widget_set_visible(GTK_WIDGET(revealer), FALSE);
}


/* Status bar parts slide in and out rather than popping up */
static GtkWidget *create_revealer(GtkWidget *child) {
	GtkWidget *revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
	gtk_revealer_set_child(GTK_REVEALER(revealer), child);
	gtk_widget_set_visible(revealer, FALSE);
	g_signal_connect(revealer, "notify::child-revealed", G_CALLBACK(on_revealer_done), NULL);
	return revealer;
}


static void reveal(GtkWidget *revealer, gboolean is_shown) {
	if (is_shown)
		gtk_widget_set_visible(revealer, TRUE);
	gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), is_shown);
}


/* A menu section's heading cannot change, so the section is put back under the new name, only when it changed */
static void name_server_menu(Window *window, const char *name) {
	g_autofree char *current = NULL;
	g_menu_model_get_item_attribute(G_MENU_MODEL(window->server_menu), 0, G_MENU_ATTRIBUTE_LABEL, "s", &current);
	if (g_strcmp0(current, name) == 0)
		return;
	g_menu_remove(window->server_menu, 0);
	g_menu_insert_section(window->server_menu, 0, name, G_MENU_MODEL(window->server_actions));
}


/* The language server shows as an icon naming it on hover, dimmed while it is stopped so it can be restarted */
static void update_server_button(Window *window, Editor *editor) {
	LspClient *client = get_editor_language_server(editor);
	GFile *file = get_editor_file(editor);
	g_autofree char *path = file ? g_file_get_path(file) : NULL;
	gboolean is_stopped = !client && path && is_lsp_server_stopped(window->servers, path);
	gtk_widget_set_visible(window->server_button, client || is_stopped);
	if (is_stopped)
		gtk_widget_add_css_class(window->server_button, "dim-label");
	else
		gtk_widget_remove_css_class(window->server_button, "dim-label");
	g_autofree char *tooltip = client ? g_strdup(get_lsp_client_name(client))
		: g_strdup_printf(TEXT_SERVER_STOPPED, get_editor_language_name(editor));
	gtk_widget_set_tooltip_text(window->server_button, tooltip);
	name_server_menu(window, tooltip);
}


/* The bar slides away while it would stand empty, such as before any file is open */
static void reveal_status_bar(Window *window, Editor *editor) {
	gboolean has_content = editor || get_switch_setting("vim-mode");
	adw_toolbar_view_set_reveal_bottom_bars(ADW_TOOLBAR_VIEW(window->toolbar), has_content
		&& get_switch_setting("show-status-bar"));
}


void update_window_status(Window *window) {
	if (window->is_closed)
		return;
	Editor *editor = get_current_editor(window);
	reveal_status_bar(window, editor);
	/* A hex view shows its own offset, so lines and columns would only mislead */
	gtk_widget_set_visible(window->position_label, editor && !is_editor_hex(editor) && !is_editor_loading(editor));
	gtk_widget_set_visible(window->language_label, editor != NULL);
	refresh_window_commands(window);
	/* Easy mode is the plain default and shows nothing, only Vim modes are worth a word */
	gboolean is_vim = get_switch_setting("vim-mode");
	gtk_widget_set_visible(gtk_widget_get_parent(window->mode_label), is_vim);
	gtk_label_set_text(GTK_LABEL(window->mode_label), editor ? get_editor_mode(editor) : TEXT_MODE_NORMAL);
	if (!editor) {
		reveal(gtk_widget_get_parent(window->selection_label), FALSE);
		gtk_widget_set_visible(window->server_button, FALSE);
		gtk_label_set_text(GTK_LABEL(window->command_label), "");
		set_count(window->errors_label, 0);
		set_count(window->warnings_label, 0);
		reveal(gtk_widget_get_parent(window->problems_button), FALSE);
		return;
	}
	gtk_label_set_text(GTK_LABEL(window->command_label), get_editor_command(editor));
	int errors = 0, warnings = 0, line = 0, column = 0;
	count_editor_problems(editor, &errors, &warnings);
	set_count(window->errors_label, errors);
	set_count(window->warnings_label, warnings);
	reveal(gtk_widget_get_parent(window->problems_button), errors > 0 || warnings > 0);
	get_editor_cursor(editor, &line, &column);
	int selected = get_editor_selection_length(editor);
	g_autofree char *selection = g_strdup_printf(TEXT_SELECTED, selected);
	gtk_label_set_text(GTK_LABEL(window->selection_label), selection);
	reveal(gtk_widget_get_parent(window->selection_label), selected > 0);
	g_autofree char *position = g_strdup_printf(TEXT_POSITION, line + 1, column + 1);
	gtk_label_set_text(GTK_LABEL(window->position_label), position);
	gtk_label_set_text(GTK_LABEL(window->language_label), get_editor_language_name(editor));
	update_server_button(window, editor);
}


/* ==================================== Editor events ==================================== */


static void on_editor_state_changed(Editor *editor, gpointer data) {
	Window *window = data;
	update_window_tab(window, editor);
	refresh_window_problems(window);
	if (editor == get_current_editor(window)) {
		update_title(window);
		update_window_status(window);
	}
}


static void on_editor_cursor_moved(Editor *editor, gpointer data) {
	Window *window = data;
	if (editor == get_current_editor(window))
		update_window_status(window);
}


static void connect_language_server(Window *window, Editor *editor) {
	GFile *file = get_editor_file(editor);
	g_autofree char *path = file ? g_file_get_path(file) : NULL;
	attach_editor_language_server(editor, path ? get_lsp_client_for_file(window->servers, path) : NULL);
	on_editor_state_changed(editor, window);
}


static void on_editor_loaded(Editor *editor, gpointer data) {
	connect_language_server(data, editor);
	int pending_line = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(get_editor_widget(editor)), PENDING_LINE_KEY));
	if (pending_line > 0) {
		g_object_set_data(G_OBJECT(get_editor_widget(editor)), PENDING_LINE_KEY, NULL);
		move_editor_to_lsp_position(editor, pending_line - 1, 0);
	}
	if (editor == get_current_editor(data))
		refresh_window_for_editor(data);
}


static gboolean is_task_file(Window *window, GFile *file) {
	if (!window->project || !file)
		return FALSE;
	g_autofree char *path = g_file_get_path(file);
	g_autofree char *task_file = get_task_file_path(window->project);
	return g_strcmp0(path, task_file) == 0;
}


/* Saving the task file rewrites the scripts at once, so they never fall behind what the file says, and a tab waiting
 * on the save closes last, since closing it frees the editor */
static void on_editor_saved(Editor *editor, gpointer data) {
	Window *window = data;
	GFile *file = get_editor_file(editor);
	if (is_task_file(window, file)) {
		g_autoptr(GError) error = NULL;
		show_window_message(window, write_task_scripts(window->project, &error) ? TEXT_TASK_SCRIPTS_WRITTEN
			: error->message);
	}
	refresh_plan_sidebar_file(window->plan, file);
	LspClient *client = get_editor_language_server(editor);
	g_autofree char *path = file ? g_file_get_path(file) : NULL;
	if (path && client != get_lsp_client_for_file(window->servers, path))
		connect_language_server(window, editor);
	finish_editor_save(window, editor, TRUE);
}


static gboolean on_editor_command_requested(Editor *editor, const char *command, gpointer data) {
	return run_window_vim_command(data, command);
}


static void on_editor_failed(Editor *editor, const char *message, gpointer data) {
	finish_editor_save(data, editor, FALSE);
	show_window_message(data, message);
}


/* A save waiting for the answer about its missing folder */
typedef struct {
	Window *window;
	Editor *editor;
	GFile *file, *folder;
} FolderQuestion;


static void free_folder_question(FolderQuestion *question) {
	g_clear_object(&question->file);
	g_object_unref(question->folder);
	g_free(question);
}


static void create_folder_and_save(FolderQuestion *question) {
	/* The editor may have closed while the question was open */
	g_autoptr(GPtrArray) editors = list_window_editors(question->window);
	if (!g_ptr_array_find(editors, question->editor, NULL))
		return;
	g_autoptr(GError) error = NULL;
	gboolean is_created = g_file_make_directory_with_parents(question->folder, NULL, &error)
		|| g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
	if (!is_created) {
		show_window_message(question->window, error->message);
		return;
	}
	save_editor(question->editor, question->file);
}


static void on_folder_question_answered(GObject *dialog, GAsyncResult *result, gpointer data) {
	FolderQuestion *question = data;
	if (strcmp(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), result), "create") == 0)
		create_folder_and_save(question);
	free_folder_question(question);
}


static void on_editor_folder_missing(Editor *editor, GFile *file, gpointer data) {
	Window *window = data;
	FolderQuestion *question = g_new0(FolderQuestion, 1);
	question->window = window;
	question->editor = editor;
	question->file = file ? g_object_ref(file) : NULL;
	question->folder = g_file_get_parent(file ? file : get_editor_file(editor));
	g_autofree char *name = g_file_get_parse_name(question->folder);
	AdwDialog *dialog = adw_alert_dialog_new(TEXT_FOLDER_MISSING_QUESTION, NULL);
	adw_alert_dialog_format_body(ADW_ALERT_DIALOG(dialog), TEXT_FOLDER_MISSING_DETAIL, name);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", TEXT_CANCEL, "create", TEXT_CREATE_AND_SAVE,
		NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "create", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "create");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), window->window, NULL, on_folder_question_answered, question);
}


void get_window_editor_events(Window *window, EditorEvents *events) {
	*events = (EditorEvents){on_editor_state_changed, on_editor_cursor_moved, on_editor_loaded, on_editor_saved,
		on_editor_command_requested, on_editor_failed, on_editor_folder_missing, window};
}


void refresh_window_for_editor(Window *window) {
	Editor *editor = get_current_editor(window);
	/* Binary files are searched as bytes in their hex view, and every other file as text */
	GtkWidget *hex = editor ? get_editor_hex_view(editor) : NULL;
	if (hex)
		set_find_bar_target(window->find_bar, get_hex_find_target(), hex);
	else
		set_find_bar_view(window->find_bar, editor ? get_editor_view(editor) : NULL);
	update_title(window);
	update_window_status(window);
	update_mode_switch(window);
}


/* ==================================== Projects, servers and tasks ==================================== */


void show_window_message(Window *window, const char *message) {
	/* Messages hold paths and error texts, which are plain text and never markup */
	AdwToast *toast = adw_toast_new(message);
	adw_toast_set_use_markup(toast, FALSE);
	adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(window->toasts), toast);
}


/* Detaches the views first, since stopping a server while views still hold it would leave them talking to nothing */
void stop_window_language_server(Window *window) {
	Editor *current = get_current_editor(window);
	LspClient *client = current ? get_editor_language_server(current) : NULL;
	if (!client)
		return;
	g_autoptr(GPtrArray) editors = list_window_editors(window);
	for (guint index = 0; index < editors->len; index++)
		if (get_editor_language_server(g_ptr_array_index(editors, index)) == client)
			attach_editor_language_server(g_ptr_array_index(editors, index), NULL);
	stop_lsp_client(window->servers, client);
	update_window_status(window);
}


void restart_window_language_servers(Window *window) {
	g_autoptr(GPtrArray) editors = list_window_editors(window);
	for (guint index = 0; index < editors->len; index++)
		attach_editor_language_server(g_ptr_array_index(editors, index), NULL);
	restart_lsp_clients(window->servers);
	for (guint index = 0; index < editors->len; index++)
		connect_language_server(window, g_ptr_array_index(editors, index));
}


void reconnect_language_servers(Window *window) {
	g_autoptr(GPtrArray) editors = list_window_editors(window);
	for (guint index = 0; index < editors->len; index++)
		attach_editor_language_server(g_ptr_array_index(editors, index), NULL);
	stop_lsp_clients(window->servers);
	for (guint index = 0; index < editors->len; index++)
		connect_language_server(window, g_ptr_array_index(editors, index));
}


/* A project with its files in view needs nothing more, while a hidden sidebar or no project gets a way forward */
/* The open buttons wait while paths load and while the page fades out behind newly opened files */
void update_window_empty_page(Window *window) {
	gboolean has_project = window->project != NULL;
	gboolean is_shown = g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(window->editor_stack)), "empty") == 0;
	gboolean offers_opening = !has_project && !window->is_loading && is_shown;
	AdwStatusPage *page = ADW_STATUS_PAGE(window->empty_page);
	/* The app's own icon greets a window with nothing open, while a project's blank page stays blank */
	adw_status_page_set_icon_name(page, offers_opening ? get_application_identity()->id : NULL);
	gtk_widget_set_visible(window->open_buttons, offers_opening);
	gtk_widget_set_visible(window->show_files_button, has_project && !is_window_sidebar_visible(window));
}


void set_window_loading(Window *window, gboolean is_loading) {
	window->is_loading = is_loading;
	update_window_empty_page(window);
}


gboolean is_window_sidebar_visible(Window *window) {
	return is_panel_slide_shown(window->sidebar_slide);
}


static void slide_sidebar(Window *window, gboolean is_visible) {
	if (is_visible == is_window_sidebar_visible(window))
		return;
	if (!is_visible) {
		window->state.sidebar_width = get_panel_slide_size(window->sidebar_slide);
		hide_panel_slide(window->sidebar_slide);
		return;
	}
	show_panel_slide(window->sidebar_slide, MAX(window->state.sidebar_width, SIDEBAR_MINIMUM_WIDTH));
}


void set_window_sidebar_visible(Window *window, gboolean is_visible) {
	window->state.shows_sidebar = is_visible;
	/* Window buttons on the left sit in the sidebar's own header while it shows */
	adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(window->header), !is_visible);
	slide_sidebar(window, is_visible);
	update_window_empty_page(window);
}


gboolean is_window_files_visible(Window *window) {
	return is_window_sidebar_visible(window) && !window->state.shows_plan;
}


gboolean is_window_plan_visible(Window *window) {
	return is_window_sidebar_visible(window) && window->state.shows_plan;
}


void toggle_window_sidebar_page(Window *window, gboolean is_plan) {
	if (is_window_sidebar_visible(window) && window->state.shows_plan == is_plan) {
		set_window_sidebar_visible(window, FALSE);
		return;
	}
	window->state.shows_plan = is_plan;
	gtk_stack_set_visible_child_name(GTK_STACK(window->sidebar_stack), is_plan ? "plan" : "files");
	set_window_sidebar_visible(window, TRUE);
}


static void save_project_rows(Window *window) {
	if (!window->project)
		return;
	ProjectState state = {list_sidebar_open_folders(window->sidebar), list_plan_open_rows(window->plan)};
	save_project_state(window->project, &state);
	clear_project_state(&state);
}


/* Shared by opening and closing, since both hand the files and servers to a new root or to none */
static void set_project(Window *window, const char *directory) {
	save_project_rows(window);
	g_set_str(&window->project, directory);
	g_set_str(&window->state.project, directory);
	g_autoptr(GFile) folder = directory ? g_file_new_for_path(directory) : NULL;
	g_auto(ProjectState) rows = {0};
	if (directory)
		load_project_state(directory, &rows);
	set_sidebar_folder(window->sidebar, folder, (const char *const *)rows.open_folders);
	set_plan_sidebar_folder(window->plan, directory, (const char *const *)rows.open_plan_rows);
	slide_sidebar(window, directory && window->state.shows_sidebar);
	set_task_button_project(window->task_button, directory);
	set_lsp_manager_root(window->servers, directory);
	reconnect_language_servers(window);
	update_title(window);
	update_window_status(window);
	update_window_empty_page(window);
}


void open_window_project(Window *window, const char *directory) {
	set_project(window, directory);
}


/* Open files stay open, so closing the project never loses unsaved work */
void close_window_project(Window *window) {
	set_project(window, NULL);
}


static gboolean is_bottom_page_visible(Window *window, GtkWidget *page) {
	return is_panel_slide_shown(window->bottom_slide)
		&& adw_view_stack_get_visible_child(ADW_VIEW_STACK(window->bottom_stack)) == page;
}


void hide_window_panel(Window *window) {
	if (!is_panel_slide_shown(window->bottom_slide))
		return;
	window->state.terminal_height = get_panel_slide_size(window->bottom_slide);
	hide_panel_slide(window->bottom_slide);
	Editor *editor = get_current_editor(window);
	if (editor)
		gtk_widget_grab_focus(GTK_WIDGET(get_editor_view(editor)));
	refresh_window_commands(window);
}


/* The header's terminal tabs and new terminal button belong to the terminal page only */
static void show_terminal_controls(Window *window, GtkWidget *page) {
	gboolean is_terminal = page == window->terminals;
	gtk_widget_set_visible(window->new_terminal_button, is_terminal);
	gtk_widget_set_visible(window->terminal_tab_bar, is_terminal);
}


/* Switching pages keeps the panel's height, so the terminal and the problems list share one place */
static void show_bottom_page(Window *window, GtkWidget *page) {
	adw_view_stack_set_visible_child(ADW_VIEW_STACK(window->bottom_stack), page);
	show_terminal_controls(window, page);
	if (!is_panel_slide_shown(window->bottom_slide))
		show_panel_slide(window->bottom_slide, MAX(window->state.terminal_height, BOTTOM_PANEL_MINIMUM_HEIGHT));
	refresh_window_commands(window);
}


static const char *get_terminal_folder(Window *window) {
	return window->project ? window->project : g_get_home_dir();
}


gboolean is_window_terminal_visible(Window *window) {
	return is_bottom_page_visible(window, window->terminals);
}


void set_window_terminal_visible(Window *window, gboolean is_visible) {
	if (!is_visible) {
		if (is_window_terminal_visible(window))
			hide_window_panel(window);
		return;
	}
	show_bottom_page(window, window->terminals);
	gtk_widget_grab_focus(get_current_terminal(window->terminals, get_terminal_folder(window)));
}


void open_window_terminal(Window *window) {
	show_bottom_page(window, window->terminals);
	gtk_widget_grab_focus(add_terminal_tab(window->terminals, get_terminal_folder(window)));
}


static void on_terminals_emptied(gpointer data) {
	Window *window = data;
	if (is_window_terminal_visible(window))
		hide_window_panel(window);
}


/* The switcher names the page and moves between the terminal and the problems list */
static void on_bottom_page_changed(AdwViewStack *stack, GParamSpec *property, Window *window) {
	GtkWidget *page = adw_view_stack_get_visible_child(stack);
	show_terminal_controls(window, page);
	if (page == window->problems_panel)
		refresh_window_problems(window);
	else if (page == window->terminals)
		get_current_terminal(window->terminals, get_terminal_folder(window));
	refresh_window_commands(window);
}


gboolean is_window_problems_visible(Window *window) {
	return is_bottom_page_visible(window, window->problems_panel);
}


void set_window_problems_visible(Window *window, gboolean is_visible) {
	if (!is_visible) {
		if (is_window_problems_visible(window))
			hide_window_panel(window);
		return;
	}
	show_bottom_page(window, window->problems_panel);
	refresh_window_problems(window);
	focus_problems_panel(window->problems_panel);
}


void refresh_window_problems(Window *window) {
	if (!is_window_problems_visible(window))
		return;
	g_autoptr(GPtrArray) editors = list_window_editors(window);
	set_problems_panel_editors(window->problems_panel, editors);
}


static void on_problem_chosen(GFile *file, int line, int column, gpointer data) {
	Editor *editor = show_window_file(data, file);
	if (editor)
		move_editor_to_lsp_position(editor, line, column);
}


/* Escape leaves the list for the editor, as it leaves other panels and popups */
static gboolean on_problems_key(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType state,
	Window *window) {
	if (key != GDK_KEY_Escape)
		return GDK_EVENT_PROPAGATE;
	set_window_problems_visible(window, FALSE);
	return GDK_EVENT_STOP;
}


/* Writes the scripts first, so the task runs exactly as the saved task file describes */
void run_window_task(Window *window, const char *name) {
	if (!window->project)
		return;
	g_autoptr(GError) error = NULL;
	if (!write_task_scripts(window->project, &error)) {
		show_window_message(window, error->message);
		return;
	}
	g_autofree char *command = get_task_command(window->project, name);
	set_window_terminal_visible(window, TRUE);
	run_terminal_command(get_current_terminal(window->terminals, window->project), window->project, command);
}


void choose_window_task(const char *name, gpointer data) {
	Window *window = data;
	if (name) {
		run_window_task(window, name);
		return;
	}
	g_autoptr(GError) error = NULL;
	if (!create_task_file(window->project, &error)) {
		show_window_message(window, error->message);
		return;
	}
	g_autofree char *path = get_task_file_path(window->project);
	g_autoptr(GFile) file = g_file_new_for_path(path);
	open_window_file(window, file);
}


static void on_file_chosen(GFile *file, gpointer data) {
	open_window_file(data, file);
}


/* A file still loading is moved to the line once it has loaded */
static void on_plan_spot_chosen(GFile *file, int line, gpointer data) {
	Editor *editor = show_window_file(data, file);
	if (!editor)
		return;
	if (is_editor_loading(editor))
		g_object_set_data(G_OBJECT(get_editor_widget(editor)), PENDING_LINE_KEY, GINT_TO_POINTER(line + 1));
	else
		move_editor_to_lsp_position(editor, line, 0);
}


static Editor *find_plan_editor(Window *window, GFile *file) {
	Editor *editor = find_window_file(window, file);
	return editor && !is_editor_loading(editor) && !is_editor_hex(editor) ? editor : NULL;
}


static char *get_open_plan_text(GFile *file, gpointer data) {
	Editor *editor = find_plan_editor(data, file);
	if (!editor)
		return NULL;
	return get_editor_file_text(editor);
}


/* An open plan file is changed in its editor and saved, so the editor never holds an older copy */
static gboolean change_open_plan_text(GFile *file, const char *text, gpointer data) {
	Editor *editor = find_plan_editor(data, file);
	if (!editor)
		return FALSE;
	replace_editor_text(editor, text);
	save_editor(editor, NULL);
	return TRUE;
}


static void on_plan_failed(const char *message, gpointer data) {
	show_window_message(data, message);
}


void open_window_path(Window *window, GFile *file) {
	g_autofree char *path = g_file_get_path(file);
	if (path && g_file_test(path, G_FILE_TEST_IS_DIR))
		open_window_project(window, path);
	else
		open_window_file(window, file);
}


/* Files that were moved or deleted since are skipped, and the one in front last time comes to the front again */
static void restore_files(Window *window) {
	char **paths = window->state.files;
	for (guint index = 0; paths && paths[index]; index++)
		if (g_file_test(paths[index], G_FILE_TEST_IS_REGULAR)) {
			g_autoptr(GFile) file = g_file_new_for_path(paths[index]);
			open_window_file(window, file);
		}
	const char *current = window->state.current_file;
	if (current && g_file_test(current, G_FILE_TEST_IS_REGULAR)) {
		g_autoptr(GFile) file = g_file_new_for_path(current);
		show_window_file(window, file);
	}
}


void restore_window_project(Window *window) {
	const char *project = window->state.project;
	if (get_switch_setting("restore-project") && project && g_file_test(project, G_FILE_TEST_IS_DIR))
		open_window_project(window, window->state.project);
	if (get_switch_setting("restore-files"))
		restore_files(window);
}


/* Each file is listed once even when several panes show it, and untitled files have no path to keep */
static void remember_open_files(Window *window) {
	g_autoptr(GPtrArray) editors = list_window_editors(window);
	g_autoptr(GStrvBuilder) paths = g_strv_builder_new();
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (guint index = 0; index < editors->len; index++) {
		GFile *file = get_editor_file(g_ptr_array_index(editors, index));
		char *path = file ? g_file_get_path(file) : NULL;
		if (!path || g_hash_table_contains(seen, path)) {
			g_free(path);
			continue;
		}
		g_strv_builder_add(paths, path);
		g_hash_table_add(seen, path);
	}
	g_strfreev(window->state.files);
	window->state.files = g_strv_builder_end(paths);
	Editor *current = get_current_editor(window);
	GFile *current_file = current ? get_editor_file(current) : NULL;
	g_free(window->state.current_file);
	window->state.current_file = current_file ? g_file_get_path(current_file) : NULL;
}


/* ==================================== Layout ==================================== */


/* Files with a visual editor show Text and Visual in the title bar, where the choice is easy to see */
void update_mode_switch(Window *window) {
	if (window->is_closed)
		return;
	Editor *editor = get_current_editor(window);
	gboolean is_supported = editor && is_editor_json(editor);
	gtk_widget_set_visible(window->mode_switch, is_supported);
	if (!is_supported)
		return;
	window->is_updating_mode = TRUE;
	adw_toggle_group_set_active_name(ADW_TOGGLE_GROUP(window->mode_switch),
		is_editor_visual(editor) ? "visual" : "text");
	window->is_updating_mode = FALSE;
}


static void on_mode_chosen(GObject *group, GParamSpec *property, Window *window) {
	Editor *editor = get_current_editor(window);
	if (window->is_updating_mode || !editor)
		return;
	set_editor_visual(editor, g_strcmp0(adw_toggle_group_get_active_name(ADW_TOGGLE_GROUP(group)), "visual") == 0);
	refresh_window_commands(window);
}


static GtkWidget *create_mode_switch(Window *window) {
	GtkWidget *group = adw_toggle_group_new();
	const char *const names[] = {"text", "visual"}, *const labels[] = {TEXT_EDITOR_TEXT, TEXT_EDITOR_VISUAL};
	for (guint index = 0; index < G_N_ELEMENTS(names); index++) {
		AdwToggle *toggle = adw_toggle_new();
		adw_toggle_set_name(toggle, names[index]);
		adw_toggle_set_label(toggle, labels[index]);
		adw_toggle_group_add(ADW_TOGGLE_GROUP(group), toggle);
	}
	gtk_widget_add_css_class(group, "mode-switch");
	gtk_widget_set_valign(group, GTK_ALIGN_CENTER);
	gtk_widget_set_visible(group, FALSE);
	g_signal_connect(group, "notify::active-name", G_CALLBACK(on_mode_chosen), window);
	return group;
}


/* A panel's button stays pressed in while its panel shows, so the title bar tells at a glance what is open */
static GtkWidget *create_panel_toggle(const char *icon, const char *tooltip, const char *action) {
	GtkWidget *button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(button), icon);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
	gtk_widget_set_tooltip_text(button, tooltip);
	return button;
}


static GtkWidget *create_header(Window *window) {
	GtkWidget *header = adw_header_bar_new();
	window->header = header;
	gtk_widget_add_css_class(header, "title-bar");
	window->title = GTK_WIDGET(adw_window_title_new(TEXT_APPLICATION_NAME, ""));
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), window->title);
	window->sidebar_button = create_panel_toggle("folder-symbolic", TEXT_TOGGLE_SIDEBAR, "win.toggle-sidebar");
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), window->sidebar_button);
	window->plan_button = create_panel_toggle("checkbox-checked-symbolic", TEXT_TOGGLE_PLAN, "win.toggle-plan");
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), window->plan_button);
	GtkWidget *menu = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "open-menu-symbolic");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu), create_window_menu());
	place_popover_at_button_end(gtk_menu_button_get_popover(GTK_MENU_BUTTON(menu)));
	gtk_widget_set_tooltip_text(menu, TEXT_MENU);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), create_panel_toggle("utilities-terminal-symbolic",
		TEXT_TOGGLE_TERMINAL, "win.toggle-terminal"));
	window->mode_switch = create_mode_switch(window);
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), window->mode_switch);
	window->task_button = create_task_button(choose_window_task, window);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), window->task_button);
	return header;
}


/* A double click on blank space starts a new file, as in many editors */
static void on_empty_page_pressed(GtkGestureClick *gesture, int presses, double x, double y, Window *window) {
	if (presses == DOUBLE_CLICK_PRESSES)
		gtk_widget_activate_action(window->window, "win.new-file", NULL);
}


static GtkWidget *create_empty_page(Window *window) {
	GtkWidget *page = adw_status_page_new();
	GtkGesture *double_click = gtk_gesture_click_new();
	g_signal_connect(double_click, "pressed", G_CALLBACK(on_empty_page_pressed), window);
	gtk_widget_add_controller(page, GTK_EVENT_CONTROLLER(double_click));
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_NORMAL);
	gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
	GtkWidget *folder = gtk_button_new_with_label(TEXT_OPEN_FOLDER);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(folder), "win.open-folder");
	gtk_widget_add_css_class(folder, "pill");
	gtk_widget_add_css_class(folder, "suggested-action");
	GtkWidget *file = gtk_button_new_with_label(TEXT_OPEN_FILE);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(file), "win.open-file");
	gtk_widget_add_css_class(file, "pill");
	gtk_box_append(GTK_BOX(buttons), folder);
	gtk_box_append(GTK_BOX(buttons), file);
	GtkWidget *show_files = gtk_button_new_with_label(TEXT_SHOW_FILES);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(show_files), "win.toggle-sidebar");
	gtk_widget_add_css_class(show_files, "pill");
	gtk_widget_add_css_class(show_files, "suggested-action");
	gtk_widget_set_halign(show_files, GTK_ALIGN_CENTER);
	GtkWidget *choices = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(choices), buttons);
	gtk_box_append(GTK_BOX(choices), show_files);
	adw_status_page_set_child(ADW_STATUS_PAGE(page), choices);
	window->empty_page = page;
	window->open_buttons = buttons;
	window->show_files_button = show_files;
	return page;
}


static GtkWidget *create_editor_area(Window *window) {
	/* The panes hold the find card, over whichever of them is in front */
	window->find_bar = create_find_bar();
	GtkWidget *editors = create_window_panes(window);
	window->editor_stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(window->editor_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_transition_duration(GTK_STACK(window->editor_stack), PAGE_FADE_MILLISECONDS);
	gtk_stack_add_named(GTK_STACK(window->editor_stack), create_empty_page(window), "empty");
	gtk_stack_add_named(GTK_STACK(window->editor_stack), editors, "editors");
	return window->editor_stack;
}


static GtkWidget *create_counter(const char *icon, GtkWidget **label, const char *style) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_TIGHT);
	GtkWidget *image = gtk_image_new_from_icon_name(icon);
	gtk_widget_add_css_class(image, style);
	*label = gtk_label_new("0");
	gtk_box_append(GTK_BOX(box), image);
	gtk_box_append(GTK_BOX(box), *label);
	gtk_widget_set_visible(box, FALSE);
	return box;
}


/* Labels and buttons carry the same side padding, so every part of the bar sits the same distance apart */
static GtkWidget *create_status_bar(Window *window) {
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SMALL);
	gtk_widget_add_css_class(bar, "status-bar");
	/* The Vim mode name is also the quickest way back to Easy mode */
	window->mode_label = gtk_label_new(NULL);
	gtk_widget_add_css_class(window->mode_label, "mode");
	GtkWidget *mode_button = gtk_button_new();
	gtk_button_set_child(GTK_BUTTON(mode_button), window->mode_label);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(mode_button), "win.vim-mode");
	gtk_widget_set_tooltip_text(mode_button, TEXT_TURN_OFF_VIM);
	gtk_widget_add_css_class(mode_button, "flat");
	gtk_widget_set_focus_on_click(mode_button, FALSE);
	window->command_label = gtk_label_new("");
	gtk_widget_add_css_class(window->command_label, "monospace");
	/* Pushes the counts and position to the right, since problem messages show on hover instead */
	GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(spacer, TRUE);
	window->position_label = gtk_label_new("");
	window->selection_label = gtk_label_new("");
	window->language_label = gtk_label_new("");
	window->server_button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(window->server_button), "network-server-symbolic");
	/* The first section acts on the server named in its heading, filled in as the file in front changes, and the
	 * second on every server */
	window->server_menu = g_menu_new();
	window->server_actions = g_menu_new();
	g_menu_append(window->server_actions, TEXT_STOP_SERVER, "win.stop-language-server");
	g_menu_append_section(window->server_menu, NULL, G_MENU_MODEL(window->server_actions));
	g_autoptr(GMenu) every_server = g_menu_new();
	g_menu_append(every_server, TEXT_RESTART_SERVERS, "win.restart-language-servers");
	g_menu_append(every_server, TEXT_EDIT_SERVERS, "win.language-servers");
	g_menu_append_section(window->server_menu, NULL, G_MENU_MODEL(every_server));
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(window->server_button), G_MENU_MODEL(window->server_menu));
	gtk_menu_button_set_direction(GTK_MENU_BUTTON(window->server_button), GTK_ARROW_UP);
	/* The button sits in the window's corner, where an arrow would run into the menu's rounded corner */
	GtkPopover *server_popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(window->server_button));
	gtk_widget_set_halign(GTK_WIDGET(server_popover), GTK_ALIGN_END);
	gtk_popover_set_has_arrow(server_popover, FALSE);
	gtk_popover_set_offset(server_popover, 0, -SPACING_SMALL);
	gtk_widget_add_css_class(window->server_button, "flat");
	gtk_box_append(GTK_BOX(bar), mode_button);
	gtk_box_append(GTK_BOX(bar), window->command_label);
	gtk_box_append(GTK_BOX(bar), spacer);
	GtkWidget *counters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_NORMAL);
	gtk_box_append(GTK_BOX(counters), create_counter("dialog-error-symbolic", &window->errors_label, "error"));
	gtk_box_append(GTK_BOX(counters), create_counter("dialog-warning-symbolic", &window->warnings_label, "warning"));
	window->problems_button = gtk_button_new();
	gtk_button_set_child(GTK_BUTTON(window->problems_button), counters);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(window->problems_button), "win.toggle-problems");
	gtk_widget_set_tooltip_text(window->problems_button, TEXT_PROBLEMS);
	gtk_widget_add_css_class(window->problems_button, "flat");
	gtk_box_append(GTK_BOX(bar), create_revealer(window->problems_button));
	gtk_box_append(GTK_BOX(bar), create_revealer(window->selection_label));
	gtk_box_append(GTK_BOX(bar), window->position_label);
	gtk_box_append(GTK_BOX(bar), window->language_label);
	gtk_box_append(GTK_BOX(bar), window->server_button);
	return bar;
}


static GtkWidget *create_panel_button(const char *icon, const char *tooltip, const char *action) {
	GtkWidget *button = gtk_button_new_from_icon_name(icon);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	return button;
}


/* A slim title bar names the panel's pages, which also switch between them, with a close button at the end */
static GtkWidget *create_bottom_panel(Window *window) {
	window->terminals = create_terminal_tabs(on_terminals_emptied, window);
	window->problems_panel = create_problems_panel(on_problem_chosen, window);
	GtkEventController *problem_keys = gtk_event_controller_key_new();
	g_signal_connect(problem_keys, "key-pressed", G_CALLBACK(on_problems_key), window);
	gtk_widget_add_controller(window->problems_panel, problem_keys);
	window->bottom_stack = adw_view_stack_new();
	adw_view_stack_set_enable_transitions(ADW_VIEW_STACK(window->bottom_stack), TRUE);
	gtk_widget_set_vexpand(window->bottom_stack, TRUE);
	adw_view_stack_add_titled(ADW_VIEW_STACK(window->bottom_stack), window->terminals, "terminal", TEXT_TERMINAL);
	adw_view_stack_add_titled(ADW_VIEW_STACK(window->bottom_stack), window->problems_panel, "problems",
		TEXT_PROBLEMS);
	g_signal_connect(window->bottom_stack, "notify::visible-child", G_CALLBACK(on_bottom_page_changed), window);
	/* The pages switch like the Text and Visual choice in the title bar, as flat toggles */
	GtkWidget *switcher = adw_inline_view_switcher_new();
	adw_inline_view_switcher_set_stack(ADW_INLINE_VIEW_SWITCHER(switcher), ADW_VIEW_STACK(window->bottom_stack));
	gtk_widget_add_css_class(switcher, "flat");
	gtk_widget_set_valign(switcher, GTK_ALIGN_CENTER);
	/* Several terminals show their tabs beside the pages in this one header, instead of in a second row below */
	GtkWidget *tabs_place = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(tabs_place, TRUE);
	window->terminal_tab_bar = get_terminal_tab_bar(window->terminals);
	gtk_widget_set_hexpand(window->terminal_tab_bar, TRUE);
	gtk_widget_set_valign(window->terminal_tab_bar, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(tabs_place), window->terminal_tab_bar);
	window->new_terminal_button = create_panel_button("list-add-symbolic", TEXT_NEW_TERMINAL, "win.new-terminal");
	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_TIGHT);
	gtk_widget_add_css_class(header, "panel-header");
	gtk_box_append(GTK_BOX(header), switcher);
	gtk_box_append(GTK_BOX(header), tabs_place);
	gtk_box_append(GTK_BOX(header), window->new_terminal_button);
	gtk_box_append(GTK_BOX(header), create_panel_button("window-close-symbolic", TEXT_HIDE_PANEL, "win.hide-panel"));
	GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(panel, "bottom-panel");
	gtk_box_append(GTK_BOX(panel), header);
	gtk_box_append(GTK_BOX(panel), window->bottom_stack);
	gtk_widget_set_visible(panel, FALSE);
	return panel;
}


static GtkWidget *create_content(Window *window) {
	window->terminal_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
	gtk_paned_set_start_child(GTK_PANED(window->terminal_paned), create_editor_area(window));
	gtk_paned_set_end_child(GTK_PANED(window->terminal_paned), create_bottom_panel(window));
	gtk_paned_set_shrink_end_child(GTK_PANED(window->terminal_paned), FALSE);
	window->bottom_slide = create_panel_slide(GTK_PANED(window->terminal_paned), TRUE);
	window->sidebar = create_sidebar(on_file_chosen, window);
	PlanEvents plan_events = {on_plan_spot_chosen, get_open_plan_text, change_open_plan_text, on_plan_failed, window};
	window->plan = create_plan_sidebar(&plan_events);
	/* The sidebar runs the full height of the window beside the title bar, as libadwaita sidebars do, and the
	 * status bar belongs to the files and panels it describes */
	window->sidebar_stack = gtk_stack_new();
	gtk_widget_add_css_class(window->sidebar_stack, "sidebar-column");
	gtk_stack_add_named(GTK_STACK(window->sidebar_stack), window->sidebar, "files");
	gtk_stack_add_named(GTK_STACK(window->sidebar_stack), window->plan, "plan");
	gtk_stack_set_visible_child_name(GTK_STACK(window->sidebar_stack), window->state.shows_plan ? "plan" : "files");
	gtk_widget_set_visible(window->sidebar_stack, FALSE);
	GtkWidget *toolbar = adw_toolbar_view_new();
	gtk_widget_add_css_class(toolbar, "title-bar");
	window->toolbar = toolbar;
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), create_header(window));
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), window->terminal_paned);
	adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), create_status_bar(window));
	window->sidebar_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_set_start_child(GTK_PANED(window->sidebar_paned), window->sidebar_stack);
	gtk_paned_set_end_child(GTK_PANED(window->sidebar_paned), toolbar);
	gtk_paned_set_shrink_start_child(GTK_PANED(window->sidebar_paned), FALSE);
	gtk_paned_set_resize_start_child(GTK_PANED(window->sidebar_paned), FALSE);
	gtk_paned_set_position(GTK_PANED(window->sidebar_paned), window->state.sidebar_width);
	window->sidebar_slide = create_panel_slide(GTK_PANED(window->sidebar_paned), FALSE);
	window->toasts = adw_toast_overlay_new();
	adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(window->toasts), window->sidebar_paned);
	return window->toasts;
}


/* ==================================== Window lifetime ==================================== */


static void drop_files(GFile **files, guint count, gpointer data) {
	for (guint index = 0; index < count; index++)
		open_window_path(data, files[index]);
}


static gboolean has_modified_editor(Window *window) {
	g_autoptr(GPtrArray) editors = list_window_editors(window);
	for (guint index = 0; index < editors->len; index++)
		if (is_editor_modified(g_ptr_array_index(editors, index)))
			return TRUE;
	return FALSE;
}


static void on_quit_answered(GObject *dialog, GAsyncResult *result, gpointer data) {
	Window *window = data;
	if (strcmp(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), result), "discard") != 0)
		return;
	window->is_close_confirmed = TRUE;
	gtk_window_close(GTK_WINDOW(window->window));
}


static void save_state(Window *window) {
	GtkWindow *gtk_window = GTK_WINDOW(window->window);
	read_window_geometry(gtk_window, &window->state.geometry);
	remember_open_files(window);
	if (is_panel_slide_shown(window->sidebar_slide))
		window->state.sidebar_width = get_panel_slide_size(window->sidebar_slide);
	if (is_panel_slide_shown(window->bottom_slide))
		window->state.terminal_height = get_panel_slide_size(window->bottom_slide);
	save_window_state(&window->state);
	save_project_rows(window);
}


static gboolean on_close_request(GtkWindow *gtk_window, Window *window) {
	if (!window->is_close_confirmed && has_modified_editor(window)) {
		AdwDialog *dialog = adw_alert_dialog_new(TEXT_DISCARD_QUESTION, TEXT_DISCARD_DETAIL);
		adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", TEXT_CANCEL, "discard", TEXT_DISCARD,
			NULL);
		adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "discard", ADW_RESPONSE_DESTRUCTIVE);
		adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
		adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), window->window, NULL, on_quit_answered, window);
		return GDK_EVENT_STOP;
	}
	save_state(window);
	hide_closing_window(gtk_window);
	unbind_window_commands(window);
	unwatch_settings(window);
	unwatch_language_servers(window);
	/* The panes outlive the editors by a moment, so they must stop calling back before they go */
	disconnect_window_panes(window);
	set_find_bar_view(window->find_bar, NULL);
	g_autoptr(GPtrArray) editors = list_window_editors(window);
	window->is_closed = TRUE;
	for (guint index = 0; index < editors->len; index++)
		free_editor(g_ptr_array_index(editors, index));
	stop_lsp_clients(window->servers);
	return GDK_EVENT_PROPAGATE;
}


static void free_window(gpointer data) {
	Window *window = data;
	free_lsp_manager(window->servers);
	g_ptr_array_unref(window->panes);
	g_ptr_array_unref(window->closed_files);
	free_panel_slide(window->sidebar_slide);
	free_panel_slide(window->bottom_slide);
	g_object_unref(window->server_menu);
	g_object_unref(window->server_actions);
	clear_window_state(&window->state);
	g_free(window->project);
	g_free(window);
}


/* A header button shows only while its command can run, instead of sitting there greyed out */
static void show_while_available(Window *window, GtkWidget *button, const char *command) {
	GAction *action = g_action_map_lookup_action(G_ACTION_MAP(window->window), command);
	g_object_bind_property(action, "enabled", button, "visible", G_BINDING_SYNC_CREATE);
}


/* Settings that shape this window's own layout */
static void apply_window_layout(const char *key, gpointer data) {
	Window *window = data;
	apply_window_pane_settings(window);
	reveal_status_bar(window, get_current_editor(window));
	if (!key || g_strcmp0(key, "vim-mode") == 0)
		update_window_status(window);
}


static void on_language_servers_changed(gpointer data) {
	reconnect_language_servers(data);
}


/* Smooth keeps GTK's own choice of fractional glyph positions, while crisp and sharp round glyphs to whole
 * pixels as browsers and VS Code do, sharp also fitting letter shapes fully to the pixel grid */
static void apply_font_rendering(void) {
	const char *rendering = get_text_setting("font-rendering");
	gboolean is_smooth = g_strcmp0(rendering, "smooth") == 0;
	GtkSettings *settings = gtk_settings_get_default();
	if (is_smooth) {
		g_object_set(settings, "gtk-font-rendering", GTK_FONT_RENDERING_AUTOMATIC, NULL);
		return;
	}
	g_object_set(settings, "gtk-xft-antialias", 1, "gtk-xft-hinting", 1, "gtk-xft-hintstyle",
		g_strcmp0(rendering, "sharp") == 0 ? "hintfull" : "hintslight", "gtk-hint-font-metrics", TRUE,
		"gtk-font-rendering", GTK_FONT_RENDERING_MANUAL, NULL);
}


/* The colour style, text rendering and interface text size apply to each window at once */
static void apply_interface_style(const char *key, gpointer data) {
	if (key && g_strcmp0(key, "style") != 0 && g_strcmp0(key, "interface-text-size") != 0
		&& g_strcmp0(key, "font-rendering") != 0)
		return;
	apply_font_rendering();
	const char *style = get_text_setting("style");
	adw_style_manager_set_color_scheme(adw_style_manager_get_default(), g_strcmp0(style, "light") == 0
		? ADW_COLOR_SCHEME_FORCE_LIGHT : g_strcmp0(style, "dark") == 0 ? ADW_COLOR_SCHEME_FORCE_DARK
		: ADW_COLOR_SCHEME_DEFAULT);
	if (!interface_style) {
		interface_style = gtk_css_provider_new();
		gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(interface_style),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	}
	g_autofree char *css = g_strdup_printf("window { font-size: %dpx; }", get_number_setting("interface-text-size"));
	gtk_css_provider_load_from_string(interface_style, css);
}


void register_window_settings(void) {
	register_settings(WINDOW_SETTINGS, G_N_ELEMENTS(WINDOW_SETTINGS));
	apply_interface_style(NULL, NULL);
	watch_settings(apply_interface_style, NULL);
}


Window *create_window(GtkApplication *application) {
	Window *window = g_new0(Window, 1);
	load_window_state(&window->state);
	window->servers = create_lsp_manager();
	window->window = adw_application_window_new(application);
	g_object_set_data_full(G_OBJECT(window->window), "window", window, free_window);
	apply_window_geometry(GTK_WINDOW(window->window), &window->state.geometry);
	adw_application_window_set_content(ADW_APPLICATION_WINDOW(window->window), create_content(window));
	bind_window_commands(window);
	show_while_available(window, window->sidebar_button, "toggle-sidebar");
	show_while_available(window, window->plan_button, "toggle-plan");
	show_while_available(window, window->task_button, "run-task");
	apply_window_layout(NULL, window);
	watch_settings(apply_window_layout, window);
	watch_language_servers(on_language_servers_changed, window);
	update_title(window);
	update_window_empty_page(window);
	add_file_drop(window->window, drop_files, window);
	g_signal_connect(window->window, "close-request", G_CALLBACK(on_close_request), window);
	watch_window_frames(window->window);
	update_window_status(window);
	return window;
}


GtkWindow *get_gtk_window(Window *window) {
	return GTK_WINDOW(window->window);
}
