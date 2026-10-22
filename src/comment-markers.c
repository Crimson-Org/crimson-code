#include <stdlib.h>

#include <adwaita.h>
#include <gtksourceview/gtksource.h>

#include "../lang/en.h"
#include "comment-markers.h"
#include "common/key-store.h"
#include "settings-dialog.h"

#define MARKERS_FILE "comments.ini"
#define PAGE_ID "plan"
#define LINE_KEY "line"
#define BLOCK_START_KEY "block-start"
#define BLOCK_END_KEY "block-end"
#define FIELD_KEY "field"

/* GtkSourceView names for the same markers in its language files */
static const struct {
	const char *key, *metadata;
} FIELDS[] = {
	{LINE_KEY, "line-comment-start"}, {BLOCK_START_KEY, "block-comment-start"},
	{BLOCK_END_KEY, "block-comment-end"},
};

/* A function that hears about changed markers */
typedef struct {
	void (*on_changed)(gpointer data);
	gpointer data;
} Watcher;

/* The settings window's language list and the fields showing that language's markers */
typedef struct {
	GtkWidget *languages, *fields[G_N_ELEMENTS(FIELDS)];
	GStrv language_ids;
} MarkerPage;

static GKeyFile *saved;
static GPtrArray *watchers;


static GKeyFile *get_saved_markers(void) {
	if (!saved) {
		g_autofree char *path = get_config_file_path(MARKERS_FILE);
		saved = load_key_file(path, G_KEY_FILE_NONE);
	}
	return saved;
}


/* One marker, the person's own when they set it, even to nothing */
static char *get_marker(const char *language_id, guint field) {
	GKeyFile *file = get_saved_markers();
	if (g_key_file_has_key(file, language_id, FIELDS[field].key, NULL))
		return g_key_file_get_string(file, language_id, FIELDS[field].key, NULL);
	GtkSourceLanguageManager *manager = gtk_source_language_manager_get_default();
	GtkSourceLanguage *language = gtk_source_language_manager_get_language(manager, language_id);
	const char *marker = language ? gtk_source_language_get_metadata(language, FIELDS[field].metadata) : NULL;
	return g_strdup(marker ? marker : "");
}


CommentMarkers *get_comment_markers(const char *language_id) {
	CommentMarkers *markers = g_new0(CommentMarkers, 1);
	markers->line = get_marker(language_id, 0);
	markers->block_start = get_marker(language_id, 1);
	markers->block_end = get_marker(language_id, 2);
	return markers;
}


void free_comment_markers(CommentMarkers *markers) {
	g_free(markers->line);
	g_free(markers->block_start);
	g_free(markers->block_end);
	g_free(markers);
}


gboolean has_comment_markers(const CommentMarkers *markers) {
	return *markers->line || (*markers->block_start && *markers->block_end);
}


void watch_comment_markers(void (*on_changed)(gpointer data), gpointer data) {
	if (!watchers)
		watchers = g_ptr_array_new_with_free_func(g_free);
	Watcher *watcher = g_new(Watcher, 1);
	*watcher = (Watcher){on_changed, data};
	g_ptr_array_add(watchers, watcher);
}


void unwatch_comment_markers(gpointer data) {
	for (guint index = watchers ? watchers->len : 0; index > 0; index--)
		if (((Watcher *)g_ptr_array_index(watchers, index - 1))->data == data)
			g_ptr_array_remove_index(watchers, index - 1);
}


static void save_markers(void) {
	g_autofree char *path = get_config_file_path(MARKERS_FILE);
	save_key_file(get_saved_markers(), path);
	for (guint index = 0; watchers && index < watchers->len; index++) {
		Watcher *watcher = g_ptr_array_index(watchers, index);
		watcher->on_changed(watcher->data);
	}
}


/* ==================================== Settings page ==================================== */


static const char *get_chosen_language(MarkerPage *page) {
	guint chosen = adw_combo_row_get_selected(ADW_COMBO_ROW(page->languages));
	return chosen < g_strv_length(page->language_ids) ? page->language_ids[chosen] : NULL;
}


static void show_markers(MarkerPage *page) {
	const char *language_id = get_chosen_language(page);
	for (guint field = 0; field < G_N_ELEMENTS(FIELDS); field++) {
		g_autofree char *marker = language_id ? get_marker(language_id, field) : g_strdup("");
		gtk_editable_set_text(GTK_EDITABLE(page->fields[field]), marker);
	}
}


static void on_language_chosen(GObject *row, GParamSpec *property, MarkerPage *page) {
	show_markers(page);
}


static void on_marker_applied(AdwEntryRow *row, MarkerPage *page) {
	const char *language_id = get_chosen_language(page);
	if (!language_id)
		return;
	guint field = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), FIELD_KEY));
	g_key_file_set_string(get_saved_markers(), language_id, FIELDS[field].key,
		gtk_editable_get_text(GTK_EDITABLE(row)));
	save_markers();
}


static void reset_markers(AdwButtonRow *row, MarkerPage *page) {
	const char *language_id = get_chosen_language(page);
	if (!language_id)
		return;
	g_key_file_remove_group(get_saved_markers(), language_id, NULL);
	save_markers();
	show_markers(page);
}


static int compare_language_names(gconstpointer first, gconstpointer second) {
	GtkSourceLanguageManager *manager = gtk_source_language_manager_get_default();
	const char *first_name = gtk_source_language_get_name(gtk_source_language_manager_get_language(manager,
		*(const char **)first));
	const char *second_name = gtk_source_language_get_name(gtk_source_language_manager_get_language(manager,
		*(const char **)second));
	return g_utf8_collate(first_name, second_name);
}


static GtkWidget *create_language_row(MarkerPage *page) {
	GtkSourceLanguageManager *manager = gtk_source_language_manager_get_default();
	page->language_ids = g_strdupv((GStrv)gtk_source_language_manager_get_language_ids(manager));
	qsort(page->language_ids, g_strv_length(page->language_ids), sizeof(char *), compare_language_names);
	GtkStringList *names = gtk_string_list_new(NULL);
	for (guint index = 0; page->language_ids[index]; index++)
		gtk_string_list_append(names, gtk_source_language_get_name(gtk_source_language_manager_get_language(
			manager, page->language_ids[index])));
	GtkWidget *row = adw_combo_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), TEXT_COMMENT_LANGUAGE);
	adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(names));
	adw_combo_row_set_enable_search(ADW_COMBO_ROW(row), TRUE);
	g_object_unref(names);
	return row;
}


static void free_marker_page(gpointer data) {
	MarkerPage *page = data;
	g_strfreev(page->language_ids);
	g_free(page);
}


static void fill_page(AdwPreferencesDialog *dialog, AdwPreferencesPage *preferences) {
	static const char *const TITLES[] = {TEXT_COMMENT_LINE, TEXT_COMMENT_BLOCK_START, TEXT_COMMENT_BLOCK_END};
	MarkerPage *page = g_new0(MarkerPage, 1);
	AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
	adw_preferences_group_set_title(group, TEXT_GROUP_COMMENTS);
	adw_preferences_group_set_description(group, TEXT_GROUP_COMMENTS_DETAIL);
	page->languages = create_language_row(page);
	adw_preferences_group_add(group, page->languages);
	for (guint field = 0; field < G_N_ELEMENTS(FIELDS); field++) {
		page->fields[field] = adw_entry_row_new();
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(page->fields[field]), TITLES[field]);
		adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(page->fields[field]), FALSE);
		adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(page->fields[field]), TRUE);
		g_object_set_data(G_OBJECT(page->fields[field]), FIELD_KEY, GUINT_TO_POINTER(field));
		g_signal_connect(page->fields[field], "apply", G_CALLBACK(on_marker_applied), page);
		adw_preferences_group_add(group, page->fields[field]);
	}
	GtkWidget *reset = adw_button_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(reset), TEXT_COMMENT_RESET);
	g_signal_connect(reset, "activated", G_CALLBACK(reset_markers), page);
	adw_preferences_group_add(group, reset);
	g_object_set_data_full(G_OBJECT(group), "marker-page", page, free_marker_page);
	g_signal_connect(page->languages, "notify::selected", G_CALLBACK(on_language_chosen), page);
	show_markers(page);
	adw_preferences_page_add(preferences, group);
}


void register_comment_markers(void) {
	add_settings_page_content(PAGE_ID, fill_page);
}
