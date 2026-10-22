#pragma once

#include <glib.h>

/* How a setting is stored and which control the settings window shows for it */
typedef enum {
	SETTING_SWITCH,
	SETTING_NUMBER,
	SETTING_TEXT,
	SETTING_CHOICE,
	SETTING_FONT,
} SettingKind;

/* One option of a choice setting, stored by id and shown by label */
typedef struct {
	const char *id, *label;
} SettingChoice;

/* A page of the settings window, which components and later plugins can add */
typedef struct {
	const char *id, *title, *icon;
} SettingsPage;

/* A setting any component lists, from which the settings window, the file and :set are all made */
typedef struct {
	const char *key;
	/* Page id and group title in the settings window */
	const char *page, *group;
	const char *title, *description;
	SettingKind kind;
	/* Value as text, such as "true", "4" or a choice id */
	const char *default_value;
	int minimum, maximum;
	/* Fixed options ending with an empty entry, or NULL when list_choices gives them */
	const SettingChoice *choices;
	/* Options that depend on the system, such as installed colour schemes, in a new array of SettingChoice */
	GPtrArray *(*list_choices)(void);
	/* Names that :set accepts, such as "relativenumber" and "rnu" */
	const char *vim_names[3];
} SettingDefinition;

typedef void (*SettingChangedFunction)(const char *key, gpointer data);

void register_settings_pages(const SettingsPage *pages, guint count);
void register_settings(const SettingDefinition *definitions, guint count);
GPtrArray *list_settings_pages(void);
GPtrArray *list_settings(void);
const SettingDefinition *find_setting(const char *key);

gboolean get_switch_setting(const char *key);
int get_number_setting(const char *key);
/* Text, choice ids and font names */
const char *get_text_setting(const char *key);
/* Checks and stores the value, saves the file and tells each watcher, returning FALSE for a bad value */
gboolean change_setting(const char *key, const char *value);
GPtrArray *list_setting_choices(const SettingDefinition *setting);

/* Watchers hear about each change and filter the keys they use */
void watch_settings(SettingChangedFunction on_changed, gpointer data);
void unwatch_settings(gpointer data);

/* Runs the argument of a Vim :set command, such as "rnu", "nornu", "invrnu" or "ts=4", with a message to show */
gboolean run_set_command(const char *argument, char **message);
