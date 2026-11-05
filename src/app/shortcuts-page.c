#include "lang/en.h"
#include "commands.h"
#include "settings-dialog.h"
#include "settings.h"
#include "shortcuts-page.h"
#include "shortcut-keys.h"
#include "common/widgets/spacing.h"

#define PAGE_ID "shortcuts"


/* Each shortcut is a row of key caps, with a wider gap between one shortcut and the next */
static GtkWidget *create_shortcuts(const Command *command) {
	GtkWidget *shortcuts = NULL;
	for (int index = 0; index < (int)G_N_ELEMENTS(command->shortcuts) && command->shortcuts[index]; index++) {
		GtkWidget *keys = create_shortcut_keys(command->shortcuts[index]);
		if (!keys)
			continue;
		if (!shortcuts)
			shortcuts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_NORMAL);
		gtk_box_append(GTK_BOX(shortcuts), keys);
	}
	return shortcuts;
}


static char *describe_names(const char *const *names, int count, const char *prefix) {
	GString *text = g_string_new(NULL);
	for (int index = 0; index < count && names[index]; index++)
		g_string_append_printf(text, "%s%s%s", text->len ? "  " : "", prefix, names[index]);
	return g_string_free(text, FALSE);
}


static GtkWidget *create_row(const char *title, const char *subtitle, GtkWidget *suffix) {
	GtkWidget *row = adw_action_row_new();
	gtk_widget_add_css_class(row, "command-row");
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	if (subtitle && *subtitle)
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
	if (suffix)
		adw_action_row_add_suffix(ADW_ACTION_ROW(row), suffix);
	return row;
}


static void fill_page(AdwPreferencesDialog *dialog, AdwPreferencesPage *page) {
	GtkWidget *commands_group = adw_preferences_group_new();
	/* The page is already named Commands, so its first group only says how to type them */
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(commands_group), TEXT_GROUP_COMMANDS_DETAIL);
	GPtrArray *commands = list_commands();
	for (guint index = 0; index < commands->len; index++) {
		const Command *command = g_ptr_array_index(commands, index);
		g_autofree char *names = describe_names(command->vim_names, G_N_ELEMENTS(command->vim_names), ":");
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(commands_group), create_row(command->label, names,
			create_shortcuts(command)));
	}
	adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(commands_group));
	GtkWidget *options_group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(options_group), TEXT_GROUP_VIM_OPTIONS);
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(options_group), TEXT_GROUP_VIM_OPTIONS_DETAIL);
	GPtrArray *settings = list_settings();
	for (guint index = 0; index < settings->len; index++) {
		const SettingDefinition *setting = g_ptr_array_index(settings, index);
		g_autofree char *names = describe_names(setting->vim_names, G_N_ELEMENTS(setting->vim_names), ":set ");
		g_autofree char *key = g_strconcat(":set ", setting->key, NULL);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(options_group), create_row(setting->title, *names ? names
			: key, NULL));
	}
	adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(options_group));
}


void register_shortcuts_page(void) {
	add_settings_page_content(PAGE_ID, fill_page);
}
