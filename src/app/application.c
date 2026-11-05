#include <adwaita.h>
#include <gtksourceview/gtksource.h>

#include "lang/en.h"
#include "application.h"
#include "common/app/startup.h"
#include "editor/editor-colours.h"
#include "editor/editor-guides.h"
#include "editor/editor-completion.h"
#include "editor/editor-settings.h"
#include "lsp/language-server-page.h"
#include "settings.h"
#include "shortcuts-page.h"
#include "plan/plan-sidebar.h"
#include "window/sidebar.h"
#include "build/tasks.h"
#include "terminal/terminal.h"
#include "themes.h"
#include "window/window.h"

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


/* The active window may be a dialog of the app rather than an editor window, so the editor windows are looked at */
static Window *get_window(GtkApplication *application) {
	for (GList *item = gtk_application_get_windows(application); item; item = item->next) {
		Window *window = g_object_get_data(G_OBJECT(item->data), "window");
		if (window)
			return window;
	}
	return create_window(application);
}


static gboolean open_requested_paths(gpointer data) {
	OpenRequest *request = data;
	if (request->files->len == 0)
		restore_window_project(request->window);
	for (guint index = 0; index < request->files->len; index++)
		open_window_path(request->window, g_ptr_array_index(request->files, index));
	set_window_loading(request->window, FALSE);
	return G_SOURCE_REMOVE;
}


static void free_open_request(gpointer data) {
	OpenRequest *request = data;
	g_ptr_array_unref(request->files);
	g_free(request);
}


static void present_and_open(GApplication *application, GFile **files, int count) {
	OpenRequest *request = g_new0(OpenRequest, 1);
	gboolean is_new = gtk_application_get_windows(GTK_APPLICATION(application)) == NULL;
	request->window = get_window(GTK_APPLICATION(application));
	request->files = g_ptr_array_new_with_free_func(g_object_unref);
	for (int index = 0; index < count; index++)
		g_ptr_array_add(request->files, g_object_ref(files[index]));
	GtkWidget *window = GTK_WIDGET(get_gtk_window(request->window));
	gtk_window_present(GTK_WINDOW(window));
	if (!is_new && count == 0) {
		free_open_request(request);
		return;
	}
	set_window_loading(request->window, TRUE);
	run_after_frame(window, open_requested_paths, request, free_open_request);
}


static void activate(GApplication *application) {
	present_and_open(application, NULL, 0);
}


static void open_files(GApplication *application, GFile **files, int count, const char *hint) {
	present_and_open(application, files, count);
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

