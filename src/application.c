#include <adwaita.h>
#include <gtksourceview/gtksource.h>

#include "../lang/en.h"
#include "application.h"
#include "common/startup.h"
#include "editor-colours.h"
#include "editor-guides.h"
#include "editor-completion.h"
#include "editor-settings.h"
#include "language-server-page.h"
#include "settings.h"
#include "shortcuts-page.h"
#include "plan-sidebar.h"
#include "sidebar.h"
#include "build/tasks.h"
#include "terminal.h"
#include "themes.h"
#include "window.h"

#define LANGUAGE_RESOURCES "resource:///io/github/CrimsonOrg/CrimsonCode/language-specs"

/* The settings window pages in order, which components fill with their own settings */
static const SettingsPage PAGES[] = {
	{"appearance", TEXT_PAGE_APPEARANCE, "preferences-desktop-appearance-symbolic"},
	{"editor", TEXT_PAGE_EDITOR, "accessories-text-editor-symbolic"},
	{"vim", TEXT_PAGE_VIM, "input-keyboard-symbolic"},
	{"files", TEXT_PAGE_FILES, "folder-symbolic"},
	{"terminal", TEXT_PAGE_TERMINAL, "utilities-terminal-symbolic"},
	{"plan", TEXT_PAGE_PLAN, "checkbox-checked-symbolic"},
	{"language-servers", TEXT_LANGUAGE_SERVERS, "network-server-symbolic"},
	{"shortcuts", TEXT_PAGE_SHORTCUTS, "preferences-desktop-keyboard-shortcuts-symbolic"},
};

/* Paths waiting for the window's first frame, so the window is on screen before anything loads */
typedef struct {
	Window *window;
	GPtrArray *files;
} OpenRequest;


static Window *get_window(GtkApplication *application) {
	GtkWindow *active = gtk_application_get_active_window(application);
	return active ? g_object_get_data(G_OBJECT(active), "window") : create_window(application);
}


static gboolean open_requested_paths(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
	OpenRequest *request = data;
	if (request->files->len == 0)
		restore_window_project(request->window);
	for (guint index = 0; index < request->files->len; index++)
		open_window_path(request->window, g_ptr_array_index(request->files, index));
	set_window_loading(request->window, FALSE);
	g_ptr_array_unref(request->files);
	g_free(request);
	return G_SOURCE_REMOVE;
}


static void present_and_open(GApplication *application, GFile **files, int count) {
	OpenRequest *request = g_new0(OpenRequest, 1);
	gboolean is_new = gtk_application_get_active_window(GTK_APPLICATION(application)) == NULL;
	request->window = get_window(GTK_APPLICATION(application));
	request->files = g_ptr_array_new_with_free_func(g_object_unref);
	for (int index = 0; index < count; index++)
		g_ptr_array_add(request->files, g_object_ref(files[index]));
	GtkWidget *window = GTK_WIDGET(get_gtk_window(request->window));
	gtk_window_present(GTK_WINDOW(window));
	if (!is_new && count == 0) {
		g_ptr_array_unref(request->files);
		g_free(request);
		return;
	}
	set_window_loading(request->window, TRUE);
	gtk_widget_add_tick_callback(window, open_requested_paths, request, NULL);
}


static void activate(GApplication *application) {
	present_and_open(application, NULL, 0);
}


static void open_files(GApplication *application, GFile **files, int count, const char *hint) {
	present_and_open(application, files, count);
}


static void quit(GSimpleAction *action, GVariant *parameter, gpointer application) {
	/* Closing a window removes it from the application's own list, so walk a copy */
	GList *windows = g_list_copy(gtk_application_get_windows(GTK_APPLICATION(application)));
	for (GList *item = windows; item; item = item->next)
		gtk_window_close(item->data);
	g_list_free(windows);
}


/* Each component lists its own settings, commands and settings page content here, as plugins will later */
static void register_components(void) {
	register_settings_pages(PAGES, G_N_ELEMENTS(PAGES));
	register_window_settings();
	register_themes();
	register_editor_settings();
	register_colour_settings();
	register_guide_settings();
	register_completion_settings();
	register_sidebar_settings();
	register_plan_settings();
	register_terminal_settings();
	register_window_commands();
	register_language_server_page();
	register_shortcuts_page();
}


static void start_up(GApplication *application) {
	gtk_source_init();
	/* Extra languages must be added before the first file is opened, when the language list is read */
	gtk_source_language_manager_append_search_path(gtk_source_language_manager_get_default(), LANGUAGE_RESOURCES);
	static const GActionEntry ENTRIES[] = {
		{"quit", quit, NULL, NULL, NULL, {0}},
	};
	g_action_map_add_action_entries(G_ACTION_MAP(application), ENTRIES, G_N_ELEMENTS(ENTRIES), application);
	register_components();
}


/* Writing task scripts needs no window, so build servers and other tools can run it */
static int handle_options(GApplication *application, GVariantDict *options) {
	const char *path = NULL;
	if (!g_variant_dict_lookup(options, "write-tasks", "^&ay", &path))
		return -1;
	g_autoptr(GError) error = NULL;
	g_autofree char *project = find_task_project(path, &error);
	if (!project || !write_task_scripts(project, &error)) {
		g_printerr("%s\n", error->message);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}


static const GOptionEntry OPTIONS[] = {
	{"write-tasks", 0, 0, G_OPTION_ARG_FILENAME, NULL, TEXT_WRITE_TASKS_OPTION, TEXT_PROJECT_ARGUMENT},
	G_OPTION_ENTRY_NULL,
};

const ApplicationSetup APPLICATION_SETUP = {
	.identity = {"io.github.CrimsonOrg.CrimsonCode", TEXT_APPLICATION_NAME, "crimson-code", APPLICATION_VERSION},
	.start_up = start_up,
	.activate = activate,
	.open = open_files,
	.handle_options = handle_options,
	.options = OPTIONS,
	.parameter_summary = TEXT_PATHS_ARGUMENT,
};

