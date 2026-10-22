#include "../lang/en.h"
#include "task-menu.h"
#include "build/tasks.h"

#define MENU_WIDTH 320

/* The button, its lists and where chosen tasks go */
typedef struct {
	GtkWidget *button, *list, *actions, *separator;
	char *project;
	TaskChosenFunction on_chosen;
	gpointer data;
} TaskMenu;


static GtkWidget *create_row(const char *icon, const char *title, const char *detail, const char *task_name) {
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	GtkWidget *label = gtk_label_new(title);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_box_append(GTK_BOX(text), label);
	if (detail && *detail) {
		GtkWidget *detail_label = gtk_label_new(detail);
		gtk_label_set_xalign(GTK_LABEL(detail_label), 0);
		gtk_label_set_wrap(GTK_LABEL(detail_label), TRUE);
		gtk_widget_add_css_class(detail_label, "dim-label");
		gtk_box_append(GTK_BOX(text), detail_label);
	}
	GtkWidget *image = gtk_image_new_from_icon_name(icon);
	gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_box_append(GTK_BOX(box), image);
	gtk_box_append(GTK_BOX(box), text);
	GtkWidget *row = gtk_list_box_row_new();
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	g_object_set_data_full(G_OBJECT(row), "task", g_strdup(task_name), g_free);
	return row;
}


static void append_row(GtkWidget *list, GtkWidget *row, gboolean is_activatable) {
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), is_activatable);
	gtk_list_box_append(GTK_LIST_BOX(list), row);
}


/* Tasks come first with their descriptions, and editing the task file sits apart below a separator */
static void fill_menu(TaskMenu *menu) {
	gtk_list_box_remove_all(GTK_LIST_BOX(menu->list));
	gtk_list_box_remove_all(GTK_LIST_BOX(menu->actions));
	if (!menu->project) {
		append_row(menu->list, create_row("folder-symbolic", TEXT_NO_PROJECT, NULL, NULL), FALSE);
		gtk_widget_set_visible(menu->separator, FALSE);
		return;
	}
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) tasks = read_task_list(menu->project, &error);
	for (guint index = 0; tasks && index < tasks->len; index++) {
		TaskInfo *task = g_ptr_array_index(tasks, index);
		append_row(menu->list, create_row("media-playback-start-symbolic", task->name, task->description, task->name),
			TRUE);
	}
	gboolean has_file = !error || !g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
	if (error && has_file)
		append_row(menu->list, create_row("dialog-warning-symbolic", TEXT_TASK_FILE_BROKEN, error->message, NULL),
			FALSE);
	gboolean has_rows = gtk_widget_get_first_child(menu->list) != NULL;
	gtk_widget_set_visible(menu->list, has_rows);
	gtk_widget_set_visible(menu->separator, has_rows);
	append_row(menu->actions, create_row(has_file ? "document-edit-symbolic" : "list-add-symbolic",
		has_file ? TEXT_EDIT_TASKS : TEXT_CREATE_TASKS, NULL, NULL), TRUE);
}


static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, TaskMenu *menu) {
	g_autofree char *name = g_strdup(g_object_get_data(G_OBJECT(row), "task"));
	gtk_menu_button_popdown(GTK_MENU_BUTTON(menu->button));
	menu->on_chosen(name, menu->data);
}


static void free_menu(TaskMenu *menu) {
	g_free(menu->project);
	g_free(menu);
}


GtkWidget *create_task_button(TaskChosenFunction on_chosen, gpointer data) {
	TaskMenu *menu = g_new0(TaskMenu, 1);
	menu->on_chosen = on_chosen;
	menu->data = data;
	menu->list = gtk_list_box_new();
	menu->actions = gtk_list_box_new();
	GtkWidget *lists[] = {menu->list, menu->actions};
	for (guint index = 0; index < G_N_ELEMENTS(lists); index++) {
		gtk_list_box_set_selection_mode(GTK_LIST_BOX(lists[index]), GTK_SELECTION_NONE);
		gtk_widget_add_css_class(lists[index], "task-list");
		g_signal_connect(lists[index], "row-activated", G_CALLBACK(on_row_activated), menu);
	}
	menu->separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_add_css_class(box, "task-menu");
	gtk_widget_set_size_request(box, MENU_WIDTH, -1);
	gtk_box_append(GTK_BOX(box), menu->list);
	gtk_box_append(GTK_BOX(box), menu->separator);
	gtk_box_append(GTK_BOX(box), menu->actions);
	GtkWidget *popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(popover), box);
	g_signal_connect_swapped(popover, "show", G_CALLBACK(fill_menu), menu);
	menu->button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu->button), "media-playback-start-symbolic");
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu->button), popover);
	gtk_widget_set_tooltip_text(menu->button, TEXT_TASKS);
	g_object_set_data_full(G_OBJECT(menu->button), "task-menu", menu, (GDestroyNotify)free_menu);
	return menu->button;
}


void set_task_button_project(GtkWidget *button, const char *project) {
	TaskMenu *menu = g_object_get_data(G_OBJECT(button), "task-menu");
	g_set_str(&menu->project, project);
}


void show_task_menu(GtkWidget *button) {
	gtk_menu_button_popup(GTK_MENU_BUTTON(button));
}
