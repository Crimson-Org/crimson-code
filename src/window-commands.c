#include <string.h>

#include "../lang/en.h"
#include "commands.h"
#include "editor-completion.h"
#include "editor-cursors.h"
#include "editor-hover.h"
#include "settings-dialog.h"
#include "settings.h"
#include "task-menu.h"
#include "window-private.h"

#define ACTION_PREFIX "win."

/* One shortcut of a command, kept with the shortcut so its scope can be checked when pressed */
typedef struct {
	const Command *command;
	const char *trigger;
} ShortcutBinding;
#define LANGUAGE_SERVERS_PAGE "language-servers"


/* ==================================== Availability ==================================== */


static gboolean has_project(Window *window) {
	return window->project != NULL;
}


static gboolean has_editor(Window *window) {
	return get_current_editor(window) != NULL;
}


static gboolean has_several_panes(Window *window) {
	return count_window_panes(window) > 1;
}


/* ==================================== Files ==================================== */


static GFile *get_start_folder(Window *window) {
	return window->project ? g_file_new_for_path(window->project) : NULL;
}


/* Names typed after a : command are relative to the project, as they would be in a shell there */
static GFile *resolve_argument(Window *window, const char *argument) {
	if (g_path_is_absolute(argument) || !window->project)
		return g_file_new_for_commandline_arg(argument);
	return g_file_new_for_commandline_arg_and_cwd(argument, window->project);
}


static void on_files_chosen(GObject *chooser, GAsyncResult *result, gpointer data) {
	Window *window = data;
	g_autoptr(GListModel) files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(chooser), result, NULL);
	for (guint index = 0; files && index < g_list_model_get_n_items(files); index++) {
		g_autoptr(GFile) file = g_list_model_get_item(files, index);
		open_window_file(window, file);
	}
}


static void open_file(Window *window, CommandInput *input) {
	if (input->argument) {
		g_autoptr(GFile) file = resolve_argument(window, input->argument);
		open_window_file(window, file);
		return;
	}
	g_autoptr(GtkFileDialog) chooser = gtk_file_dialog_new();
	g_autoptr(GFile) folder = get_start_folder(window);
	gtk_file_dialog_set_initial_folder(chooser, folder);
	gtk_file_dialog_open_multiple(chooser, GTK_WINDOW(window->window), NULL, on_files_chosen, window);
}


static void on_folder_chosen(GObject *chooser, GAsyncResult *result, gpointer data) {
	g_autoptr(GFile) folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(chooser), result, NULL);
	g_autofree char *path = folder ? g_file_get_path(folder) : NULL;
	if (path)
		open_window_project(data, path);
}


static void open_folder(Window *window, CommandInput *input) {
	if (input->argument) {
		g_autoptr(GFile) folder = resolve_argument(window, input->argument);
		open_window_path(window, folder);
		return;
	}
	g_autoptr(GtkFileDialog) chooser = gtk_file_dialog_new();
	g_autoptr(GFile) folder = get_start_folder(window);
	gtk_file_dialog_set_initial_folder(chooser, folder);
	gtk_file_dialog_select_folder(chooser, GTK_WINDOW(window->window), NULL, on_folder_chosen, window);
}


static void new_file(Window *window, CommandInput *input) {
	add_window_editor(window);
}


static void on_save_target_chosen(GObject *chooser, GAsyncResult *result, gpointer data) {
	Editor *editor = data;
	g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(chooser), result, NULL);
	if (file)
		save_editor(editor, file);
}


static void save_file_as(Window *window, CommandInput *input) {
	Editor *editor = get_current_editor(window);
	if (!editor)
		return;
	if (input->argument) {
		g_autoptr(GFile) target = resolve_argument(window, input->argument);
		save_editor(editor, target);
		return;
	}
	g_autoptr(GtkFileDialog) chooser = gtk_file_dialog_new();
	GFile *file = get_editor_file(editor);
	if (file) {
		gtk_file_dialog_set_initial_file(chooser, file);
	} else {
		g_autoptr(GFile) folder = get_start_folder(window);
		gtk_file_dialog_set_initial_folder(chooser, folder);
	}
	gtk_file_dialog_save(chooser, GTK_WINDOW(window->window), NULL, on_save_target_chosen, editor);
}


static void save_file(Window *window, CommandInput *input) {
	Editor *editor = get_current_editor(window);
	if (editor && get_editor_file(editor) && !input->argument)
		save_editor(editor, NULL);
	else if (editor)
		save_file_as(window, input);
}


static void save_all(Window *window, CommandInput *input) {
	g_autoptr(GPtrArray) editors = list_window_editors(window);
	for (guint index = 0; index < editors->len; index++) {
		Editor *editor = g_ptr_array_index(editors, index);
		if (get_editor_file(editor) && is_editor_modified(editor))
			save_editor(editor, NULL);
	}
}


static void close_file(Window *window, CommandInput *input) {
	Editor *editor = get_current_editor(window);
	if (editor)
		close_window_editor(window, editor, input->is_forced);
}


static void save_and_close(Window *window, CommandInput *input) {
	Editor *editor = get_current_editor(window);
	if (!editor)
		return;
	if (save_editor(editor, NULL))
		close_window_editor(window, editor, TRUE);
}


static void close_window(Window *window, CommandInput *input) {
	gtk_window_close(GTK_WINDOW(window->window));
}


/* ==================================== Panels and editing ==================================== */


static void toggle_sidebar(Window *window, CommandInput *input) {
	if (window->project)
		toggle_window_sidebar_page(window, FALSE);
}


static void toggle_plan(Window *window, CommandInput *input) {
	if (window->project)
		toggle_window_sidebar_page(window, TRUE);
}


static void reopen_closed_file(Window *window, CommandInput *input) {
	reopen_window_file(window);
}


static gboolean has_closed_file(Window *window) {
	return window->closed_files->len > 0;
}


static gboolean has_language_server(Window *window) {
	Editor *editor = get_current_editor(window);
	return editor && get_editor_language_server(editor);
}


static void stop_language_server(Window *window, CommandInput *input) {
	stop_window_language_server(window);
}


static void restart_language_servers(Window *window, CommandInput *input) {
	restart_window_language_servers(window);
}


static void toggle_problems(Window *window, CommandInput *input) {
	set_window_problems_visible(window, !is_window_problems_visible(window));
}


static void close_project(Window *window, CommandInput *input) {
	close_window_project(window);
}


static void new_terminal(Window *window, CommandInput *input) {
	open_window_terminal(window);
}


static void hide_panel(Window *window, CommandInput *input) {
	hide_window_panel(window);
}


static void toggle_terminal(Window *window, CommandInput *input) {
	set_window_terminal_visible(window, !is_window_terminal_visible(window));
}


static void run_task(Window *window, CommandInput *input) {
	if (input->argument)
		run_window_task(window, input->argument);
	else
		show_task_menu(window->task_button);
}


static void edit_tasks(Window *window, CommandInput *input) {
	if (window->project)
		choose_window_task(NULL, window);
}


static void find(Window *window, CommandInput *input) {
	show_find_bar(window->find_bar);
}


static void replace(Window *window, CommandInput *input) {
	show_replace_bar(window->find_bar);
}


static void toggle_comment(Window *window, CommandInput *input) {
	Editor *editor = get_current_editor(window);
	if (editor)
		toggle_editor_comment(editor);
}


static void go_to_next_problem(Window *window, CommandInput *input) {
	Editor *editor = get_current_editor(window);
	if (editor && !move_editor_to_problem(editor, TRUE))
		input->message = g_strdup(TEXT_NO_PROBLEMS);
}


static void go_to_previous_problem(Window *window, CommandInput *input) {
	Editor *editor = get_current_editor(window);
	if (editor && !move_editor_to_problem(editor, FALSE))
		input->message = g_strdup(TEXT_NO_PROBLEMS);
}


static gboolean has_json_editor(Window *window) {
	Editor *editor = get_current_editor(window);
	return editor && is_editor_json(editor);
}


static gboolean is_visual_editing(Window *window) {
	Editor *editor = get_current_editor(window);
	return editor && is_editor_visual(editor);
}


static void toggle_visual_editing(Window *window, CommandInput *input) {
	Editor *editor = get_current_editor(window);
	if (editor)
		set_editor_visual(editor, !is_editor_visual(editor));
	update_mode_switch(window);
}


static void select_next(Window *window, CommandInput *input) {
	Editor *editor = get_current_editor(window);
	if (editor)
		select_next_match(GTK_TEXT_VIEW(get_editor_view(editor)));
}


static void select_all(Window *window, CommandInput *input) {
	Editor *editor = get_current_editor(window);
	if (editor)
		select_all_matches(GTK_TEXT_VIEW(get_editor_view(editor)));
}


static void show_suggestions(Window *window, CommandInput *input) {
	Editor *editor = get_current_editor(window);
	if (editor)
		show_editor_suggestions(editor);
}


static void show_details(Window *window, CommandInput *input) {
	Editor *editor = get_current_editor(window);
	if (editor)
		show_editor_cursor_hover(editor);
}


static void select_next_tab(Window *window, CommandInput *input) {
	select_next_window_tab(window, TRUE);
}


static void select_previous_tab(Window *window, CommandInput *input) {
	select_next_window_tab(window, FALSE);
}


/* ==================================== Panes ==================================== */


static void split_right(Window *window, CommandInput *input) {
	split_window_pane(window, GTK_ORIENTATION_HORIZONTAL);
	if (input->argument)
		open_file(window, input);
}


static void split_down(Window *window, CommandInput *input) {
	split_window_pane(window, GTK_ORIENTATION_VERTICAL);
	if (input->argument)
		open_file(window, input);
}


static void close_pane(Window *window, CommandInput *input) {
	close_window_pane(window);
}


static void keep_only_pane(Window *window, CommandInput *input) {
	close_other_window_panes(window);
}


static void move_left(Window *window, CommandInput *input) {
	move_to_window_pane(window, PANE_LEFT);
}


static void move_right(Window *window, CommandInput *input) {
	move_to_window_pane(window, PANE_RIGHT);
}


static void move_up(Window *window, CommandInput *input) {
	move_to_window_pane(window, PANE_UP);
}


static void move_down(Window *window, CommandInput *input) {
	move_to_window_pane(window, PANE_DOWN);
}


/* Runs a Vim window command by its letter, as typed after :wincmd or Ctrl+W */
static void run_window_command(Window *window, CommandInput *input) {
	static const struct {
		char letter;
		CommandFunction run;
	} LETTERS[] = {
		{'h', move_left}, {'l', move_right}, {'k', move_up}, {'j', move_down}, {'w', NULL}, {'v', split_right},
		{'s', split_down}, {'c', close_pane}, {'q', close_pane}, {'o', keep_only_pane},
	};
	char letter = input->argument ? input->argument[0] : 'w';
	for (guint index = 0; index < G_N_ELEMENTS(LETTERS); index++) {
		if (LETTERS[index].letter != letter)
			continue;
		CommandInput plain = {NULL, FALSE, NULL};
		if (LETTERS[index].run)
			LETTERS[index].run(window, &plain);
		else
			move_to_next_window_pane(window);
		return;
	}
}


static void change_text_size(int step) {
	g_autofree char *size = step ? g_strdup_printf("%d", get_number_setting("text-size") + step)
		: g_strdup(find_setting("text-size")->default_value);
	change_setting("text-size", size);
}


static void zoom_in(Window *window, CommandInput *input) {
	change_text_size(1);
}


static void zoom_out(Window *window, CommandInput *input) {
	change_text_size(-1);
}


static void zoom_reset(Window *window, CommandInput *input) {
	change_text_size(0);
}


static void set_option(Window *window, CommandInput *input) {
	run_set_command(input->argument, &input->message);
}


static void show_settings(Window *window, CommandInput *input) {
	show_settings_dialog(window->window, input->argument);
}


static void show_language_servers(Window *window, CommandInput *input) {
	show_settings_dialog(window->window, LANGUAGE_SERVERS_PAGE);
}


static void show_about(Window *window, CommandInput *input) {
	AdwDialog *dialog = adw_about_dialog_new();
	adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(dialog), TEXT_APPLICATION_NAME);
	adw_about_dialog_set_version(ADW_ABOUT_DIALOG(dialog), APPLICATION_VERSION);
	adw_dialog_present(dialog, window->window);
}


/* ==================================== Registration ==================================== */


/* The everyday commands sit at the top, the rest in submenus, and their shortcuts work wherever they are */
static const MenuSection MENU_SECTIONS[] = {
	{"open", NULL}, {"find", NULL}, {"file", TEXT_MENU_FILE}, {"edit", TEXT_MENU_EDIT}, {"view", TEXT_MENU_VIEW},
	{"tools", TEXT_MENU_TOOLS}, {"program", NULL},
};

static const Command COMMANDS[] = {
	{"open-folder", TEXT_OPEN_FOLDER, "open", {"<Control><Shift>o"}, COMMAND_EVERYWHERE, {"cd"}, open_folder, NULL,
		NULL, NULL},
	{"open-file", TEXT_OPEN_FILE, "open", {"<Control>o"}, COMMAND_EASY_EDITING, {"e", "edit"}, open_file, NULL, NULL,
		NULL},
	{"new-file", TEXT_NEW_FILE, "open", {"<Control>n"}, COMMAND_EASY_EDITING, {"enew"}, new_file, NULL, NULL, NULL},
	{"close-project", TEXT_CLOSE_PROJECT, "file", {NULL}, COMMAND_EVERYWHERE, {NULL}, close_project, NULL,
		has_project, NULL},
	{"save", TEXT_SAVE, "open", {"<Control>s"}, COMMAND_OUTSIDE_TERMINAL, {"w", "write"}, save_file, NULL,
		has_editor, NULL},
	{"save-as", TEXT_SAVE_AS, "file", {"<Control><Shift>s"}, COMMAND_EVERYWHERE, {"sav", "saveas"}, save_file_as,
		NULL, has_editor, NULL},
	{"save-all", TEXT_SAVE_ALL, "file", {NULL}, COMMAND_EVERYWHERE, {"wa", "wall"}, save_all, NULL, has_editor, NULL},
	{"reopen-closed-file", TEXT_REOPEN_CLOSED_FILE, "file", {"<Control><Shift>t"}, COMMAND_EVERYWHERE, {NULL},
		reopen_closed_file, NULL, has_closed_file, NULL},
	{"close-file", TEXT_CLOSE_FILE, "file", {"<Control>w"}, COMMAND_EASY_EDITING, {"q", "quit"}, close_file, NULL,
		has_editor, NULL},
	{"save-and-close", TEXT_SAVE_AND_CLOSE, NULL, {NULL}, COMMAND_EVERYWHERE, {"wq", "x", "xit", "exit"},
		save_and_close, NULL, has_editor, NULL},
	{"replace", TEXT_REPLACE, "edit", {"<Control>h"}, COMMAND_EASY_EDITING, {NULL}, replace, NULL, has_editor, NULL},
	{"find", TEXT_FIND, "find", {"<Control>f"}, COMMAND_EASY_EDITING, {NULL}, find, NULL, has_editor, NULL},
	{"next-problem", TEXT_NEXT_PROBLEM, "edit", {"F8"}, COMMAND_EVERYWHERE, {"lne", "lnext", "cn", "cnext"},
		go_to_next_problem, NULL, has_editor, NULL},
	{"previous-problem", TEXT_PREVIOUS_PROBLEM, "edit", {"<Shift>F8"}, COMMAND_EVERYWHERE,
		{"lp", "lprevious", "cp", "cprevious"}, go_to_previous_problem, NULL, has_editor, NULL},
	{"select-next-match", TEXT_SELECT_NEXT_MATCH, "edit", {"<Control>d"}, COMMAND_EASY_EDITING, {NULL}, select_next,
		NULL, has_editor, NULL},
	{"select-all-matches", TEXT_SELECT_ALL_MATCHES, "edit", {"<Control><Shift>l"}, COMMAND_OUTSIDE_TERMINAL, {NULL},
		select_all, NULL, has_editor, NULL},
	{"show-suggestions", TEXT_SHOW_SUGGESTIONS, "edit", {"<Control>space"}, COMMAND_OUTSIDE_TERMINAL, {NULL},
		show_suggestions, NULL, has_editor, NULL},
	{"show-details", TEXT_SHOW_DETAILS, "edit", {"<Control>k"}, COMMAND_OUTSIDE_TERMINAL, {"hover"}, show_details,
		NULL, has_editor, NULL},
	{"toggle-comment", TEXT_TOGGLE_COMMENT, "edit", {"<Control><Shift>c", "<Control>slash"}, COMMAND_OUTSIDE_TERMINAL,
		{NULL}, toggle_comment, NULL, has_editor, NULL},
	{"toggle-sidebar", TEXT_PROJECT_FILES, "view", {"<Control>b", "<Control><Shift>e"}, COMMAND_EASY_EDITING,
		{"Lex", "explore"}, toggle_sidebar, NULL, has_project, is_window_files_visible},
	{"toggle-plan", TEXT_PLAN, "view", {"<Control><Shift>y"}, COMMAND_EVERYWHERE, {"plan", "todo"}, toggle_plan,
		NULL, has_project, is_window_plan_visible},
	{"toggle-visual-editing", TEXT_VISUAL_EDITING, "view", {"<Control><Shift>j"}, COMMAND_EVERYWHERE,
		{"VisualEdit"}, toggle_visual_editing, NULL, has_json_editor, is_visual_editing},
	{"toggle-terminal", TEXT_TERMINAL, "view", {"F12", "<Control>grave"}, COMMAND_EVERYWHERE, {"term", "terminal"},
		toggle_terminal, NULL, NULL, is_window_terminal_visible},
	{"new-terminal", TEXT_NEW_TERMINAL, NULL, {"<Control>F12", "<Control><Shift>grave"}, COMMAND_EVERYWHERE, {NULL},
		new_terminal, NULL, NULL, NULL},
	{"toggle-problems", TEXT_PROBLEMS, "view", {"<Control><Shift>m"}, COMMAND_EVERYWHERE,
		{"cope", "copen", "lop", "lopen"}, toggle_problems, NULL, NULL, is_window_problems_visible},
	{"hide-panel", TEXT_HIDE_PANEL, NULL, {NULL}, COMMAND_EVERYWHERE, {"ccl", "cclose", "lcl", "lclose"}, hide_panel,
		NULL, NULL, NULL},
	{"split-right", TEXT_SPLIT_RIGHT, "view", {"<Control><Shift>r", "<Control>backslash"}, COMMAND_OUTSIDE_TERMINAL,
		{"vs", "vsplit"}, split_right, NULL, has_editor, NULL},
	{"split-down", TEXT_SPLIT_DOWN, "view", {"<Control><Shift>d", "<Control><Shift>backslash"}, COMMAND_EVERYWHERE,
		{"sp", "split"}, split_down, NULL, has_editor, NULL},
	{"close-pane", TEXT_CLOSE_PANE, "view", {"<Control><Alt>w"}, COMMAND_EVERYWHERE, {"clo", "close"}, close_pane,
		NULL, has_several_panes, NULL},
	{"keep-only-pane", TEXT_ONLY_PANE, "view", {NULL}, COMMAND_EVERYWHERE, {"on", "only"}, keep_only_pane, NULL,
		has_several_panes, NULL},
	{"pane-left", TEXT_PANE_LEFT, NULL, {"<Control><Alt>Left"}, COMMAND_EVERYWHERE, {NULL}, move_left, NULL,
		has_several_panes, NULL},
	{"pane-right", TEXT_PANE_RIGHT, NULL, {"<Control><Alt>Right"}, COMMAND_EVERYWHERE, {NULL}, move_right, NULL,
		has_several_panes, NULL},
	{"pane-up", TEXT_PANE_UP, NULL, {"<Control><Alt>Up"}, COMMAND_EVERYWHERE, {NULL}, move_up, NULL,
		has_several_panes, NULL},
	{"pane-down", TEXT_PANE_DOWN, NULL, {"<Control><Alt>Down"}, COMMAND_EVERYWHERE, {NULL}, move_down, NULL,
		has_several_panes, NULL},
	{"window-command", TEXT_WINDOW_COMMAND, NULL, {NULL}, COMMAND_EVERYWHERE, {"winc", "wincmd"},
		run_window_command, NULL, NULL, NULL},
	{"show-minimap", TEXT_MINIMAP, "view", {NULL}, COMMAND_EVERYWHERE, {NULL}, NULL, "show-minimap", NULL, NULL},
	{"next-file", TEXT_NEXT_FILE, NULL, {"<Control>Page_Down", "<Control>Tab"}, COMMAND_EVERYWHERE,
		{"tabn", "tabnext", "bn", "bnext"}, select_next_tab, NULL, has_editor, NULL},
	{"previous-file", TEXT_PREVIOUS_FILE, NULL, {"<Control>Page_Up", "<Control><Shift>Tab"}, COMMAND_EVERYWHERE,
		{"tabp", "tabprevious", "bp", "bprevious"}, select_previous_tab, NULL, has_editor, NULL},
	{"zoom-in", TEXT_ZOOM_IN, NULL, {"<Control>plus", "<Control>equal"}, COMMAND_OUTSIDE_TERMINAL, {NULL}, zoom_in,
		NULL, NULL, NULL},
	{"zoom-out", TEXT_ZOOM_OUT, NULL, {"<Control>minus"}, COMMAND_OUTSIDE_TERMINAL, {NULL}, zoom_out, NULL, NULL, NULL},
	{"zoom-reset", TEXT_ZOOM_RESET, NULL, {"<Control>0"}, COMMAND_OUTSIDE_TERMINAL, {NULL}, zoom_reset, NULL, NULL, NULL},
	{"vim-mode", TEXT_VIM_MODE, "view", {"<Control><Alt>v"}, COMMAND_EVERYWHERE, {NULL}, NULL, "vim-mode", NULL, NULL},
	{"run-task", TEXT_RUN_TASK, "tools", {"<Control><Shift>b"}, COMMAND_EVERYWHERE, {"task", "make"}, run_task,
		NULL, has_project, NULL},
	{"edit-tasks", TEXT_EDIT_TASKS, "tools", {NULL}, COMMAND_EVERYWHERE, {NULL}, edit_tasks, NULL, has_project, NULL},
	{"language-servers", TEXT_LANGUAGE_SERVERS, "tools", {NULL}, COMMAND_EVERYWHERE, {"lsp"},
		show_language_servers, NULL, NULL, NULL},
	{"restart-language-servers", TEXT_RESTART_SERVERS, NULL, {NULL}, COMMAND_EVERYWHERE, {"LspRestart"},
		restart_language_servers, NULL, NULL, NULL},
	{"stop-language-server", TEXT_STOP_SERVER, NULL, {NULL}, COMMAND_EVERYWHERE, {"LspStop"}, stop_language_server,
		NULL, has_language_server, NULL},
	{"set", TEXT_SET_OPTION, NULL, {NULL}, COMMAND_EVERYWHERE, {"se", "set"}, set_option, NULL, NULL, NULL},
	{"settings", TEXT_SETTINGS, "program", {"<Control>comma"}, COMMAND_EVERYWHERE, {"settings", "options"},
		show_settings, NULL, NULL, NULL},
	{"about", TEXT_ABOUT, "program", {NULL}, COMMAND_EVERYWHERE, {NULL}, show_about, NULL, NULL, NULL},
	{"quit", TEXT_QUIT, "program", {"<Control>q"}, COMMAND_EVERYWHERE, {"qa", "qall", "quitall"}, close_window,
		NULL, NULL, NULL},
};


void register_window_commands(void) {
	register_menu_sections(MENU_SECTIONS, G_N_ELEMENTS(MENU_SECTIONS));
	register_commands(COMMANDS, G_N_ELEMENTS(COMMANDS));
}


/* ==================================== Binding to a window ==================================== */


/* Quit and close take the window down while the command runs, so it is kept alive until the command returns */
static void run_command(Window *window, const Command *command, CommandInput *input) {
	GtkWidget *keep = g_object_ref(window->window);
	if (command->run) {
		command->run(window, input);
	} else if (command->setting) {
		change_setting(command->setting, get_switch_setting(command->setting) ? "false" : "true");
	}
	if (input->message && !window->is_closed)
		show_window_message(window, input->message);
	refresh_window_commands(window);
	g_object_unref(keep);
}


static void on_action_activated(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = g_object_get_data(G_OBJECT(action), "window");
	CommandInput input = {NULL, FALSE, NULL};
	run_command(window, data, &input);
	g_free(input.message);
}


/* Switch commands keep their check marks in step with the setting, however it was changed */
static void sync_switch_actions(const char *key, gpointer data) {
	Window *window = data;
	GPtrArray *commands = list_commands();
	for (guint index = 0; index < commands->len; index++) {
		const Command *command = g_ptr_array_index(commands, index);
		if (g_strcmp0(command->setting, key) != 0)
			continue;
		GAction *action = g_action_map_lookup_action(G_ACTION_MAP(window->window), command->id);
		g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(get_switch_setting(key)));
	}
}


/* Ctrl with Shift is never used by shells or Vim, so those shortcuts work wherever the focus is */
static CommandScope get_trigger_scope(const Command *command, const char *trigger) {
	gboolean has_shift = strstr(trigger, "<Shift>") != NULL;
	gboolean has_control = strstr(trigger, "<Control>") != NULL;
	return has_shift && has_control ? COMMAND_EVERYWHERE : command->scope;
}


static gboolean is_focus_inside(Window *window, GtkWidget *container) {
	GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(window->window));
	return focus && (focus == container || gtk_widget_is_ancestor(focus, container));
}


/* Returning FALSE lets the key continue to the focused widget, which is how the terminal and Vim keep keys */
static gboolean run_shortcut(GtkWidget *widget, GVariant *arguments, gpointer data) {
	Window *window = g_object_get_data(G_OBJECT(widget), "window");
	ShortcutBinding *binding = data;
	const Command *command = binding->command;
	CommandScope scope = get_trigger_scope(command, binding->trigger);
	if (scope != COMMAND_EVERYWHERE && is_focus_inside(window, window->terminals))
		return FALSE;
	GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(window->window));
	if (scope == COMMAND_EASY_EDITING && get_switch_setting("vim-mode") && GTK_SOURCE_IS_VIEW(focus)
		&& !get_switch_setting("vim-gives-control-keys"))
		return FALSE;
	if (command->is_available && !command->is_available(window))
		return FALSE;
	CommandInput input = {NULL, FALSE, NULL};
	run_command(window, command, &input);
	g_free(input.message);
	return TRUE;
}


static void add_shortcuts(Window *window) {
	GtkEventController *controller = gtk_shortcut_controller_new();
	gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
	gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(controller), GTK_SHORTCUT_SCOPE_GLOBAL);
	GPtrArray *commands = list_commands();
	for (guint index = 0; index < commands->len; index++) {
		const Command *command = g_ptr_array_index(commands, index);
		for (int number = 0; number < 3 && command->shortcuts[number]; number++) {
			GtkShortcutTrigger *trigger = gtk_shortcut_trigger_parse_string(command->shortcuts[number]);
			if (!trigger) {
				g_warning("Shortcut %s of %s is not valid", command->shortcuts[number], command->id);
				continue;
			}
			ShortcutBinding *binding = g_new0(ShortcutBinding, 1);
			binding->command = command;
			binding->trigger = command->shortcuts[number];
			GtkShortcutAction *action = gtk_callback_action_new(run_shortcut, binding, g_free);
			gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(controller), gtk_shortcut_new(trigger,
				action));
		}
	}
	gtk_widget_add_controller(window->window, controller);
}


void bind_window_commands(Window *window) {
	GPtrArray *commands = list_commands();
	for (guint index = 0; index < commands->len; index++) {
		const Command *command = g_ptr_array_index(commands, index);
		gboolean is_on = command->setting && get_switch_setting(command->setting);
		GSimpleAction *action = command->setting || command->is_checked ? g_simple_action_new_stateful(command->id,
			NULL, g_variant_new_boolean(is_on)) : g_simple_action_new(command->id, NULL);
		g_object_set_data(G_OBJECT(action), "window", window);
		g_signal_connect(action, "activate", G_CALLBACK(on_action_activated), (gpointer)command);
		g_action_map_add_action(G_ACTION_MAP(window->window), G_ACTION(action));
		g_object_unref(action);
	}
	watch_settings(sync_switch_actions, window);
	add_shortcuts(window);
}


/* Commands that cannot run now are turned off, which hides them in the menu and hides buttons bound to them,
 * and commands showing a panel get their check marks */
void refresh_window_commands(Window *window) {
	/* Quitting frees the editors before the command that asked for it returns here */
	if (window->is_closed)
		return;
	GPtrArray *commands = list_commands();
	for (guint index = 0; index < commands->len; index++) {
		const Command *command = g_ptr_array_index(commands, index);
		GAction *action = g_action_map_lookup_action(G_ACTION_MAP(window->window), command->id);
		if (command->is_available)
			g_simple_action_set_enabled(G_SIMPLE_ACTION(action), command->is_available(window));
		if (command->is_checked)
			g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(command->is_checked(window)));
	}
}


void unbind_window_commands(Window *window) {
	unwatch_settings(window);
}


/* A typed : command waiting to run once Vim has finished handling the key that sent it */
typedef struct {
	GtkWidget *window;
	char *text;
} PendingVimCommand;


static void run_vim_command_now(Window *window, const char *text);


static gboolean run_pending_vim_command(gpointer data) {
	PendingVimCommand *pending = data;
	Window *window = g_object_get_data(G_OBJECT(pending->window), "window");
	if (window && window->panes)
		run_vim_command_now(window, pending->text);
	g_object_unref(pending->window);
	g_free(pending->text);
	g_free(pending);
	return G_SOURCE_REMOVE;
}


static const Command *find_typed_command(const char *text, char **name, char **argument) {
	g_autofree char *line = g_strstrip(g_strdup(text + (*text == ':')));
	char *space = strpbrk(line, " \t");
	if (space)
		*space = '\0';
	const char *rest = space ? g_strstrip(space + 1) : NULL;
	*name = g_strdup(line);
	*argument = rest && *rest ? g_strdup(rest) : NULL;
	return *line ? find_vim_command(line) : NULL;
}


/* Reads ":name[!] argument", so typing a registered name runs the same command as its menu entry or shortcut.
 * The command runs after Vim returns, since one such as :close frees the editor whose Vim sent it */
gboolean run_window_vim_command(Window *window, const char *text) {
	g_autofree char *name = NULL, *argument = NULL;
	if (!find_typed_command(text, &name, &argument))
		return FALSE;
	PendingVimCommand *pending = g_new0(PendingVimCommand, 1);
	pending->window = g_object_ref(window->window);
	pending->text = g_strdup(text);
	g_idle_add(run_pending_vim_command, pending);
	return TRUE;
}


static void run_vim_command_now(Window *window, const char *text) {
	g_autofree char *line = NULL, *argument = NULL;
	const Command *command = find_typed_command(text, &line, &argument);
	if (!command)
		return;
	if (command->is_available && !command->is_available(window)) {
		show_window_message(window, TEXT_COMMAND_UNAVAILABLE);
		return;
	}
	CommandInput input = {argument, g_str_has_suffix(line, "!"), NULL};
	run_command(window, command, &input);
	g_free(input.message);
}


GMenuModel *create_window_menu(void) {
	GMenu *menu = g_menu_new();
	GPtrArray *sections = list_menu_sections(), *commands = list_commands();
	GMenu *submenus = NULL;
	for (guint section_index = 0; section_index < sections->len; section_index++) {
		const MenuSection *definition = g_ptr_array_index(sections, section_index);
		const char *section_id = definition->id;
		g_autoptr(GMenu) section = g_menu_new();
		for (guint index = 0; index < commands->len; index++) {
			const Command *command = g_ptr_array_index(commands, index);
			if (g_strcmp0(command->menu_section, section_id) != 0)
				continue;
			g_autofree char *action = g_strconcat(ACTION_PREFIX, command->id, NULL);
			g_autoptr(GMenuItem) item = g_menu_item_new(command->label, action);
			/* Shortcuts live in the window's own controller, so the menu is told which one to show */
			if (command->shortcuts[0])
				g_menu_item_set_attribute(item, "accel", "s", command->shortcuts[0]);
			g_menu_item_set_attribute(item, "hidden-when", "s", "action-disabled");
			g_menu_append_item(section, item);
		}
		if (g_menu_model_get_n_items(G_MENU_MODEL(section)) == 0)
			continue;
		if (!definition->label) {
			g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
			submenus = NULL;
			continue;
		}
		if (!submenus) {
			submenus = g_menu_new();
			g_menu_append_section(menu, NULL, G_MENU_MODEL(submenus));
			g_object_unref(submenus);
		}
		g_menu_append_submenu(submenus, definition->label, G_MENU_MODEL(section));
	}
	return G_MENU_MODEL(menu);
}
