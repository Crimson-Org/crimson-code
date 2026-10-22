#include <string.h>

#include "../lang/en.h"
#include "settings.h"
#include "window-private.h"

#define EDITOR_KEY "editor"
#define PANE_KEY "pane"
#define MODIFIED_MARK "• "

/* One tile of the editor area with its own tabs, which splits into more tiles like Vim windows */
struct Pane {
	Window *window;
	GtkWidget *root;
	AdwTabView *tabs;
	AdwTabBar *bar;
};

/* Directions for moving between panes, as unit steps on the screen */
static const struct {
	int x, y;
} DIRECTIONS[] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};


/* ==================================== Pages ==================================== */


static Editor *get_page_editor(AdwTabPage *page) {
	return page ? g_object_get_data(G_OBJECT(adw_tab_page_get_child(page)), EDITOR_KEY) : NULL;
}


static Pane *get_editor_pane(Editor *editor) {
	return g_object_get_data(G_OBJECT(get_editor_widget(editor)), PANE_KEY);
}


static AdwTabPage *find_editor_page(Editor *editor) {
	Pane *pane = get_editor_pane(editor);
	if (!pane)
		return NULL;
	for (int index = 0; index < adw_tab_view_get_n_pages(pane->tabs); index++) {
		AdwTabPage *page = adw_tab_view_get_nth_page(pane->tabs, index);
		if (get_page_editor(page) == editor)
			return page;
	}
	return NULL;
}


/* None once closing has freed the editors, though their tabs may linger a moment */
Editor *get_current_editor(Window *window) {
	if (!window->current_pane || window->is_closed)
		return NULL;
	return get_page_editor(adw_tab_view_get_selected_page(window->current_pane->tabs));
}


GPtrArray *list_window_editors(Window *window) {
	GPtrArray *editors = g_ptr_array_new();
	for (guint pane_index = 0; pane_index < window->panes->len; pane_index++) {
		Pane *pane = g_ptr_array_index(window->panes, pane_index);
		for (int index = 0; index < adw_tab_view_get_n_pages(pane->tabs); index++)
			g_ptr_array_add(editors, get_page_editor(adw_tab_view_get_nth_page(pane->tabs, index)));
	}
	return editors;
}


static int count_pages(Window *window) {
	int count = 0;
	for (guint index = 0; index < window->panes->len; index++)
		count += adw_tab_view_get_n_pages(((Pane *)g_ptr_array_index(window->panes, index))->tabs);
	return count;
}


void update_window_tab(Window *window, Editor *editor) {
	AdwTabPage *page = find_editor_page(editor);
	if (!page)
		return;
	g_autofree char *name = get_editor_title(editor);
	g_autofree char *title = is_editor_modified(editor) ? g_strconcat(MODIFIED_MARK, name, NULL) : g_strdup(name);
	adw_tab_page_set_title(page, title);
	/* The type is guessed from the name alone, so a renamed or untitled file gets its icon without reading it */
	g_autofree char *type = g_content_type_guess(name, NULL, 0, NULL);
	g_autoptr(GIcon) icon = g_content_type_get_icon(type);
	adw_tab_page_set_icon(page, icon);
	GFile *file = get_editor_file(editor);
	g_autofree char *path = file ? g_file_get_path(file) : NULL;
	adw_tab_page_set_tooltip(page, path ? path : name);
}


static void show_editor(Editor *editor) {
	Pane *pane = get_editor_pane(editor);
	AdwTabPage *page = find_editor_page(editor);
	if (page)
		adw_tab_view_set_selected_page(pane->tabs, page);
	gtk_widget_grab_focus(GTK_WIDGET(get_editor_view(editor)));
}


/* ==================================== Focus ==================================== */


static void focus_pane(Window *window, Pane *pane) {
	if (window->current_pane == pane)
		return;
	window->current_pane = pane;
	for (guint index = 0; index < window->panes->len; index++) {
		Pane *other = g_ptr_array_index(window->panes, index);
		if (other == pane)
			gtk_widget_add_css_class(other->root, "current");
		else
			gtk_widget_remove_css_class(other->root, "current");
	}
	refresh_window_for_editor(window);
}


static void on_pane_focused(GtkEventControllerFocus *focus, Pane *pane) {
	focus_pane(pane->window, pane);
}


static void on_page_selected(AdwTabView *tabs, GParamSpec *property, Pane *pane) {
	if (pane->window->current_pane == pane)
		refresh_window_for_editor(pane->window);
}


/* ==================================== Pane tree ==================================== */


/* Puts a widget where another one was, whether that place is a split or the top of the editor area */
static void replace_widget(Window *window, GtkWidget *old, GtkWidget *replacement) {
	GtkWidget *parent = gtk_widget_get_parent(old);
	if (GTK_IS_PANED(parent)) {
		gboolean is_start = gtk_paned_get_start_child(GTK_PANED(parent)) == old;
		if (is_start)
			gtk_paned_set_start_child(GTK_PANED(parent), replacement);
		else
			gtk_paned_set_end_child(GTK_PANED(parent), replacement);
		return;
	}
	gtk_box_remove(GTK_BOX(window->pane_area), old);
	gtk_box_append(GTK_BOX(window->pane_area), replacement);
}


/* Disconnecting happens before a pane's widgets go, since by the time the list is freed GTK has destroyed them */
static void free_pane(Pane *pane) {
	g_free(pane);
}


/* An emptied pane gives its space to its neighbour, as closing a Vim window does */
static void remove_pane(Window *window, Pane *pane) {
	if (window->panes->len <= 1)
		return;
	g_signal_handlers_disconnect_by_data(pane->tabs, pane);
	GtkWidget *split = gtk_widget_get_parent(pane->root);
	GtkWidget *start = gtk_paned_get_start_child(GTK_PANED(split));
	GtkWidget *sibling = start == pane->root ? gtk_paned_get_end_child(GTK_PANED(split)) : start;
	g_object_ref(sibling);
	gtk_paned_set_start_child(GTK_PANED(split), NULL);
	gtk_paned_set_end_child(GTK_PANED(split), NULL);
	replace_widget(window, split, sibling);
	g_object_unref(sibling);
	if (window->current_pane == pane)
		window->current_pane = NULL;
	g_ptr_array_remove(window->panes, pane);
	Pane *next = g_ptr_array_index(window->panes, 0);
	focus_pane(window, next);
	Editor *editor = get_current_editor(window);
	if (editor)
		gtk_widget_grab_focus(GTK_WIDGET(get_editor_view(editor)));
}


static gboolean on_close_page(AdwTabView *tabs, AdwTabPage *page, Pane *pane);


static void on_page_attached(AdwTabView *tabs, AdwTabPage *page, int position, Pane *pane) {
	g_object_set_data(G_OBJECT(adw_tab_page_get_child(page)), PANE_KEY, pane);
}


/* Runs after the closing tab view has finished its own work, since removing it from inside its signal is unsafe */
static gboolean remove_empty_panes(gpointer data) {
	Window *window = g_object_get_data(G_OBJECT(data), "window");
	for (guint index = window && window->panes ? window->panes->len : 0; index > 0; index--) {
		Pane *pane = g_ptr_array_index(window->panes, index - 1);
		if (adw_tab_view_get_n_pages(pane->tabs) == 0 && window->panes->len > 1)
			remove_pane(window, pane);
	}
	if (window && window->panes)
		refresh_window_for_editor(window);
	return G_SOURCE_REMOVE;
}


static void on_page_count_changed(AdwTabView *tabs, GParamSpec *property, Pane *pane) {
	Window *window = pane->window;
	gtk_stack_set_visible_child_name(GTK_STACK(window->editor_stack), count_pages(window) > 0 ? "editors" : "empty");
	update_window_empty_page(window);
	if (adw_tab_view_get_n_pages(tabs) == 0 && window->panes->len > 1)
		g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, remove_empty_panes, g_object_ref(window->window), g_object_unref);
	refresh_window_for_editor(window);
}


static Pane *create_pane(Window *window) {
	Pane *pane = g_new0(Pane, 1);
	pane->window = window;
	pane->tabs = adw_tab_view_new();
	gtk_widget_set_vexpand(GTK_WIDGET(pane->tabs), TRUE);
	pane->bar = adw_tab_bar_new();
	adw_tab_bar_set_view(pane->bar, pane->tabs);
	adw_tab_bar_set_autohide(pane->bar, g_strcmp0(get_text_setting("tab-bar"), "always") != 0);
	pane->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(pane->root, "pane");
	gtk_box_append(GTK_BOX(pane->root), GTK_WIDGET(pane->bar));
	gtk_box_append(GTK_BOX(pane->root), GTK_WIDGET(pane->tabs));
	g_signal_connect(pane->tabs, "close-page", G_CALLBACK(on_close_page), pane);
	g_signal_connect(pane->tabs, "page-attached", G_CALLBACK(on_page_attached), pane);
	g_signal_connect(pane->tabs, "notify::n-pages", G_CALLBACK(on_page_count_changed), pane);
	g_signal_connect(pane->tabs, "notify::selected-page", G_CALLBACK(on_page_selected), pane);
	GtkEventController *focus = gtk_event_controller_focus_new();
	g_signal_connect(focus, "enter", G_CALLBACK(on_pane_focused), pane);
	gtk_widget_add_controller(pane->root, focus);
	g_ptr_array_add(window->panes, pane);
	return pane;
}


GtkWidget *create_window_panes(Window *window) {
	window->panes = g_ptr_array_new_with_free_func((GDestroyNotify)free_pane);
	window->closed_files = g_ptr_array_new_with_free_func(g_object_unref);
	window->pane_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_vexpand(window->pane_area, TRUE);
	Pane *pane = create_pane(window);
	gtk_box_append(GTK_BOX(window->pane_area), pane->root);
	window->current_pane = pane;
	return window->pane_area;
}


void apply_window_pane_settings(Window *window) {
	gboolean is_automatic = g_strcmp0(get_text_setting("tab-bar"), "always") != 0;
	for (guint index = 0; index < window->panes->len; index++)
		adw_tab_bar_set_autohide(((Pane *)g_ptr_array_index(window->panes, index))->bar, is_automatic);
}


/* ==================================== Editors in panes ==================================== */


static void place_editor(Pane *pane, Editor *editor) {
	GtkWidget *widget = get_editor_widget(editor);
	g_object_set_data(G_OBJECT(widget), EDITOR_KEY, editor);
	g_object_set_data(G_OBJECT(widget), PANE_KEY, pane);
	AdwTabPage *page = adw_tab_view_append(pane->tabs, widget);
	adw_tab_view_set_selected_page(pane->tabs, page);
	update_window_tab(pane->window, editor);
	gtk_widget_grab_focus(GTK_WIDGET(get_editor_view(editor)));
}


Editor *add_window_editor(Window *window) {
	EditorEvents events;
	get_window_editor_events(window, &events);
	Editor *editor = create_editor(&events);
	place_editor(window->current_pane, editor);
	return editor;
}


static Editor *add_editor_view(Pane *pane, Editor *source) {
	EditorEvents events;
	get_window_editor_events(pane->window, &events);
	Editor *editor = create_editor_view(source, &events);
	place_editor(pane, editor);
	return editor;
}


static Editor *find_open_file(Window *window, GFile *file, Pane *only_pane) {
	g_autoptr(GPtrArray) editors = list_window_editors(window);
	for (guint index = 0; index < editors->len; index++) {
		Editor *editor = g_ptr_array_index(editors, index);
		GFile *open = get_editor_file(editor);
		if (open && g_file_equal(open, file) && (!only_pane || get_editor_pane(editor) == only_pane))
			return editor;
	}
	return NULL;
}


/* Files open in the current pane, showing a file that is open in another pane as a second view of it */
Editor *open_window_file(Window *window, GFile *file) {
	Editor *here = find_open_file(window, file, window->current_pane);
	if (here) {
		show_editor(here);
		return here;
	}
	Editor *elsewhere = find_open_file(window, file, NULL);
	if (elsewhere)
		return add_editor_view(window->current_pane, elsewhere);
	Editor *editor = add_window_editor(window);
	load_editor_file(editor, file);
	return editor;
}


Editor *find_window_file(Window *window, GFile *file) {
	return find_open_file(window, file, NULL);
}


Editor *show_window_file(Window *window, GFile *file) {
	Editor *editor = find_open_file(window, file, window->current_pane);
	if (!editor)
		editor = find_open_file(window, file, NULL);
	if (!editor)
		return open_window_file(window, file);
	show_editor(editor);
	return editor;
}


/* ==================================== Closing ==================================== */


static void remember_closed_file(Editor *editor) {
	GFile *file = get_editor_file(editor);
	if (!file || count_editor_views(editor) > 1)
		return;
	GPtrArray *closed = get_editor_pane(editor)->window->closed_files;
	for (guint index = 0; index < closed->len; index++)
		if (g_file_equal(g_ptr_array_index(closed, index), file)) {
			g_ptr_array_remove_index(closed, index);
			break;
		}
	g_ptr_array_add(closed, g_object_ref(file));
}


void reopen_window_file(Window *window) {
	if (window->closed_files->len == 0)
		return;
	g_autoptr(GFile) file = g_ptr_array_steal_index(window->closed_files, window->closed_files->len - 1);
	open_window_file(window, file);
}


static void finish_close(AdwTabView *tabs, AdwTabPage *page, gboolean is_closing) {
	Editor *editor = get_page_editor(page);
	if (is_closing)
		remember_closed_file(editor);
	adw_tab_view_close_page_finish(tabs, page, is_closing);
	if (is_closing)
		free_editor(editor);
}


static void on_close_answered(GObject *dialog, GAsyncResult *result, gpointer data) {
	AdwTabPage *page = data;
	Pane *pane = g_object_get_data(G_OBJECT(dialog), PANE_KEY);
	const char *answer = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), result);
	/* A save waiting on the missing folder question keeps the file open */
	gboolean is_closing = strcmp(answer, "cancel") != 0;
	if (strcmp(answer, "save") == 0)
		is_closing = save_editor(get_page_editor(page), NULL);
	finish_close(pane->tabs, page, is_closing);
	g_object_unref(page);
}


/* Unsaved text asks first, unless another view still shows it, and closing waits as AdwTabView expects */
static gboolean on_close_page(AdwTabView *tabs, AdwTabPage *page, Pane *pane) {
	Editor *editor = get_page_editor(page);
	gboolean is_forced = GPOINTER_TO_INT(g_object_steal_data(G_OBJECT(page), "forced"));
	if (!is_editor_modified(editor) || is_forced || count_editor_views(editor) > 1) {
		finish_close(tabs, page, TRUE);
		return GDK_EVENT_STOP;
	}
	g_autofree char *name = get_editor_title(editor);
	AdwDialog *dialog = adw_alert_dialog_new(TEXT_SAVE_QUESTION, NULL);
	adw_alert_dialog_format_body(ADW_ALERT_DIALOG(dialog), TEXT_SAVE_QUESTION_DETAIL, name);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", TEXT_CANCEL, "discard", TEXT_DISCARD,
		"save", TEXT_SAVE, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "discard", ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "save", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(dialog), "save", get_editor_file(editor) != NULL);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "save");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	g_object_set_data(G_OBJECT(dialog), PANE_KEY, pane);
	adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), pane->window->window, NULL, on_close_answered,
		g_object_ref(page));
	return GDK_EVENT_STOP;
}


void close_window_editor(Window *window, Editor *editor, gboolean is_forced) {
	AdwTabPage *page = find_editor_page(editor);
	if (!page)
		return;
	g_object_set_data(G_OBJECT(page), "forced", GINT_TO_POINTER(is_forced));
	adw_tab_view_close_page(get_editor_pane(editor)->tabs, page);
}


static void close_pane_pages(Pane *pane) {
	for (int index = adw_tab_view_get_n_pages(pane->tabs) - 1; index >= 0; index--)
		adw_tab_view_close_page(pane->tabs, adw_tab_view_get_nth_page(pane->tabs, index));
}


void close_window_pane(Window *window) {
	if (window->panes->len > 1 && window->current_pane)
		close_pane_pages(window->current_pane);
}


void close_other_window_panes(Window *window) {
	g_autoptr(GPtrArray) others = g_ptr_array_new();
	for (guint index = 0; index < window->panes->len; index++)
		if (g_ptr_array_index(window->panes, index) != window->current_pane)
			g_ptr_array_add(others, g_ptr_array_index(window->panes, index));
	for (guint index = 0; index < others->len; index++)
		close_pane_pages(g_ptr_array_index(others, index));
}


/* Stops the panes calling back while the window closes and the editors are freed */
void disconnect_window_panes(Window *window) {
	for (guint index = 0; index < window->panes->len; index++)
		g_signal_handlers_disconnect_by_data(((Pane *)g_ptr_array_index(window->panes, index))->tabs,
			g_ptr_array_index(window->panes, index));
}


guint count_window_panes(Window *window) {
	return window->panes->len;
}


/* ==================================== Splitting and moving ==================================== */


/* Splits the current pane in two and shows the current file in the new half, as :vsplit and :split do */
void split_window_pane(Window *window, GtkOrientation orientation) {
	Pane *current = window->current_pane;
	Editor *editor = get_current_editor(window);
	int size = orientation == GTK_ORIENTATION_HORIZONTAL ? gtk_widget_get_width(current->root)
		: gtk_widget_get_height(current->root);
	GtkWidget *split = gtk_paned_new(orientation);
	gtk_paned_set_wide_handle(GTK_PANED(split), FALSE);
	g_object_ref(current->root);
	replace_widget(window, current->root, split);
	gtk_paned_set_start_child(GTK_PANED(split), current->root);
	g_object_unref(current->root);
	Pane *pane = create_pane(window);
	gtk_paned_set_end_child(GTK_PANED(split), pane->root);
	gtk_paned_set_position(GTK_PANED(split), size / 2);
	focus_pane(window, pane);
	if (editor)
		add_editor_view(pane, editor);
}


static void get_pane_centre(Pane *pane, GtkWidget *window, double *x, double *y) {
	graphene_rect_t bounds;
	if (!gtk_widget_compute_bounds(pane->root, window, &bounds)) {
		*x = *y = 0;
		return;
	}
	*x = bounds.origin.x + bounds.size.width / 2;
	*y = bounds.origin.y + bounds.size.height / 2;
}


/* Picks the nearest pane whose centre lies in the direction, as Ctrl+W with h, j, k or l does in Vim */
void move_to_window_pane(Window *window, PaneDirection direction) {
	Pane *current = window->current_pane, *best = NULL;
	double from_x, from_y, best_distance = G_MAXDOUBLE;
	get_pane_centre(current, window->window, &from_x, &from_y);
	for (guint index = 0; index < window->panes->len; index++) {
		Pane *pane = g_ptr_array_index(window->panes, index);
		double x, y;
		get_pane_centre(pane, window->window, &x, &y);
		double along = (x - from_x) * DIRECTIONS[direction].x + (y - from_y) * DIRECTIONS[direction].y;
		double distance = (x - from_x) * (x - from_x) + (y - from_y) * (y - from_y);
		if (pane != current && along > 0 && distance < best_distance) {
			best = pane;
			best_distance = distance;
		}
	}
	if (!best)
		return;
	focus_pane(window, best);
	Editor *editor = get_current_editor(window);
	gtk_widget_grab_focus(editor ? GTK_WIDGET(get_editor_view(editor)) : GTK_WIDGET(best->tabs));
}


void move_to_next_window_pane(Window *window) {
	guint position = 0;
	g_ptr_array_find(window->panes, window->current_pane, &position);
	Pane *next = g_ptr_array_index(window->panes, (position + 1) % window->panes->len);
	focus_pane(window, next);
	Editor *editor = get_current_editor(window);
	gtk_widget_grab_focus(editor ? GTK_WIDGET(get_editor_view(editor)) : GTK_WIDGET(next->tabs));
}


void select_next_window_tab(Window *window, gboolean is_forward) {
	if (is_forward)
		adw_tab_view_select_next_page(window->current_pane->tabs);
	else
		adw_tab_view_select_previous_page(window->current_pane->tabs);
}
