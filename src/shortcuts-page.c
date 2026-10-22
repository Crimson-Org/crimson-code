#include "../lang/en.h"
#include "commands.h"
#include "settings-dialog.h"
#include "settings.h"
#include "shortcuts-page.h"

#define PAGE_ID "shortcuts"


static char *describe_shortcuts(const Command *command) {
	GString *text = g_string_new(NULL);
	for (int index = 0; index < 3 && command->shortcuts[index]; index++) {
		guint key = 0;
		GdkModifierType modifiers = 0;
		if (!gtk_accelerator_parse(command->shortcuts[index], &key, &modifiers))
			continue;
		g_autofree char *label = gtk_accelerator_get_label(key, modifiers);
		g_string_append_printf(text, "%s%s", text->len ? "   " : "", label);
	}
	return g_string_free(text, FALSE);
}


static char *describe_names(const char *const *names, int count, const char *prefix) {
	GString *text = g_string_new(NULL);
	for (int index = 0; index < count && names[index]; index++)
		g_string_append_printf(text, "%s%s%s", text->len ? "  " : "", prefix, names[index]);
	return g_string_free(text, FALSE);
}


static GtkWidget *create_row(const char *title, const char *subtitle, const char *suffix) {
	GtkWidget *row = adw_action_row_new();
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	if (subtitle && *subtitle)
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
	if (suffix && *suffix) {
		GtkWidget *label = gtk_label_new(suffix);
		gtk_widget_add_css_class(label, "dim-label");
		adw_action_row_add_suffix(ADW_ACTION_ROW(row), label);
	}
	return row;
}


static void fill_page(AdwPreferencesDialog *dialog, AdwPreferencesPage *page) {
	GtkWidget *commands_group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(commands_group), TEXT_GROUP_COMMANDS);
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(commands_group), TEXT_GROUP_COMMANDS_DETAIL);
	GPtrArray *commands = list_commands();
	for (guint index = 0; index < commands->len; index++) {
		const Command *command = g_ptr_array_index(commands, index);
		g_autofree char *names = describe_names(command->vim_names, 4, ":");
		g_autofree char *shortcuts = describe_shortcuts(command);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(commands_group), create_row(command->label, names,
			shortcuts));
	}
	adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(commands_group));
	GtkWidget *options_group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(options_group), TEXT_GROUP_VIM_OPTIONS);
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(options_group), TEXT_GROUP_VIM_OPTIONS_DETAIL);
	GPtrArray *settings = list_settings();
	for (guint index = 0; index < settings->len; index++) {
		const SettingDefinition *setting = g_ptr_array_index(settings, index);
		g_autofree char *names = describe_names(setting->vim_names, 3, ":set ");
		g_autofree char *key = g_strconcat(":set ", setting->key, NULL);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(options_group), create_row(setting->title, *names ? names
			: key, NULL));
	}
	adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(options_group));
}


void register_shortcuts_page(void) {
	add_settings_page_content(PAGE_ID, fill_page);
}
