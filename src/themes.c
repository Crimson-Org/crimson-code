#include <string.h>

#include <adwaita.h>
#include <gtksourceview/gtksource.h>

#include "../lang/en.h"
#include "common/themes.h"
#include "settings-dialog.h"
#include "settings.h"
#include "terminal.h"
#include "themes.h"
#include "window.h"

#define TEMPLATE_RESOURCE BUNDLED_THEMES_RESOURCE "/" DEFAULT_DARK_THEME ".xml"
#define TEMPLATE_ID DEFAULT_DARK_THEME
#define TEMPLATE_NAME "Crimson Monokai"
#define PAGE_ID "appearance"
#define MAXIMUM_THEME_NUMBER 1000

static GPtrArray *list_schemes(void);

static const SettingDefinition THEME_SETTINGS[] = {
	{"theme-window", PAGE_ID, TEXT_GROUP_COLOURS, TEXT_SETTING_THEME_WINDOW, TEXT_SETTING_THEME_WINDOW_DETAIL,
		SETTING_SWITCH, "true", 0, 0, NULL, NULL, {NULL}},
	{"light-scheme", PAGE_ID, TEXT_GROUP_COLOURS, TEXT_SETTING_LIGHT_SCHEME, NULL, SETTING_CHOICE,
		DEFAULT_LIGHT_THEME, 0, 0, NULL, list_schemes, {NULL}},
	{"dark-scheme", PAGE_ID, TEXT_GROUP_COLOURS, TEXT_SETTING_DARK_SCHEME, NULL, SETTING_CHOICE, DEFAULT_DARK_THEME,
		0, 0, NULL, list_schemes, {NULL}},
};

/* Explains the format at the top of each new theme, since editing the file is how a theme is made */
static const char TEMPLATE_HELP[] =
	"<!--\n"
	"  Saving this file updates the theme in Crimson Code at once.\n"
	"  Colours are #rrggbb values or names from the <color> lines below.\n"
	"  A style can set foreground, background, bold, italic, underline and strikethrough.\n"
	"  Styles named text, selection, cursor, current-line, line-numbers, current-line-number,\n"
	"  bracket-match, search-match, right-margin and draw-spaces colour the editor itself.\n"
	"  Styles named crimson:window, crimson:titlebar, crimson:sidebar and crimson:statusbar colour the window around it,\n"
	"  and crimson:accent colours buttons and selections.\n"
	"  Styles starting with def: colour code in all languages, such as def:comment or def:keyword,\n"
	"  and a language can be coloured on its own with its id, such as c:preprocessor.\n"
	"  The full list is at https://gnome.pages.gitlab.gnome.org/gtksourceview/gtksourceview5/style-reference.html\n"
	"-->\n";

static GtkCssProvider *scheme_style;


static GPtrArray *list_schemes(void) {
	GPtrArray *themes = list_themes();
	GPtrArray *choices = g_ptr_array_new_with_free_func(g_free);
	for (guint index = 0; index < themes->len; index++) {
		const Theme *theme = g_ptr_array_index(themes, index);
		SettingChoice *choice = g_new0(SettingChoice, 1);
		choice->id = theme->id;
		choice->label = theme->name;
		g_ptr_array_add(choices, choice);
	}
	return choices;
}


/* ==================================== New themes ==================================== */


static char *make_theme_text(const char *id, const char *name) {
	g_autoptr(GBytes) template = g_resources_lookup_data(TEMPLATE_RESOURCE, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
	g_autofree char *text = g_strndup(g_bytes_get_data(template, NULL), g_bytes_get_size(template));
	g_autofree char *identity = g_strdup_printf("id=\"%s\" name=\"%s\"", id, name);
	g_autoptr(GString) theme = g_string_new(text);
	g_string_replace(theme, "id=\"" TEMPLATE_ID "\" name=\"" TEMPLATE_NAME "\"", identity, 1);
	char *declaration_end = strstr(theme->str, "?>\n");
	if (declaration_end)
		g_string_insert(theme, declaration_end + 3 - theme->str, TEMPLATE_HELP);
	return g_string_free(g_steal_pointer(&theme), FALSE);
}


/* Picks the first free "My Theme" name, as a theme with a taken id would hide the other one */
static GFile *choose_theme_file(const char *folder, char **id, char **name) {
	for (int number = 1; number < MAXIMUM_THEME_NUMBER; number++) {
		g_autofree char *candidate = number == 1 ? g_strdup("my-theme") : g_strdup_printf("my-theme-%d", number);
		g_autofree char *file_name = g_strconcat(candidate, ".xml", NULL);
		g_autofree char *path = g_build_filename(folder, file_name, NULL);
		if (g_file_test(path, G_FILE_TEST_EXISTS) || find_theme(candidate))
			continue;
		*id = g_steal_pointer(&candidate);
		*name = number == 1 ? g_strdup(TEXT_MY_THEME) : g_strdup_printf("%s %d", TEXT_MY_THEME, number);
		return g_file_new_for_path(path);
	}
	return NULL;
}


static void create_theme(AdwButtonRow *row, AdwPreferencesDialog *dialog) {
	g_autofree char *folder = get_user_theme_folder();
	g_autofree char *id = NULL, *name = NULL;
	g_autoptr(GFile) file = g_mkdir_with_parents(folder, 0755) == 0 ? choose_theme_file(folder, &id, &name) : NULL;
	g_autofree char *text = file ? make_theme_text(id, name) : NULL;
	g_autoptr(GError) error = NULL;
	g_autofree char *path = file ? g_file_get_path(file) : NULL;
	if (!file || !g_file_set_contents(path, text, -1, &error)) {
		g_warning("A new theme could not be written to %s", folder);
		return;
	}
	reload_themes();
	change_setting(adw_style_manager_get_dark(adw_style_manager_get_default()) ? "dark-scheme" : "light-scheme", id);
	GtkWindow *active = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
	Window *window = active ? g_object_get_data(G_OBJECT(active), "window") : NULL;
	adw_dialog_close(ADW_DIALOG(dialog));
	if (window)
		open_window_path(window, file);
}


static void open_theme_folder(AdwButtonRow *row, AdwPreferencesDialog *dialog) {
	g_autofree char *path = get_user_theme_folder();
	g_mkdir_with_parents(path, 0755);
	g_autoptr(GFile) folder = g_file_new_for_path(path);
	g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new(folder);
	gtk_file_launcher_launch(launcher, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(dialog))), NULL, NULL, NULL);
	reload_themes();
}


static GtkWidget *create_button_row(const char *title, GCallback callback, AdwPreferencesDialog *dialog) {
	GtkWidget *row = adw_button_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	g_signal_connect(row, "activated", callback, dialog);
	return row;
}


static void fill_page(AdwPreferencesDialog *dialog, AdwPreferencesPage *page) {
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), TEXT_GROUP_THEMES);
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), TEXT_GROUP_THEMES_DETAIL);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), create_button_row(TEXT_NEW_THEME,
		G_CALLBACK(create_theme), dialog));
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), create_button_row(TEXT_OPEN_THEME_FOLDER,
		G_CALLBACK(open_theme_folder), dialog));
	adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(group));
}


/* Code outside the editor takes the theme's text colours, and the editor reads the same theme files again when
 * they change on disk */
static void on_themes_changed(gboolean has_read_files, gpointer data) {
	if (has_read_files)
		gtk_source_style_scheme_manager_force_rescan(gtk_source_style_scheme_manager_get_default());
	GdkRGBA background, foreground;
	gboolean has_text = get_theme_text_colours(&background, &foreground);
	set_terminal_background(has_text ? &background : NULL);
	g_autoptr(GString) css = g_string_new(NULL);
	if (has_text) {
		g_autofree char *background_text = gdk_rgba_to_string(&background);
		g_autofree char *foreground_text = gdk_rgba_to_string(&foreground);
		g_string_append_printf(css, ".hover-code, .problems-panel, terminal { background-color: %s; color: %s; }\n",
			background_text, foreground_text);
	}
	if (!scheme_style) {
		scheme_style = gtk_css_provider_new();
		gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(scheme_style),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	}
	gtk_css_provider_load_from_string(scheme_style, css->str);
}


/* With window theming on, the theme colours the window around the editor too */
static void apply_scheme_style(void) {
	use_themes(get_text_setting("light-scheme"), get_text_setting("dark-scheme"),
		get_switch_setting("theme-window") ? THEME_WINDOW | THEME_TITLE_BAR : 0);
}


static void on_scheme_setting_changed(const char *key, gpointer data) {
	if (!key || g_strcmp0(key, "light-scheme") == 0 || g_strcmp0(key, "dark-scheme") == 0
		|| g_strcmp0(key, "theme-window") == 0)
		apply_scheme_style();
}


void register_themes(void) {
	gtk_source_style_scheme_manager_append_search_path(gtk_source_style_scheme_manager_get_default(),
		"resource://" BUNDLED_THEMES_RESOURCE);
	register_settings(THEME_SETTINGS, G_N_ELEMENTS(THEME_SETTINGS));
	add_settings_page_content(PAGE_ID, fill_page);
	watch_themes(on_themes_changed, NULL);
	apply_scheme_style();
	watch_settings(on_scheme_setting_changed, NULL);
}
