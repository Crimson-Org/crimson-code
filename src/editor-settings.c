#include <gtksourceview/gtksource.h>

#include "../lang/en.h"
#include "editor-settings.h"
#include "settings.h"

static const SettingChoice HOME_END_CHOICES[] = {
	{"off", TEXT_HOME_END_OFF}, {"before", TEXT_HOME_END_BEFORE}, {"after", TEXT_HOME_END_AFTER},
	{"always", TEXT_HOME_END_ALWAYS}, {NULL, NULL},
};

/* CSS font weights, which variable fonts draw exactly and other fonts round to their nearest style */
static const SettingChoice WEIGHT_CHOICES[] = {
	{"100", TEXT_WEIGHT_THIN}, {"200", TEXT_WEIGHT_EXTRA_LIGHT}, {"300", TEXT_WEIGHT_LIGHT},
	{"400", TEXT_WEIGHT_REGULAR}, {"500", TEXT_WEIGHT_MEDIUM}, {"600", TEXT_WEIGHT_SEMIBOLD}, {"700", TEXT_WEIGHT_BOLD},
	{"800", TEXT_WEIGHT_EXTRA_BOLD}, {"900", TEXT_WEIGHT_BLACK}, {NULL, NULL},
};

/* Larger sizes draw the minimap's blocks bigger, which also makes the minimap wider */
static const SettingChoice MINIMAP_SIZE_CHOICES[] = {
	{"small", TEXT_MINIMAP_SMALL}, {"medium", TEXT_MINIMAP_MEDIUM}, {"large", TEXT_MINIMAP_LARGE}, {NULL, NULL},
};

static const SettingChoice WRAP_CHOICES[] = {
	{"none", TEXT_WRAP_NONE}, {"word", TEXT_WRAP_WORD}, {"character", TEXT_WRAP_CHARACTER},
	{"word-character", TEXT_WRAP_WORD_CHARACTER}, {NULL, NULL},
};

static const SettingChoice SPACE_CHOICES[] = {
	{"none", TEXT_SPACES_NONE}, {"trailing", TEXT_SPACES_TRAILING}, {"all", TEXT_SPACES_ALL}, {NULL, NULL},
};

static const SettingDefinition EDITOR_SETTINGS[] = {
	{"font", "editor", TEXT_GROUP_FONT, TEXT_SETTING_FONT, TEXT_SETTING_FONT_DETAIL, SETTING_FONT, "", 0, 0, NULL,
		NULL, {"guifont", "gfn"}},
	{"font-weight", "editor", TEXT_GROUP_FONT, TEXT_SETTING_FONT_WEIGHT, TEXT_SETTING_FONT_WEIGHT_DETAIL,
		SETTING_CHOICE, "400", 0, 0, WEIGHT_CHOICES, NULL, {NULL}},
	{"text-size", "editor", TEXT_GROUP_FONT, TEXT_SETTING_TEXT_SIZE, TEXT_SETTING_TEXT_SIZE_DETAIL, SETTING_NUMBER,
		"16", 8, 48, NULL, NULL, {NULL}},
	{"line-spacing", "editor", TEXT_GROUP_FONT, TEXT_SETTING_LINE_SPACING, TEXT_SETTING_LINE_SPACING_DETAIL,
		SETTING_NUMBER, "2", 0, 16, NULL, NULL, {"linespace", "lsp"}},
	{"tab-width", "editor", TEXT_GROUP_INDENTATION, TEXT_SETTING_TAB_WIDTH, NULL, SETTING_NUMBER, "4", 1, 16, NULL,
		NULL, {"tabstop", "ts"}},
	{"insert-spaces", "editor", TEXT_GROUP_INDENTATION, TEXT_SETTING_INSERT_SPACES, NULL, SETTING_SWITCH, "false",
		0, 0, NULL, NULL, {"expandtab", "et"}},
	{"auto-indent", "editor", TEXT_GROUP_INDENTATION, TEXT_SETTING_AUTO_INDENT, TEXT_SETTING_AUTO_INDENT_DETAIL,
		SETTING_SWITCH, "true", 0, 0, NULL, NULL, {"autoindent", "ai"}},
	{"indent-on-tab", "editor", TEXT_GROUP_INDENTATION, TEXT_SETTING_INDENT_ON_TAB, TEXT_SETTING_INDENT_ON_TAB_DETAIL,
		SETTING_SWITCH, "true", 0, 0, NULL, NULL, {NULL}},
	{"smart-backspace", "editor", TEXT_GROUP_INDENTATION, TEXT_SETTING_SMART_BACKSPACE,
		TEXT_SETTING_SMART_BACKSPACE_DETAIL, SETTING_SWITCH, "true", 0, 0, NULL, NULL, {"smarttab", "sta"}},
	{"smart-home-end", "editor", TEXT_GROUP_INDENTATION, TEXT_SETTING_HOME_END, TEXT_SETTING_HOME_END_DETAIL,
		SETTING_CHOICE, "before", 0, 0, HOME_END_CHOICES, NULL, {NULL}},
	{"show-line-numbers", "editor", TEXT_GROUP_DISPLAY, TEXT_SETTING_LINE_NUMBERS, NULL, SETTING_SWITCH, "true", 0,
		0, NULL, NULL, {"number", "nu"}},
	{"show-problem-icons", "editor", TEXT_GROUP_DISPLAY, TEXT_SETTING_PROBLEM_ICONS, TEXT_SETTING_PROBLEM_ICONS_DETAIL,
		SETTING_SWITCH, "true", 0, 0, NULL, NULL, {NULL}},
	{"highlight-current-line", "editor", TEXT_GROUP_DISPLAY, TEXT_SETTING_CURRENT_LINE, NULL, SETTING_SWITCH, "true",
		0, 0, NULL, NULL, {"cursorline", "cul"}},
	{"matching-brackets", "editor", TEXT_GROUP_DISPLAY, TEXT_SETTING_BRACKETS, NULL, SETTING_SWITCH, "true", 0, 0,
		NULL, NULL, {"showmatch", "sm"}},
	{"show-minimap", "editor", TEXT_GROUP_DISPLAY, TEXT_MINIMAP, TEXT_SETTING_MINIMAP_DETAIL, SETTING_SWITCH, "true",
		0, 0, NULL, NULL, {"minimap"}},
	{"minimap-size", "editor", TEXT_GROUP_DISPLAY, TEXT_SETTING_MINIMAP_SIZE, NULL, SETTING_CHOICE, "small", 0, 0,
		MINIMAP_SIZE_CHOICES, NULL, {NULL}},
	{"wrap", "editor", TEXT_GROUP_DISPLAY, TEXT_SETTING_WRAP, NULL, SETTING_CHOICE, "none", 0, 0, WRAP_CHOICES, NULL,
		{NULL}},
	{"show-margin", "editor", TEXT_GROUP_DISPLAY, TEXT_SETTING_MARGIN, NULL, SETTING_SWITCH, "false", 0, 0, NULL,
		NULL, {NULL}},
	{"margin-column", "editor", TEXT_GROUP_DISPLAY, TEXT_SETTING_MARGIN_COLUMN, NULL, SETTING_NUMBER, "80", 0, 300,
		NULL, NULL, {"colorcolumn", "cc"}},
	{"second-margin-column", "editor", TEXT_GROUP_DISPLAY, TEXT_SETTING_SECOND_MARGIN,
		TEXT_SETTING_SECOND_MARGIN_DETAIL, SETTING_NUMBER, "120", 0, 300, NULL, NULL, {NULL}},
	{"draw-spaces", "editor", TEXT_GROUP_DISPLAY, TEXT_SETTING_SPACES, NULL, SETTING_CHOICE, "trailing", 0, 0,
		SPACE_CHOICES, NULL, {NULL}},
	{"scroll-past-end", "editor", TEXT_GROUP_DISPLAY, TEXT_SETTING_SCROLL_PAST_END, NULL, SETTING_SWITCH, "true", 0,
		0, NULL, NULL, {NULL}},
	{"cursor-blink", "editor", TEXT_GROUP_DISPLAY, TEXT_SETTING_CURSOR_BLINK, TEXT_SETTING_CURSOR_BLINK_DETAIL,
		SETTING_SWITCH, "true", 0, 0, NULL, NULL, {NULL}},
	{"trim-trailing-spaces", "editor", TEXT_GROUP_SAVING, TEXT_SETTING_TRIM, NULL, SETTING_SWITCH, "false", 0, 0,
		NULL, NULL, {NULL}},
	{"final-newline", "editor", TEXT_GROUP_SAVING, TEXT_SETTING_FINAL_NEWLINE, TEXT_SETTING_FINAL_NEWLINE_DETAIL,
		SETTING_SWITCH, "true", 0, 0, NULL, NULL, {"fixendofline", "fixeol"}},
	{"vim-mode", "vim", TEXT_GROUP_VIM, TEXT_VIM_MODE, TEXT_SETTING_VIM_DETAIL, SETTING_SWITCH, "false", 0, 0, NULL,
		NULL, {NULL}},
	{"vim-control-shortcuts", "vim", TEXT_GROUP_VIM, TEXT_SETTING_VIM_CONTROL, TEXT_SETTING_VIM_CONTROL_DETAIL,
		SETTING_SWITCH, "false", 0, 0, NULL, NULL, {NULL}},
	{"relative-numbers", "vim", TEXT_GROUP_VIM, TEXT_RELATIVE_NUMBERS, NULL, SETTING_SWITCH, "true", 0, 0, NULL,
		NULL, {"relativenumber", "rnu"}},
};

static GtkCssProvider *code_style;


/* One style sheet sets the font of each editor and terminal, so a change reaches all of them at once */
static void apply_code_style(const char *key, gpointer data) {
	if (key && g_strcmp0(key, "font") != 0 && g_strcmp0(key, "font-weight") != 0 && g_strcmp0(key, "text-size") != 0
		&& g_strcmp0(key, "cursor-blink") != 0)
		return;
	if (!code_style) {
		code_style = gtk_css_provider_new();
		gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(code_style),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	}
	const char *font = get_text_setting("font");
	g_autofree char *family = *font ? g_strdup_printf("\"%s\", monospace", font) : g_strdup("monospace");
	g_autofree char *css = g_strdup_printf(".code-text { font-family: %s; font-size: %dpx; font-weight: %s; }",
		family, get_number_setting("text-size"), get_text_setting("font-weight"));
	gtk_css_provider_load_from_string(code_style, css);
	g_object_set(gtk_settings_get_default(), "gtk-cursor-blink", get_switch_setting("cursor-blink"), NULL);
}


void register_editor_settings(void) {
	register_settings(EDITOR_SETTINGS, G_N_ELEMENTS(EDITOR_SETTINGS));
	apply_code_style(NULL, NULL);
	watch_settings(apply_code_style, NULL);
}
