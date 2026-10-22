#pragma once

#include <gtksourceview/gtksource.h>

#include "lsp.h"

/* Space between the gutter and the text */
#define EDITOR_LEFT_MARGIN_PIXELS 8

/* One open file with its text view, minimap, Vim keys and language server connection */
typedef struct Editor Editor;

/* What the window hears from an editor */
typedef struct {
	/* Title, unsaved state, language or problems changed */
	void (*state_changed)(Editor *editor, gpointer data);
	void (*cursor_moved)(Editor *editor, gpointer data);
	void (*loaded)(Editor *editor, gpointer data);
	void (*saved)(Editor *editor, gpointer data);
	/* A : command typed in Vim mode, returning TRUE when a registered command ran it */
	gboolean (*command_requested)(Editor *editor, const char *command, gpointer data);
	void (*failed)(Editor *editor, const char *message, gpointer data);
	/* Saving to file, or to the current file when it is NULL, needs a folder that does not exist yet */
	void (*folder_missing)(Editor *editor, GFile *file, gpointer data);
	gpointer data;
} EditorEvents;

Editor *create_editor(const EditorEvents *events);
/* Another view of the same text, such as the same file in a second split pane */
Editor *create_editor_view(Editor *editor, const EditorEvents *events);
gboolean is_same_document(Editor *first, Editor *second);
/* How many views show this editor's text, so closing one of several never loses changes */
guint count_editor_views(Editor *editor);
void free_editor(Editor *editor);
GtkWidget *get_editor_widget(Editor *editor);
/* The layer over the editor that hover cards lie on */
GtkWidget *get_editor_overlay(Editor *editor);
GtkSourceView *get_editor_view(Editor *editor);

void load_editor_file(Editor *editor, GFile *file);
/* Saves to the given file, or to the current one when file is NULL, and returns FALSE while asking for its missing
 * folder instead */
gboolean save_editor(Editor *editor, GFile *file);
GFile *get_editor_file(Editor *editor);
char *get_editor_title(Editor *editor);
gboolean is_editor_modified(Editor *editor);
const char *get_editor_language_name(Editor *editor);
gboolean is_editor_json(Editor *editor);
/* Whether the file is binary and shown in the hex view */
gboolean is_editor_hex(Editor *editor);
/* The hex view of a binary file, or NULL until it has loaded */
GtkWidget *get_editor_hex_view(Editor *editor);
gboolean is_editor_loading(Editor *editor);
/* Switches between the text and the visual editor, which only JSON files have */
void set_editor_visual(Editor *editor, gboolean is_visual);
gboolean is_editor_visual(Editor *editor);
void get_editor_cursor(Editor *editor, int *line, int *column);
/* Characters in the selection, or 0 without one */
int get_editor_selection_length(Editor *editor);
/* EASY, or the Vim mode such as NORMAL or INSERT */
const char *get_editor_mode(Editor *editor);
/* The Vim command line or pending keys, or an empty string */
const char *get_editor_command(Editor *editor);

/* Hands the file to a language server, or detaches it when client is NULL */
void attach_editor_language_server(Editor *editor, LspClient *client);
LspClient *get_editor_language_server(Editor *editor);
/* Sends edits still waiting for a pause in typing, so a request that follows sees the current text */
void send_editor_changes(Editor *editor);
void count_editor_problems(Editor *editor, int *errors, int *warnings);
/* The file's problems, borrowed until the server next reports */
GPtrArray *get_editor_problems(Editor *editor);
/* Places the cursor at a spot counted in the language server's units and shows what is there */
void move_editor_to_lsp_position(Editor *editor, int line, int column);
void toggle_editor_comment(Editor *editor);
/* The text as saving writes it to the file */
char *get_editor_file_text(Editor *editor);
/* Changes the text to the given file text as one undo step */
void replace_editor_text(Editor *editor, const char *file_text);
/* Problems covering the spot, or anywhere on its line, borrowed until problems next change */
GPtrArray *find_editor_problems(Editor *editor, const GtkTextIter *iter, gboolean is_whole_line);
/* The spot as the language server counts it, or FALSE without a server */
gboolean get_editor_lsp_position(Editor *editor, const GtkTextIter *iter, int *line, int *column);
const char *get_editor_uri(Editor *editor);
gboolean move_editor_to_problem(Editor *editor, gboolean is_forward);
