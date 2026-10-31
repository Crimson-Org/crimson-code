#include "common/key-store.h"
#include "state.h"

#define STATE_FILE "window.ini"
#define STATE_GROUP "window"
#define PROJECTS_FOLDER "projects"
#define PROJECT_GROUP "project"
#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 860
#define DEFAULT_SIDEBAR_WIDTH 260
#define DEFAULT_TERMINAL_HEIGHT 260
#define MINIMUM_PANEL_SIZE 120
#define MAXIMUM_PANEL_SIZE 16384


void load_window_state(WindowState *state) {
	g_autofree char *path = get_state_file_path(STATE_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	load_window_geometry(file, &state->geometry, DEFAULT_WIDTH, DEFAULT_HEIGHT);
	state->shows_sidebar = !g_key_file_has_key(file, STATE_GROUP, "sidebar", NULL)
		|| g_key_file_get_boolean(file, STATE_GROUP, "sidebar", NULL);
	state->shows_plan = g_key_file_get_boolean(file, STATE_GROUP, "plan", NULL);
	state->sidebar_width = read_key_integer(file, STATE_GROUP, "sidebar-width", DEFAULT_SIDEBAR_WIDTH,
		MINIMUM_PANEL_SIZE, MAXIMUM_PANEL_SIZE);
	state->terminal_height = read_key_integer(file, STATE_GROUP, "terminal-height", DEFAULT_TERMINAL_HEIGHT,
		MINIMUM_PANEL_SIZE, MAXIMUM_PANEL_SIZE);
	state->project = g_key_file_get_string(file, STATE_GROUP, "project", NULL);
	state->files = g_key_file_get_string_list(file, STATE_GROUP, "files", NULL, NULL);
	state->current_file = g_key_file_get_string(file, STATE_GROUP, "current-file", NULL);
}


void save_window_state(const WindowState *state) {
	g_autofree char *path = get_state_file_path(STATE_FILE);
	g_autoptr(GKeyFile) file = g_key_file_new();
	store_window_geometry(file, &state->geometry);
	g_key_file_set_boolean(file, STATE_GROUP, "sidebar", state->shows_sidebar);
	g_key_file_set_boolean(file, STATE_GROUP, "plan", state->shows_plan);
	g_key_file_set_integer(file, STATE_GROUP, "sidebar-width", state->sidebar_width);
	g_key_file_set_integer(file, STATE_GROUP, "terminal-height", state->terminal_height);
	if (state->project)
		g_key_file_set_string(file, STATE_GROUP, "project", state->project);
	if (state->files)
		g_key_file_set_string_list(file, STATE_GROUP, "files", (const char *const *)state->files,
			g_strv_length(state->files));
	if (state->current_file)
		g_key_file_set_string(file, STATE_GROUP, "current-file", state->current_file);
	save_key_file(file, path);
}


void clear_window_state(WindowState *state) {
	g_clear_pointer(&state->project, g_free);
	g_clear_pointer(&state->files, g_strfreev);
	g_clear_pointer(&state->current_file, g_free);
}


/* Each project gets a file named after its path, which also names the project inside for anyone reading it */
static char *get_project_file_path(const char *project) {
	g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, project, -1);
	g_autofree char *name = g_strconcat(PROJECTS_FOLDER G_DIR_SEPARATOR_S, hash, ".ini", NULL);
	return get_state_file_path(name);
}


void load_project_state(const char *project, ProjectState *state) {
	g_autofree char *path = get_project_file_path(project);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	state->open_folders = g_key_file_get_string_list(file, PROJECT_GROUP, "open-folders", NULL, NULL);
	state->open_plan_rows = g_key_file_get_string_list(file, PROJECT_GROUP, "open-plan-rows", NULL, NULL);
}


static void set_list(GKeyFile *file, const char *key, char **values) {
	if (values)
		g_key_file_set_string_list(file, PROJECT_GROUP, key, (const char *const *)values, g_strv_length(values));
}


void save_project_state(const char *project, const ProjectState *state) {
	g_autofree char *path = get_project_file_path(project);
	g_autoptr(GKeyFile) file = g_key_file_new();
	g_key_file_set_string(file, PROJECT_GROUP, "folder", project);
	set_list(file, "open-folders", state->open_folders);
	set_list(file, "open-plan-rows", state->open_plan_rows);
	save_key_file(file, path);
}


void clear_project_state(ProjectState *state) {
	g_clear_pointer(&state->open_folders, g_strfreev);
	g_clear_pointer(&state->open_plan_rows, g_strfreev);
}
