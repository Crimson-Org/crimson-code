#include <adwaita.h>

#include "../lang/en.h"
#include "terminal-tabs.h"
#include "terminal.h"

/* The tabs and who hears when they run out */
typedef struct {
	AdwTabView *view;
	TerminalsEmptiedFunction on_emptied;
	gpointer data;
	/* Counts up so that each new terminal gets a name no other open one has */
	guint opened;
} TerminalTabs;


static void on_page_count_changed(AdwTabView *view, GParamSpec *property, TerminalTabs *tabs) {
	if (adw_tab_view_get_n_pages(view) == 0) {
		tabs->opened = 0;
		tabs->on_emptied(tabs->data);
	}
}


GtkWidget *create_terminal_tabs(TerminalsEmptiedFunction on_emptied, gpointer data) {
	TerminalTabs *tabs = g_new0(TerminalTabs, 1);
	tabs->on_emptied = on_emptied;
	tabs->data = data;
	tabs->view = adw_tab_view_new();
	gtk_widget_set_vexpand(GTK_WIDGET(tabs->view), TRUE);
	g_signal_connect(tabs->view, "notify::n-pages", G_CALLBACK(on_page_count_changed), tabs);
	AdwTabBar *bar = adw_tab_bar_new();
	adw_tab_bar_set_view(bar, tabs->view);
	adw_tab_bar_set_autohide(bar, TRUE);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(box, "terminal-tabs");
	gtk_box_append(GTK_BOX(box), GTK_WIDGET(bar));
	gtk_box_append(GTK_BOX(box), GTK_WIDGET(tabs->view));
	g_object_set_data_full(G_OBJECT(box), "terminal-tabs", tabs, g_free);
	return box;
}


GtkWidget *add_terminal_tab(GtkWidget *tabs_widget, const char *directory) {
	TerminalTabs *tabs = g_object_get_data(G_OBJECT(tabs_widget), "terminal-tabs");
	GtkWidget *terminal = create_terminal();
	AdwTabPage *page = adw_tab_view_append(tabs->view, terminal);
	g_autofree char *title = ++tabs->opened == 1 ? g_strdup(TEXT_TERMINAL)
		: g_strdup_printf(TEXT_TERMINAL_NUMBERED, tabs->opened);
	adw_tab_page_set_title(page, title);
	adw_tab_view_set_selected_page(tabs->view, page);
	start_terminal(terminal, directory);
	return terminal;
}


GtkWidget *get_current_terminal(GtkWidget *tabs_widget, const char *directory) {
	TerminalTabs *tabs = g_object_get_data(G_OBJECT(tabs_widget), "terminal-tabs");
	AdwTabPage *page = adw_tab_view_get_selected_page(tabs->view);
	return page ? adw_tab_page_get_child(page) : add_terminal_tab(tabs_widget, directory);
}
