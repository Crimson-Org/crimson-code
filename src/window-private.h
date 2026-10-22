#pragma once

#include <adwaita.h>

#include "commands.h"
#include "editor.h"
#include "find-bar.h"
#include "lsp.h"
#include "panel-slide.h"
#include "state.h"
#include "window.h"

typedef struct Pane Pane;

/* Ways to move between split panes */
typedef enum {
	PANE_LEFT,
	PANE_RIGHT,
	PANE_UP,
	PANE_DOWN,
} PaneDirection;

struct Window {
	/* The sidebar stack shows the project files or the plan */
	GtkWidget *sidebar_stack, *plan, *plan_button;
	GtkWidget *window, *toasts, *title, *sidebar, *sidebar_paned, *terminal_paned, *terminals, *task_button;
	GtkWidget *editor_stack, *toolbar, *pane_area;
	/* The bottom panel shows the terminal or the problems list, one at a time */
	GtkWidget *bottom_stack, *problems_panel, *problems_button, *new_terminal_button;
	/* Files whose last view closed, newest last, for reopening them */
	GPtrArray *closed_files;
	PanelSlide *sidebar_slide, *bottom_slide;
	/* The empty page offers to open something without a project, and to show the files when the sidebar is hidden */
	GtkWidget *empty_page, *open_buttons, *show_files_button;
	/* The split panes in the order they were made, and the one new files open in */
	GPtrArray *panes;
	Pane *current_pane;
	FindBar *find_bar;
	GtkWidget *mode_label, *command_label, *errors_label, *warnings_label, *position_label;
	GtkWidget *language_label, *server_button, *sidebar_button, *selection_label, *mode_switch;
	/* Set while the mode switch follows the editor, so that change is not taken as the person's choice */
	gboolean is_updating_mode;
	/* Set while requested paths wait for the first frame, so the empty page stays blank */
	gboolean is_loading;
	LspManager *servers;
	/* Folder path of the open project, or NULL when only loose files are open */
	char *project;
	WindowState state;
	gboolean is_close_confirmed;
	/* Set once the editors are freed for closing, after which nothing may look at them again */
	gboolean is_closed;
};

Editor *get_current_editor(Window *window);
void get_window_editor_events(Window *window, EditorEvents *events);
/* Shows the empty page's choices that fit the project, sidebar and open files */
void update_window_empty_page(Window *window);
/* Updates the title, status bar and find bar for the file in front */
void refresh_window_for_editor(Window *window);
Editor *open_window_file(Window *window, GFile *file);
/* Adds an empty untitled file */
Editor *add_window_editor(Window *window);
/* Closes the file's tab, asking about unsaved changes unless is_forced */
void close_window_editor(Window *window, Editor *editor, gboolean is_forced);
GPtrArray *list_window_editors(Window *window);
/* Brings forward a view of the file wherever it is open, or opens it in the current pane */
Editor *show_window_file(Window *window, GFile *file);
/* An open view of the file in any pane, or NULL */
Editor *find_window_file(Window *window, GFile *file);
/* Opens the file whose last view closed most recently */
void reopen_window_file(Window *window);
void set_window_sidebar_visible(Window *window, gboolean is_visible);
gboolean is_window_sidebar_visible(Window *window);
/* Shows the files or the plan in the sidebar, or hides the sidebar when that page already shows */
void toggle_window_sidebar_page(Window *window, gboolean is_plan);
gboolean is_window_files_visible(Window *window);
gboolean is_window_plan_visible(Window *window);
/* Runs the named task, or opens the task file when name is NULL */
void choose_window_task(const char *name, gpointer data);
void open_window_project(Window *window, const char *directory);
void close_window_project(Window *window);
void show_window_message(Window *window, const char *message);
void set_window_terminal_visible(Window *window, gboolean is_visible);
gboolean is_window_terminal_visible(Window *window);
/* Opens another terminal tab in the bottom panel */
void open_window_terminal(Window *window);
void hide_window_panel(Window *window);
void set_window_problems_visible(Window *window, gboolean is_visible);
gboolean is_window_problems_visible(Window *window);
/* Lists the open files' problems again while the problems panel shows */
void refresh_window_problems(Window *window);
void run_window_task(Window *window, const char *name);
/* Hands each open file to the servers again, after the server setup or project changed */
void reconnect_language_servers(Window *window);
/* Stops the current file's server until it is restarted */
void stop_window_language_server(Window *window);
/* Starts every server afresh, including stopped ones */
void restart_window_language_servers(Window *window);
void update_window_status(Window *window);
/* Shows Text and Visual in the title bar for files with a visual editor, matching the one in front */
void update_mode_switch(Window *window);

/* Kept in window-panes.c */
GtkWidget *create_window_panes(Window *window);
void apply_window_pane_settings(Window *window);
void update_window_tab(Window *window, Editor *editor);
void split_window_pane(Window *window, GtkOrientation orientation);
void close_window_pane(Window *window);
void close_other_window_panes(Window *window);
guint count_window_panes(Window *window);
void move_to_window_pane(Window *window, PaneDirection direction);
void move_to_next_window_pane(Window *window);
void select_next_window_tab(Window *window, gboolean is_forward);
void disconnect_window_panes(Window *window);

/* Kept in window-commands.c */
void bind_window_commands(Window *window);
void unbind_window_commands(Window *window);
void refresh_window_commands(Window *window);
gboolean run_window_vim_command(Window *window, const char *text);
/* Runs ":name[!] argument" at once, telling the person when the command cannot run now */
void run_window_command_line(Window *window, const char *text);
/* Runs a command as its menu entry does, when it can run now */
void activate_window_command(Window *window, const Command *command);
GMenuModel *create_window_menu(void);
