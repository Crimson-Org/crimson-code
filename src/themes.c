#include <string.h>

#include <adwaita.h>
#include <gtksourceview/gtksource.h>

#include "../lang/en.h"
#include "settings-dialog.h"
#include "settings.h"
#include "terminal.h"
#include "themes.h"
#include "window.h"

#define BUNDLED_THEMES "resource:///io/github/CrimsonOrg/CrimsonCode/styles"
#define TEMPLATE_RESOURCE "/io/github/CrimsonOrg/CrimsonCode/styles/crimson-monokai.xml"
#define TEMPLATE_ID "crimson-monokai"
#define TEMPLATE_NAME "Crimson Monokai"
#define PAGE_ID "appearance"
#define MAXIMUM_THEME_NUMBER 1000

static GPtrArray *list_schemes(void);

/* How far derived window colours move from the code background towards black in dark themes or white in light */
#define SIDEBAR_SHADE 0.12
#define BAR_SHADE 0.45
#define LIFT_SHADE 0.06
#define DARK_CARD_SHADE 0.05
#define LIGHT_CARD_SHADE 0.6

static const SettingDefinition THEME_SETTINGS[] = {
	{"theme-window", PAGE_ID, TEXT_GROUP_COLOURS, TEXT_SETTING_THEME_WINDOW, TEXT_SETTING_THEME_WINDOW_DETAIL,
		SETTING_SWITCH, "true", 0, 0, NULL, NULL, {NULL}},
	{"light-scheme", PAGE_ID, TEXT_GROUP_COLOURS, TEXT_SETTING_LIGHT_SCHEME, NULL, SETTING_CHOICE,
		"crimson-monokai-light", 0, 0, NULL, list_schemes, {NULL}},
	{"dark-scheme", PAGE_ID, TEXT_GROUP_COLOURS, TEXT_SETTING_DARK_SCHEME, NULL, SETTING_CHOICE, "crimson-monokai", 0,
		0, NULL, list_schemes, {NULL}},
};

/* Explains the format at the top of each new theme, since editing the file is how a theme is made */
static const char TEMPLATE_HELP[] =
	"<!--\n"
	"  Saving this file updates the theme in Crimson Code at once.\n"
	"  Colours are #rrggbb values or names from the <color> lines below.\n"
	"  A style can set foreground, background, bold, italic, underline and strikethrough.\n"
	"  Styles named text, selection, cursor, current-line, line-numbers, current-line-number,\n"
	"  bracket-match, search-match, right-margin and draw-spaces colour the editor itself.\n"
	"  Styles named crimson:titlebar, crimson:sidebar and crimson:statusbar colour the window around it.\n"
	"  Styles starting with def: colour code in all languages, such as def:comment or def:keyword,\n"
	"  and a language can be coloured on its own with its id, such as c:preprocessor.\n"
	"  The full list is at https://gnome.pages.gitlab.gnome.org/gtksourceview/gtksourceview5/style-reference.html\n"
	"-->\n";

static GFileMonitor *theme_monitor;
static GtkCssProvider *scheme_style;


static GPtrArray *list_schemes(void) {
	GtkSourceStyleSchemeManager *schemes = gtk_source_style_scheme_manager_get_default();
	GPtrArray *choices = g_ptr_array_new_with_free_func(g_free);
	for (const char *const *id = gtk_source_style_scheme_manager_get_scheme_ids(schemes); id && *id; id++) {
		SettingChoice *choice = g_new0(SettingChoice, 1);
		choice->id = *id;
		choice->label = gtk_source_style_scheme_get_name(gtk_source_style_scheme_manager_get_scheme(schemes, *id));
		g_ptr_array_add(choices, choice);
	}
	return choices;
}


/* People's own themes live where all GtkSourceView editors look, so they also work in other editors */
static char *get_theme_folder(void) {
	return g_build_filename(g_get_user_data_dir(), "gtksourceview-5", "styles", NULL);
}


static void on_theme_folder_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event,
	gpointer data) {
	if (event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || event == G_FILE_MONITOR_EVENT_DELETED
		|| event == G_FILE_MONITOR_EVENT_RENAMED || event == G_FILE_MONITOR_EVENT_MOVED_IN)
		gtk_source_style_scheme_manager_force_rescan(gtk_source_style_scheme_manager_get_default());
}


static void watch_theme_folder(void) {
	g_autofree char *path = get_theme_folder();
	if (theme_monitor || !g_file_test(path, G_FILE_TEST_IS_DIR))
		return;
	g_autoptr(GFile) folder = g_file_new_for_path(path);
	theme_monitor = g_file_monitor_directory(folder, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
	if (theme_monitor)
		g_signal_connect(theme_monitor, "changed", G_CALLBACK(on_theme_folder_changed), NULL);
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
	GtkSourceStyleSchemeManager *schemes = gtk_source_style_scheme_manager_get_default();
	for (int number = 1; number < MAXIMUM_THEME_NUMBER; number++) {
		g_autofree char *candidate = number == 1 ? g_strdup("my-theme") : g_strdup_printf("my-theme-%d", number);
		g_autofree char *file_name = g_strconcat(candidate, ".xml", NULL);
		g_autofree char *path = g_build_filename(folder, file_name, NULL);
		if (g_file_test(path, G_FILE_TEST_EXISTS) || gtk_source_style_scheme_manager_get_scheme(schemes, candidate))
			continue;
		*id = g_steal_pointer(&candidate);
		*name = number == 1 ? g_strdup(TEXT_MY_THEME) : g_strdup_printf("%s %d", TEXT_MY_THEME, number);
		return g_file_new_for_path(path);
	}
	return NULL;
}


static void create_theme(AdwButtonRow *row, AdwPreferencesDialog *dialog) {
	g_autofree char *folder = get_theme_folder();
	g_autofree char *id = NULL, *name = NULL;
	g_autoptr(GFile) file = g_mkdir_with_parents(folder, 0755) == 0 ? choose_theme_file(folder, &id, &name) : NULL;
	g_autofree char *text = file ? make_theme_text(id, name) : NULL;
	g_autoptr(GError) error = NULL;
	g_autofree char *path = file ? g_file_get_path(file) : NULL;
	if (!file || !g_file_set_contents(path, text, -1, &error)) {
		g_warning("A new theme could not be written to %s", folder);
		return;
	}
	watch_theme_folder();
	gtk_source_style_scheme_manager_force_rescan(gtk_source_style_scheme_manager_get_default());
	change_setting(adw_style_manager_get_dark(adw_style_manager_get_default()) ? "dark-scheme" : "light-scheme", id);
	GtkWindow *active = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
	Window *window = active ? g_object_get_data(G_OBJECT(active), "window") : NULL;
	adw_dialog_close(ADW_DIALOG(dialog));
	if (window)
		open_window_path(window, file);
}


static void open_theme_folder(AdwButtonRow *row, AdwPreferencesDialog *dialog) {
	g_autofree char *path = get_theme_folder();
	g_mkdir_with_parents(path, 0755);
	g_autoptr(GFile) folder = g_file_new_for_path(path);
	g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new(folder);
	gtk_file_launcher_launch(launcher, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(dialog))), NULL, NULL, NULL);
	watch_theme_folder();
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


/* Reads one colour of a scheme style, which is only there when the scheme sets it */
static gboolean get_style_colour(GtkSourceStyleScheme *scheme, const char *name, const char *property,
	GdkRGBA *colour) {
	GtkSourceStyle *style = scheme ? gtk_source_style_scheme_get_style(scheme, name) : NULL;
	if (!style)
		return FALSE;
	g_autofree char *set_name = g_strconcat(property, "-set", NULL);
	g_autofree char *value = NULL;
	gboolean is_set = FALSE;
	g_object_get(style, property, &value, set_name, &is_set, NULL);
	return is_set && value && gdk_rgba_parse(colour, value);
}


static GdkRGBA shade(GdkRGBA colour, double amount, gboolean is_dark) {
	float target = is_dark ? 0.0f : 1.0f;
	colour.red += (target - colour.red) * (float)amount;
	colour.green += (target - colour.green) * (float)amount;
	colour.blue += (target - colour.blue) * (float)amount;
	return colour;
}


/* A window part takes the scheme's own crimson style when it has one, or a shade of the code background
 * It is left alone when the scheme gives no text colour to go with it */
static gboolean get_part_colours(GtkSourceStyleScheme *scheme, const char *style, double amount, gboolean is_dark,
	GdkRGBA *background, GdkRGBA *foreground) {
	GdkRGBA text_background = {is_dark ? 0.12f : 1, is_dark ? 0.12f : 1, is_dark ? 0.12f : 1, 1}, text_foreground;
	get_style_colour(scheme, "text", "background", &text_background);
	gboolean has_foreground = get_style_colour(scheme, "text", "foreground", &text_foreground);
	if (!get_style_colour(scheme, style, "background", background))
		*background = shade(text_background, amount, is_dark);
	if (get_style_colour(scheme, style, "foreground", foreground))
		return TRUE;
	*foreground = text_foreground;
	return has_foreground;
}


static void append_rule(GString *css, const char *selector, const GdkRGBA *background, const GdkRGBA *foreground) {
	g_autofree char *background_text = gdk_rgba_to_string(background);
	g_autofree char *foreground_text = gdk_rgba_to_string(foreground);
	g_string_append_printf(css, "%s { background-color: %s; color: %s; }\n", selector, background_text,
		foreground_text);
}


/* Sets libadwaita's colour variables for one kind of surface, such as view or headerbar */
static void append_variables(GString *css, const char *surface, const GdkRGBA *background,
	const GdkRGBA *foreground) {
	g_autofree char *background_text = gdk_rgba_to_string(background);
	g_autofree char *foreground_text = gdk_rgba_to_string(foreground);
	g_string_append_printf(css, "  --%s-bg-color: %s;\n  --%s-fg-color: %s;\n", surface, background_text, surface,
		foreground_text);
}


static double get_luminance(const GdkRGBA *colour) {
	return 0.2126 * colour->red + 0.7152 * colour->green + 0.0722 * colour->blue;
}


/* Popovers and dialogs float above the window, so they sit a little lighter in dark themes and darker in light */
static GdkRGBA lift(GdkRGBA colour, gboolean is_dark) {
	return shade(colour, LIFT_SHADE, !is_dark);
}


/* Code outside the editor takes the scheme's text colours
 * With window theming on, libadwaita's colour variables make the window follow the scheme too */
static void apply_scheme_style(void) {
	GtkSourceStyleSchemeManager *schemes = gtk_source_style_scheme_manager_get_default();
	gboolean is_dark = adw_style_manager_get_dark(adw_style_manager_get_default());
	GtkSourceStyleScheme *scheme = gtk_source_style_scheme_manager_get_scheme(schemes,
		get_text_setting(is_dark ? "dark-scheme" : "light-scheme"));
	g_autoptr(GString) css = g_string_new(NULL);
	GdkRGBA text_background, text_foreground, background, foreground;
	gboolean has_text = get_part_colours(scheme, "text", 0, is_dark, &text_background, &text_foreground);
	/* A light scheme may be chosen for the dark style or the other way round, so shading follows the scheme */
	if (get_style_colour(scheme, "text", "background", &background))
		is_dark = get_luminance(&background) < 0.5;
	set_terminal_background(has_text ? &text_background : NULL);
	if (has_text)
		append_rule(css, ".hover-code, .problems-panel, terminal", &text_background, &text_foreground);
	if (has_text && get_switch_setting("theme-window")) {
		g_string_append(css, ":root {\n");
		append_variables(css, "window", &text_background, &text_foreground);
		append_variables(css, "view", &text_background, &text_foreground);
		GdkRGBA raised = lift(text_background, is_dark);
		append_variables(css, "popover", &raised, &text_foreground);
		append_variables(css, "dialog", &raised, &text_foreground);
		/* Cards such as settings rows sit just lighter than the window, which libadwaita would otherwise colour for
		 * its own style even when the scheme is the opposite */
		GdkRGBA card = shade(text_background, is_dark ? DARK_CARD_SHADE : LIGHT_CARD_SHADE, FALSE);
		append_variables(css, "card", &card, &text_foreground);
		append_variables(css, "thumbnail", &card, &text_foreground);
		append_variables(css, "overview", &text_background, &text_foreground);
		if (get_part_colours(scheme, "crimson:titlebar", BAR_SHADE, is_dark, &background, &foreground)) {
			append_variables(css, "headerbar", &background, &foreground);
			g_autofree char *backdrop = gdk_rgba_to_string(&background);
			g_string_append_printf(css, "  --headerbar-backdrop-color: %s;\n", backdrop);
		}
		if (get_part_colours(scheme, "crimson:sidebar", SIDEBAR_SHADE, is_dark, &background, &foreground)) {
			append_variables(css, "sidebar", &background, &foreground);
			append_variables(css, "secondary-sidebar", &background, &foreground);
			g_autofree char *backdrop = gdk_rgba_to_string(&background);
			g_string_append_printf(css, "  --sidebar-backdrop-color: %s;\n", backdrop);
		}
		g_string_append(css, "}\n");
		/* Bars inside a toolbar view draw the window colour rather than the header bar one, so the title bar gets
		 * its colour directly */
		if (get_part_colours(scheme, "crimson:titlebar", BAR_SHADE, is_dark, &background, &foreground))
			append_rule(css, "headerbar.title-bar", &background, &foreground);
		if (get_part_colours(scheme, "crimson:statusbar", BAR_SHADE, is_dark, &background, &foreground))
			append_rule(css, ".status-bar", &background, &foreground);
	}
	if (!scheme_style) {
		scheme_style = gtk_css_provider_new();
		gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(scheme_style),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	}
	gtk_css_provider_load_from_string(scheme_style, css->str);
}


static void on_scheme_changed(GObject *object, GParamSpec *property, gpointer data) {
	apply_scheme_style();
}


static void on_scheme_setting_changed(const char *key, gpointer data) {
	if (!key || g_strcmp0(key, "light-scheme") == 0 || g_strcmp0(key, "dark-scheme") == 0
		|| g_strcmp0(key, "theme-window") == 0)
		apply_scheme_style();
}


void register_themes(void) {
	gtk_source_style_scheme_manager_append_search_path(gtk_source_style_scheme_manager_get_default(),
		BUNDLED_THEMES);
	register_settings(THEME_SETTINGS, G_N_ELEMENTS(THEME_SETTINGS));
	add_settings_page_content(PAGE_ID, fill_page);
	watch_theme_folder();
	apply_scheme_style();
	watch_settings(on_scheme_setting_changed, NULL);
	g_signal_connect(adw_style_manager_get_default(), "notify::dark", G_CALLBACK(on_scheme_changed), NULL);
	g_signal_connect(gtk_source_style_scheme_manager_get_default(), "notify::scheme-ids",
		G_CALLBACK(on_scheme_changed), NULL);
}
