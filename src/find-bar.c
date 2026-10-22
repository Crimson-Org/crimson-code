#include "../lang/en.h"
#include "find-bar.h"

#define CONTEXT_KEY "find-context"
#define BAR_KEY "find-bar"
#define ENTRY_WIDTH_PIXELS 320
#define COUNT_WIDTH_CHARACTERS 12

struct FindBar {
	GtkWidget *bar, *entry, *count, *replace_row, *replace_entry, *replace_toggle, *replace_problem;
	GtkSourceSearchSettings *settings;
	/* The bar searches either a text view or a target such as the hex view */
	GtkSourceView *view;
	const FindTarget *target;
	gpointer target_data;
};


static void update_count(FindBar *bar);


/* The count can finish after this window closed, so it reaches the bar only while the bar's entry still exists */
static void notice_count(GtkWidget *entry) {
	update_count(g_object_get_data(G_OBJECT(entry), BAR_KEY));
}


static const char *get_query(FindBar *bar) {
	return gtk_editable_get_text(GTK_EDITABLE(bar->entry));
}


/* Each file keeps its own search context, created the first time it is searched */
static GtkSourceSearchContext *get_context(FindBar *bar) {
	if (!bar->view)
		return NULL;
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(bar->view));
	GtkSourceSearchContext *context = g_object_get_data(G_OBJECT(buffer), CONTEXT_KEY);
	if (context)
		return context;
	context = gtk_source_search_context_new(GTK_SOURCE_BUFFER(buffer), bar->settings);
	g_object_set_data_full(G_OBJECT(buffer), CONTEXT_KEY, context, g_object_unref);
	/* Large files are counted in the background, so the count fills in when it finishes */
	g_signal_connect_object(context, "notify::occurrences-count", G_CALLBACK(notice_count), bar->entry,
		G_CONNECT_SWAPPED);
	return context;
}


static void show_count(FindBar *bar, int position, int total) {
	g_autofree char *label = position > 0 ? g_strdup_printf(TEXT_FIND_POSITION, position, total)
		: g_strdup_printf(TEXT_FIND_TOTAL, total);
	gtk_label_set_text(GTK_LABEL(bar->count), label);
}


static void update_count(FindBar *bar) {
	const char *query = get_query(bar);
	if (!*query) {
		gtk_label_set_text(GTK_LABEL(bar->count), "");
		return;
	}
	if (bar->target) {
		int position = 0, total = 0;
		bar->target->count(bar->target_data, query, &position, &total);
		show_count(bar, position, total);
		return;
	}
	GtkSourceSearchContext *context = get_context(bar);
	int total = context ? gtk_source_search_context_get_occurrences_count(context) : 0;
	if (!context || total < 0) {
		gtk_label_set_text(GTK_LABEL(bar->count), "");
		return;
	}
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(bar->view));
	GtkTextIter start, end;
	gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
	show_count(bar, gtk_source_search_context_get_occurrence_position(context, &start, &end), total);
}


static void select_match(FindBar *bar, gboolean is_backward, gboolean includes_current) {
	if (bar->target) {
		if (*get_query(bar))
			bar->target->find(bar->target_data, get_query(bar), is_backward, includes_current);
		update_count(bar);
		return;
	}
	GtkSourceSearchContext *context = get_context(bar);
	if (!context)
		return;
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(bar->view));
	GtkTextIter start, end, match_start, match_end;
	gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
	gboolean has_wrapped = FALSE;
	gboolean is_found = is_backward
		? gtk_source_search_context_backward(context, &start, &match_start, &match_end, &has_wrapped)
		: gtk_source_search_context_forward(context, includes_current ? &start : &end, &match_start, &match_end,
			&has_wrapped);
	if (!is_found)
		return;
	gtk_text_buffer_select_range(buffer, &match_start, &match_end);
	gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(bar->view), gtk_text_buffer_get_insert(buffer), 0.2, FALSE, 0, 0);
	update_count(bar);
}


static void highlight_target(FindBar *bar) {
	if (!bar->target)
		return;
	gboolean is_open = gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(bar->bar));
	bar->target->highlight(bar->target_data, is_open && *get_query(bar) ? get_query(bar) : NULL);
}


static void on_search_changed(GtkSearchEntry *entry, FindBar *bar) {
	const char *text = get_query(bar);
	gtk_source_search_settings_set_search_text(bar->settings, *text ? text : NULL);
	highlight_target(bar);
	select_match(bar, FALSE, TRUE);
	update_count(bar);
}


static void on_next(FindBar *bar) {
	select_match(bar, FALSE, FALSE);
}


static void on_previous(FindBar *bar) {
	select_match(bar, TRUE, FALSE);
}


/* ==================================== Replacing ==================================== */


static void replace_text(FindBar *bar, gboolean is_all) {
	GtkSourceSearchContext *context = get_context(bar);
	if (!context || !*get_query(bar))
		return;
	const char *replacement = gtk_editable_get_text(GTK_EDITABLE(bar->replace_entry));
	if (is_all) {
		gtk_source_search_context_replace_all(context, replacement, -1, NULL);
		return;
	}
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(bar->view));
	GtkTextIter start, end;
	gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
	/* A selection that is not a match is first moved to the next match, which the next press replaces */
	if (gtk_source_search_context_get_occurrence_position(context, &start, &end) <= 0) {
		select_match(bar, FALSE, TRUE);
		return;
	}
	gtk_source_search_context_replace(context, &start, &end, replacement, -1, NULL);
	select_match(bar, FALSE, FALSE);
}


static void replace(FindBar *bar, gboolean is_all) {
	if (!bar->target) {
		replace_text(bar, is_all);
		update_count(bar);
		return;
	}
	g_autofree char *message = NULL;
	int count = bar->target->replace(bar->target_data, get_query(bar),
		gtk_editable_get_text(GTK_EDITABLE(bar->replace_entry)), is_all, &message);
	gtk_label_set_text(GTK_LABEL(bar->replace_problem), count < 0 ? message : "");
	if (count < 0)
		return;
	if (!is_all)
		select_match(bar, FALSE, FALSE);
	update_count(bar);
}


static void on_replace(FindBar *bar) {
	replace(bar, FALSE);
}


static void on_replace_all(FindBar *bar) {
	replace(bar, TRUE);
}


static void set_replace_visible(FindBar *bar, gboolean is_visible) {
	gtk_revealer_set_reveal_child(GTK_REVEALER(bar->replace_row), is_visible);
	gtk_button_set_icon_name(GTK_BUTTON(bar->replace_toggle), is_visible ? "pan-down-symbolic" : "pan-end-symbolic");
}


static void on_replace_toggled(FindBar *bar) {
	set_replace_visible(bar, !gtk_revealer_get_reveal_child(GTK_REVEALER(bar->replace_row)));
}


/* ==================================== Opening and closing ==================================== */


static void focus_searched(FindBar *bar) {
	if (bar->target)
		bar->target->focus(bar->target_data);
	else if (bar->view)
		gtk_widget_grab_focus(GTK_WIDGET(bar->view));
}


static void on_stop(FindBar *bar) {
	gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(bar->bar), FALSE);
	focus_searched(bar);
}


static gboolean on_entry_key(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType state,
	FindBar *bar) {
	if ((key != GDK_KEY_Return && key != GDK_KEY_KP_Enter) || !(state & GDK_SHIFT_MASK))
		return FALSE;
	on_previous(bar);
	return TRUE;
}


static void on_mode_changed(GObject *search_bar, GParamSpec *property, FindBar *bar) {
	gboolean is_open = gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(bar->bar));
	GtkSourceSearchContext *context = get_context(bar);
	if (context)
		gtk_source_search_context_set_highlight(context, is_open);
	highlight_target(bar);
	if (!is_open)
		set_replace_visible(bar, FALSE);
}


static GtkWidget *create_button(const char *icon, const char *label, const char *tooltip, GCallback callback,
	FindBar *bar) {
	GtkWidget *button = icon ? gtk_button_new_from_icon_name(icon) : gtk_button_new_with_label(label);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_add_css_class(button, "flat");
	g_signal_connect_swapped(button, "clicked", callback, bar);
	return button;
}


static GtkWidget *create_replace_row(FindBar *bar) {
	bar->replace_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(bar->replace_entry), TEXT_REPLACE_WITH);
	gtk_widget_set_size_request(bar->replace_entry, ENTRY_WIDTH_PIXELS, -1);
	g_signal_connect_swapped(bar->replace_entry, "activate", G_CALLBACK(on_replace), bar);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_top(box, 6);
	gtk_box_append(GTK_BOX(box), bar->replace_entry);
	gtk_box_append(GTK_BOX(box), create_button(NULL, TEXT_REPLACE, TEXT_REPLACE, G_CALLBACK(on_replace), bar));
	gtk_box_append(GTK_BOX(box), create_button(NULL, TEXT_REPLACE_ALL, TEXT_REPLACE_ALL, G_CALLBACK(on_replace_all),
		bar));
	bar->replace_problem = gtk_label_new(NULL);
	gtk_widget_add_css_class(bar->replace_problem, "error");
	gtk_box_append(GTK_BOX(box), bar->replace_problem);
	bar->replace_row = gtk_revealer_new();
	gtk_revealer_set_child(GTK_REVEALER(bar->replace_row), box);
	return bar->replace_row;
}


FindBar *create_find_bar(void) {
	FindBar *bar = g_new0(FindBar, 1);
	bar->settings = gtk_source_search_settings_new();
	gtk_source_search_settings_set_wrap_around(bar->settings, TRUE);
	bar->entry = gtk_search_entry_new();
	g_object_set_data(G_OBJECT(bar->entry), BAR_KEY, bar);
	gtk_widget_set_size_request(bar->entry, ENTRY_WIDTH_PIXELS, -1);
	bar->count = gtk_label_new("");
	gtk_widget_add_css_class(bar->count, "dim-label");
	/* A steady width keeps the buttons in place while the count changes */
	gtk_label_set_width_chars(GTK_LABEL(bar->count), COUNT_WIDTH_CHARACTERS);
	gtk_label_set_xalign(GTK_LABEL(bar->count), 0);
	bar->replace_toggle = create_button("pan-end-symbolic", NULL, TEXT_REPLACE, G_CALLBACK(on_replace_toggled), bar);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(box), bar->replace_toggle);
	gtk_box_append(GTK_BOX(box), bar->entry);
	gtk_box_append(GTK_BOX(box), bar->count);
	gtk_box_append(GTK_BOX(box), create_button("go-up-symbolic", NULL, TEXT_FIND_PREVIOUS, G_CALLBACK(on_previous),
		bar));
	gtk_box_append(GTK_BOX(box), create_button("go-down-symbolic", NULL, TEXT_FIND_NEXT, G_CALLBACK(on_next), bar));
	/* The replace row lines up under the search entry, past the toggle */
	GtkWidget *replace_row = create_replace_row(bar);
	gtk_widget_set_margin_start(replace_row, 40);
	GtkWidget *rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(rows), box);
	gtk_box_append(GTK_BOX(rows), replace_row);
	bar->bar = gtk_search_bar_new();
	gtk_search_bar_set_child(GTK_SEARCH_BAR(bar->bar), rows);
	gtk_search_bar_connect_entry(GTK_SEARCH_BAR(bar->bar), GTK_EDITABLE(bar->entry));
	gtk_search_bar_set_show_close_button(GTK_SEARCH_BAR(bar->bar), TRUE);
	g_signal_connect(bar->entry, "search-changed", G_CALLBACK(on_search_changed), bar);
	g_signal_connect_swapped(bar->entry, "activate", G_CALLBACK(on_next), bar);
	g_signal_connect_swapped(bar->entry, "next-match", G_CALLBACK(on_next), bar);
	g_signal_connect_swapped(bar->entry, "previous-match", G_CALLBACK(on_previous), bar);
	g_signal_connect_swapped(bar->entry, "stop-search", G_CALLBACK(on_stop), bar);
	g_signal_connect(bar->bar, "notify::search-mode-enabled", G_CALLBACK(on_mode_changed), bar);
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_entry_key), bar);
	gtk_widget_add_controller(bar->entry, keys);
	return bar;
}


GtkWidget *get_find_bar_widget(FindBar *bar) {
	return bar->bar;
}


static void forget_searched(FindBar *bar) {
	GtkSourceSearchContext *previous = get_context(bar);
	if (previous)
		gtk_source_search_context_set_highlight(previous, FALSE);
	if (bar->target)
		bar->target->highlight(bar->target_data, NULL);
	if (bar->view)
		g_object_remove_weak_pointer(G_OBJECT(bar->view), (gpointer *)&bar->view);
	bar->view = NULL;
	bar->target = NULL;
	bar->target_data = NULL;
}


void set_find_bar_view(FindBar *bar, GtkSourceView *view) {
	forget_searched(bar);
	bar->view = view;
	/* A view closed with its window leaves the bar searching nothing rather than a freed view */
	if (view)
		g_object_add_weak_pointer(G_OBJECT(view), (gpointer *)&bar->view);
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(bar->entry), NULL);
	on_mode_changed(NULL, NULL, bar);
	update_count(bar);
}


void set_find_bar_target(FindBar *bar, const FindTarget *target, gpointer data) {
	forget_searched(bar);
	bar->target = target;
	bar->target_data = data;
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(bar->entry), target->placeholder);
	on_mode_changed(NULL, NULL, bar);
	update_count(bar);
}


void show_find_bar(FindBar *bar) {
	if (!bar->view && !bar->target)
		return;
	if (bar->view) {
		GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(bar->view));
		GtkTextIter start, end;
		if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end) && gtk_text_iter_get_line(&start)
			== gtk_text_iter_get_line(&end)) {
			g_autofree char *selected = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
			gtk_editable_set_text(GTK_EDITABLE(bar->entry), selected);
		}
	}
	gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(bar->bar), TRUE);
	gtk_widget_grab_focus(bar->entry);
	gtk_editable_select_region(GTK_EDITABLE(bar->entry), 0, -1);
}


void show_replace_bar(FindBar *bar) {
	show_find_bar(bar);
	set_replace_visible(bar, TRUE);
}
