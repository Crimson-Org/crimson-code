#pragma once

#include <adwaita.h>

/* Adds a component's own groups to a settings page, below the settings registered for it */
typedef void (*SettingsPageFillFunction)(AdwPreferencesDialog *dialog, AdwPreferencesPage *page);

void add_settings_page_content(const char *page, SettingsPageFillFunction fill);
/* Builds the settings window from each registered page and setting, opening the given page or the first */
void show_settings_dialog(GtkWidget *parent, const char *page);
