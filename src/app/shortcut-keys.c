#include <string.h>

#include "shortcut-keys.h"
#include "common/widgets/spacing.h"

#define KEY_JOIN "+"


static void add_key(GtkWidget *keys, const char *name) {
	GtkWidget *cap = gtk_label_new(name);
	gtk_widget_add_css_class(cap, "keycap");
	gtk_box_append(GTK_BOX(keys), cap);
}


/* The label GTK writes, such as Shift+Ctrl+O, is split into its keys, and the key itself is read on its own since it
 * may be a + */
GtkWidget *create_shortcut_keys(const char *accelerator) {
	guint key = 0;
	GdkModifierType modifiers = 0;
	if (!accelerator || !gtk_accelerator_parse(accelerator, &key, &modifiers))
		return NULL;
	g_autofree char *label = gtk_accelerator_get_label(key, modifiers);
	g_autofree char *key_name = gtk_accelerator_get_label(key, 0);
	GtkWidget *keys = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_TIGHT);
	gtk_widget_add_css_class(keys, "shortcut-keys");
	gtk_widget_set_valign(keys, GTK_ALIGN_CENTER);
	if (!g_str_has_suffix(label, key_name)) {
		add_key(keys, label);
		return keys;
	}
	g_autofree char *modifier_text = g_strndup(label, strlen(label) - strlen(key_name));
	g_auto(GStrv) modifier_names = g_strsplit(modifier_text, KEY_JOIN, -1);
	for (guint index = 0; modifier_names[index]; index++)
		if (*modifier_names[index])
			add_key(keys, modifier_names[index]);
	add_key(keys, key_name);
	return keys;
}
