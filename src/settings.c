#include <string.h>

#include "settings.h"

#define SETTINGS_GROUP "settings"
#define TRUE_TEXT "true"
#define FALSE_TEXT "false"

/* A registered setting with its current value as text */
typedef struct {
	const SettingDefinition *definition;
	char *value;
} SettingValue;

/* A function that hears about changes */
typedef struct {
	SettingChangedFunction on_changed;
	gpointer data;
} Watcher;

static GPtrArray *pages, *settings, *watchers;
static GHashTable *values;
static GKeyFile *saved;


static char *get_settings_path(void) {
	return g_build_filename(g_get_user_config_dir(), "crimson-code", "settings.ini", NULL);
}


/* The file is read once, before the first setting is registered, so later components still find their values */
static void prepare(void) {
	if (settings)
		return;
	pages = g_ptr_array_new();
	settings = g_ptr_array_new();
	watchers = g_ptr_array_new_with_free_func(g_free);
	values = g_hash_table_new(g_str_hash, g_str_equal);
	saved = g_key_file_new();
	g_autofree char *path = get_settings_path();
	g_key_file_load_from_file(saved, path, G_KEY_FILE_NONE, NULL);
}


GPtrArray *list_setting_choices(const SettingDefinition *setting) {
	if (setting->list_choices)
		return setting->list_choices();
	GPtrArray *choices = g_ptr_array_new();
	for (const SettingChoice *choice = setting->choices; choice && choice->id; choice++)
		g_ptr_array_add(choices, (gpointer)choice);
	return choices;
}


static gboolean is_choice(const SettingDefinition *setting, const char *value) {
	g_autoptr(GPtrArray) choices = list_setting_choices(setting);
	for (guint index = 0; index < choices->len; index++)
		if (g_strcmp0(((SettingChoice *)g_ptr_array_index(choices, index))->id, value) == 0)
			return TRUE;
	return FALSE;
}


/* Gives the stored form of a value, or NULL when it does not fit the setting */
static char *normalise_value(const SettingDefinition *setting, const char *value) {
	if (!value)
		return NULL;
	switch (setting->kind) {
	case SETTING_SWITCH:
		if (g_ascii_strcasecmp(value, TRUE_TEXT) == 0 || g_strcmp0(value, "1") == 0)
			return g_strdup(TRUE_TEXT);
		if (g_ascii_strcasecmp(value, FALSE_TEXT) == 0 || g_strcmp0(value, "0") == 0)
			return g_strdup(FALSE_TEXT);
		return NULL;
	case SETTING_NUMBER: {
		char *end = NULL;
		gint64 number = g_ascii_strtoll(value, &end, 10);
		if (!*value || *end || number < setting->minimum || number > setting->maximum)
			return NULL;
		return g_strdup_printf("%" G_GINT64_FORMAT, number);
	}
	case SETTING_CHOICE:
		return is_choice(setting, value) ? g_strdup(value) : NULL;
	case SETTING_TEXT:
	case SETTING_FONT:
		return g_strdup(value);
	}
	return NULL;
}


void register_settings_pages(const SettingsPage *new_pages, guint count) {
	prepare();
	for (guint index = 0; index < count; index++)
		g_ptr_array_add(pages, (gpointer)&new_pages[index]);
}


void register_settings(const SettingDefinition *definitions, guint count) {
	prepare();
	for (guint index = 0; index < count; index++) {
		const SettingDefinition *definition = &definitions[index];
		SettingValue *setting = g_new0(SettingValue, 1);
		setting->definition = definition;
		g_autofree char *stored = g_key_file_get_string(saved, SETTINGS_GROUP, definition->key, NULL);
		setting->value = normalise_value(definition, stored);
		if (!setting->value)
			setting->value = g_strdup(definition->default_value);
		g_ptr_array_add(settings, (gpointer)definition);
		g_hash_table_insert(values, (gpointer)definition->key, setting);
	}
}


GPtrArray *list_settings_pages(void) {
	prepare();
	return pages;
}


GPtrArray *list_settings(void) {
	prepare();
	return settings;
}


const SettingDefinition *find_setting(const char *key) {
	prepare();
	SettingValue *setting = g_hash_table_lookup(values, key);
	return setting ? setting->definition : NULL;
}


static const char *get_value(const char *key) {
	prepare();
	SettingValue *setting = g_hash_table_lookup(values, key);
	if (!setting)
		g_critical("Setting %s was read before any component registered it", key);
	return setting ? setting->value : "";
}


gboolean get_switch_setting(const char *key) {
	return strcmp(get_value(key), TRUE_TEXT) == 0;
}


int get_number_setting(const char *key) {
	return (int)g_ascii_strtoll(get_value(key), NULL, 10);
}


const char *get_text_setting(const char *key) {
	return get_value(key);
}


/* Only values that differ from their defaults are written, so improved defaults reach everyone later */
static void save_settings(void) {
	for (guint index = 0; index < settings->len; index++) {
		const SettingDefinition *definition = g_ptr_array_index(settings, index);
		SettingValue *setting = g_hash_table_lookup(values, definition->key);
		if (g_strcmp0(setting->value, definition->default_value) == 0)
			g_key_file_remove_key(saved, SETTINGS_GROUP, definition->key, NULL);
		else
			g_key_file_set_string(saved, SETTINGS_GROUP, definition->key, setting->value);
	}
	g_autofree char *path = get_settings_path();
	g_autofree char *directory = g_path_get_dirname(path);
	g_autoptr(GError) error = NULL;
	if (g_mkdir_with_parents(directory, 0700) != 0 || !g_key_file_save_to_file(saved, path, &error))
		g_warning("Settings were not saved to %s", path);
}


gboolean change_setting(const char *key, const char *value) {
	prepare();
	SettingValue *setting = g_hash_table_lookup(values, key);
	g_autofree char *normal = setting ? normalise_value(setting->definition, value) : NULL;
	if (!normal)
		return FALSE;
	if (g_strcmp0(normal, setting->value) == 0)
		return TRUE;
	g_free(setting->value);
	setting->value = g_steal_pointer(&normal);
	save_settings();
	for (guint index = 0; index < watchers->len; index++) {
		Watcher *watcher = g_ptr_array_index(watchers, index);
		watcher->on_changed(key, watcher->data);
	}
	return TRUE;
}


void watch_settings(SettingChangedFunction on_changed, gpointer data) {
	prepare();
	Watcher *watcher = g_new0(Watcher, 1);
	watcher->on_changed = on_changed;
	watcher->data = data;
	g_ptr_array_add(watchers, watcher);
}


void unwatch_settings(gpointer data) {
	prepare();
	for (guint index = watchers->len; index > 0; index--)
		if (((Watcher *)g_ptr_array_index(watchers, index - 1))->data == data)
			g_ptr_array_remove_index(watchers, index - 1);
}


/* ==================================== Vim :set ==================================== */


static const SettingDefinition *find_vim_setting(const char *name) {
	for (guint index = 0; index < list_settings()->len; index++) {
		const SettingDefinition *setting = g_ptr_array_index(settings, index);
		if (strcmp(setting->key, name) == 0)
			return setting;
		for (int alias = 0; alias < 3 && setting->vim_names[alias]; alias++)
			if (strcmp(setting->vim_names[alias], name) == 0)
				return setting;
	}
	return NULL;
}


/* Follows Vim, where "name" turns a switch on or shows a value, "noname" turns it off and "invname" or "name!"
 * toggles it */
gboolean run_set_command(const char *argument, char **message) {
	g_autofree char *text = g_strstrip(g_strdup(argument ? argument : ""));
	char *equals = strchr(text, '=');
	if (equals)
		*equals = '\0';
	gboolean is_flipped = g_str_has_suffix(text, "!") || g_str_has_prefix(text, "inv");
	if (g_str_has_suffix(text, "!"))
		text[strlen(text) - 1] = '\0';
	const char *name = g_str_has_prefix(text, "inv") ? text + 3 : text;
	const SettingDefinition *setting = find_vim_setting(name);
	gboolean is_off = !setting && g_str_has_prefix(name, "no");
	if (is_off)
		setting = find_vim_setting(name + 2);
	if (!setting) {
		*message = g_strdup_printf("Unknown option: %s", name);
		return FALSE;
	}
	const char *value = equals ? equals + 1 : NULL;
	if (setting->kind == SETTING_SWITCH && !value)
		value = is_flipped ? (get_switch_setting(setting->key) ? FALSE_TEXT : TRUE_TEXT) : is_off ? FALSE_TEXT
			: TRUE_TEXT;
	if (!value) {
		*message = g_strdup_printf("%s=%s", name, get_text_setting(setting->key));
		return TRUE;
	}
	if (!change_setting(setting->key, value)) {
		*message = g_strdup_printf("Invalid value for %s: %s", name, value);
		return FALSE;
	}
	*message = NULL;
	return TRUE;
}
