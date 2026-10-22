#include "../lang/en.h"
#include "settings-dialog.h"
#include "settings.h"

#define KEY_DATA "setting-key"

/* A component's extra content for one page */
typedef struct {
	const char *page;
	SettingsPageFillFunction fill;
} PageContent;

/* The open window and its rows by setting key, so outside changes such as zooming show at once */
typedef struct {
	AdwDialog *dialog;
	GHashTable *rows;
	gboolean is_updating;
} SettingsDialog;

static GPtrArray *page_contents;


void add_settings_page_content(const char *page, SettingsPageFillFunction fill) {
	if (!page_contents)
		page_contents = g_ptr_array_new_with_free_func(g_free);
	PageContent *content = g_new0(PageContent, 1);
	content->page = page;
	content->fill = fill;
	g_ptr_array_add(page_contents, content);
}


/* ==================================== Rows ==================================== */


static void store_value(SettingsDialog *dialog, GtkWidget *row, const char *value) {
	if (!dialog->is_updating)
		change_setting(g_object_get_data(G_OBJECT(row), KEY_DATA), value);
}


static void on_switch_changed(GtkWidget *row, GParamSpec *property, SettingsDialog *dialog) {
	store_value(dialog, row, adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? "true" : "false");
}


static void on_number_changed(GtkWidget *row, GParamSpec *property, SettingsDialog *dialog) {
	g_autofree char *value = g_strdup_printf("%d", (int)adw_spin_row_get_value(ADW_SPIN_ROW(row)));
	store_value(dialog, row, value);
}


static void on_text_applied(GtkWidget *row, SettingsDialog *dialog) {
	store_value(dialog, row, gtk_editable_get_text(GTK_EDITABLE(row)));
}


static void on_choice_changed(GtkWidget *row, GParamSpec *property, SettingsDialog *dialog) {
	GPtrArray *choices = g_object_get_data(G_OBJECT(row), "choices");
	guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
	if (selected < choices->len)
		store_value(dialog, row, ((SettingChoice *)g_ptr_array_index(choices, selected))->id);
}


static void on_font_changed(GtkFontDialogButton *button, GParamSpec *property, SettingsDialog *dialog) {
	PangoFontDescription *font = gtk_font_dialog_button_get_font_desc(button);
	const char *family = font ? pango_font_description_get_family(font) : NULL;
	store_value(dialog, g_object_get_data(G_OBJECT(button), "row"), family ? family : "");
}


static void reset_font(GtkButton *button, SettingsDialog *dialog) {
	store_value(dialog, g_object_get_data(G_OBJECT(button), "row"), "");
}


/* Shows the stored value in a row without sending it back as a change */
static void show_value(SettingsDialog *dialog, GtkWidget *row, const SettingDefinition *setting) {
	dialog->is_updating = TRUE;
	const char *value = get_text_setting(setting->key);
	switch (setting->kind) {
	case SETTING_SWITCH:
		adw_switch_row_set_active(ADW_SWITCH_ROW(row), get_switch_setting(setting->key));
		break;
	case SETTING_NUMBER:
		adw_spin_row_set_value(ADW_SPIN_ROW(row), get_number_setting(setting->key));
		break;
	case SETTING_TEXT:
		gtk_editable_set_text(GTK_EDITABLE(row), value);
		break;
	case SETTING_CHOICE: {
		GPtrArray *choices = g_object_get_data(G_OBJECT(row), "choices");
		for (guint index = 0; index < choices->len; index++)
			if (g_strcmp0(((SettingChoice *)g_ptr_array_index(choices, index))->id, value) == 0)
				adw_combo_row_set_selected(ADW_COMBO_ROW(row), index);
		break;
	}
	case SETTING_FONT: {
		GtkWidget *button = g_object_get_data(G_OBJECT(row), "button");
		g_autoptr(PangoFontDescription) font = pango_font_description_from_string(*value ? value : "Monospace");
		gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(button), font);
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), *value ? value : TEXT_SYSTEM_MONOSPACE);
		break;
	}
	}
	dialog->is_updating = FALSE;
}


static gboolean is_monospace_font(gpointer item, gpointer data) {
	PangoFontFamily *family = PANGO_IS_FONT_FACE(item) ? pango_font_face_get_family(item) : item;
	return pango_font_family_is_monospace(family);
}


static GtkWidget *create_font_row(SettingsDialog *dialog) {
	GtkWidget *row = adw_action_row_new();
	GtkFontDialog *chooser = gtk_font_dialog_new();
	gtk_font_dialog_set_filter(chooser, GTK_FILTER(gtk_custom_filter_new(is_monospace_font, NULL, NULL)));
	GtkWidget *button = gtk_font_dialog_button_new(chooser);
	gtk_font_dialog_button_set_level(GTK_FONT_DIALOG_BUTTON(button), GTK_FONT_LEVEL_FAMILY);
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	g_object_set_data(G_OBJECT(button), "row", row);
	g_signal_connect(button, "notify::font-desc", G_CALLBACK(on_font_changed), dialog);
	GtkWidget *reset = gtk_button_new_from_icon_name("edit-undo-symbolic");
	gtk_widget_set_valign(reset, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(reset, TEXT_SYSTEM_MONOSPACE);
	gtk_widget_add_css_class(reset, "flat");
	g_object_set_data(G_OBJECT(reset), "row", row);
	g_signal_connect(reset, "clicked", G_CALLBACK(reset_font), dialog);
	adw_action_row_add_suffix(ADW_ACTION_ROW(row), reset);
	adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
	g_object_set_data(G_OBJECT(row), "button", button);
	return row;
}


static void set_up_choice(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_NONE);
	gtk_list_item_set_child(item, label);
}


static void bind_choice(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	GtkStringObject *text = gtk_list_item_get_item(item);
	gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), gtk_string_object_get_string(text));
}


/* The default combo row shortens long choices with an ellipsis, so its labels are replaced with full ones */
static GtkWidget *create_choice_row(SettingsDialog *dialog, const SettingDefinition *setting) {
	GtkWidget *row = adw_combo_row_new();
	GPtrArray *choices = list_setting_choices(setting);
	g_object_set_data_full(G_OBJECT(row), "choices", choices, (GDestroyNotify)g_ptr_array_unref);
	GtkStringList *labels = gtk_string_list_new(NULL);
	for (guint index = 0; index < choices->len; index++)
		gtk_string_list_append(labels, ((SettingChoice *)g_ptr_array_index(choices, index))->label);
	adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(labels));
	g_object_unref(labels);
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(set_up_choice), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_choice), NULL);
	adw_combo_row_set_factory(ADW_COMBO_ROW(row), factory);
	g_object_unref(factory);
	return row;
}


static GtkWidget *create_row(SettingsDialog *dialog, const SettingDefinition *setting) {
	GtkWidget *row = NULL;
	switch (setting->kind) {
	case SETTING_SWITCH:
		row = adw_switch_row_new();
		break;
	case SETTING_NUMBER:
		row = adw_spin_row_new_with_range(setting->minimum, setting->maximum, 1);
		break;
	case SETTING_TEXT:
		row = adw_entry_row_new();
		adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(row), TRUE);
		break;
	case SETTING_CHOICE:
		row = create_choice_row(dialog, setting);
		break;
	case SETTING_FONT:
		row = create_font_row(dialog);
		break;
	}
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), setting->title);
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
	if (setting->description && ADW_IS_ACTION_ROW(row) && setting->kind != SETTING_FONT)
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), setting->description);
	else if (setting->description)
		gtk_widget_set_tooltip_text(row, setting->description);
	g_object_set_data(G_OBJECT(row), KEY_DATA, (gpointer)setting->key);
	show_value(dialog, row, setting);
	static const char *const SIGNALS[] = {"notify::active", "notify::value", "apply", "notify::selected", NULL};
	static const GCallback HANDLERS[] = {G_CALLBACK(on_switch_changed), G_CALLBACK(on_number_changed),
		G_CALLBACK(on_text_applied), G_CALLBACK(on_choice_changed), NULL};
	if (SIGNALS[setting->kind])
		g_signal_connect(row, SIGNALS[setting->kind], HANDLERS[setting->kind], dialog);
	g_hash_table_insert(dialog->rows, (gpointer)setting->key, row);
	return row;
}


/* ==================================== Pages ==================================== */


static AdwPreferencesGroup *find_group(AdwPreferencesPage *page, GHashTable *groups, const char *title) {
	AdwPreferencesGroup *group = g_hash_table_lookup(groups, title);
	if (group)
		return group;
	group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
	adw_preferences_group_set_title(group, title);
	adw_preferences_page_add(page, group);
	g_hash_table_insert(groups, (gpointer)title, group);
	return group;
}


static AdwPreferencesPage *create_page(SettingsDialog *dialog, const SettingsPage *description) {
	AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
	adw_preferences_page_set_name(page, description->id);
	adw_preferences_page_set_title(page, description->title);
	adw_preferences_page_set_icon_name(page, description->icon);
	g_autoptr(GHashTable) groups = g_hash_table_new(g_str_hash, g_str_equal);
	GPtrArray *settings = list_settings();
	for (guint index = 0; index < settings->len; index++) {
		const SettingDefinition *setting = g_ptr_array_index(settings, index);
		if (g_strcmp0(setting->page, description->id) == 0)
			adw_preferences_group_add(find_group(page, groups, setting->group), create_row(dialog, setting));
	}
	for (guint index = 0; page_contents && index < page_contents->len; index++) {
		PageContent *content = g_ptr_array_index(page_contents, index);
		if (g_strcmp0(content->page, description->id) == 0)
			content->fill(ADW_PREFERENCES_DIALOG(dialog->dialog), page);
	}
	return page;
}


static void on_setting_changed(const char *key, gpointer data) {
	SettingsDialog *dialog = data;
	GtkWidget *row = g_hash_table_lookup(dialog->rows, key);
	if (row)
		show_value(dialog, row, find_setting(key));
}


static void on_closed(AdwDialog *widget, SettingsDialog *dialog) {
	unwatch_settings(dialog);
	g_hash_table_unref(dialog->rows);
	g_free(dialog);
}


void show_settings_dialog(GtkWidget *parent, const char *page) {
	SettingsDialog *dialog = g_new0(SettingsDialog, 1);
	dialog->rows = g_hash_table_new(g_str_hash, g_str_equal);
	dialog->dialog = adw_preferences_dialog_new();
	adw_dialog_set_title(dialog->dialog, TEXT_SETTINGS);
	adw_dialog_set_content_width(dialog->dialog, 820);
	adw_dialog_set_content_height(dialog->dialog, 720);
	GPtrArray *pages = list_settings_pages();
	for (guint index = 0; index < pages->len; index++)
		adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog->dialog), create_page(dialog,
			g_ptr_array_index(pages, index)));
	if (page)
		adw_preferences_dialog_set_visible_page_name(ADW_PREFERENCES_DIALOG(dialog->dialog), page);
	watch_settings(on_setting_changed, dialog);
	g_signal_connect(dialog->dialog, "closed", G_CALLBACK(on_closed), dialog);
	adw_dialog_present(dialog->dialog, parent);
}
