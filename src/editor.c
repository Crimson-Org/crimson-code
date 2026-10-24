#include <string.h>

#include <adwaita.h>

#include "../lang/en.h"
#include "comment-markers.h"
#include "editor-colours.h"
#include "editor-completion.h"
#include "editor-cursors.h"
#include "hex-view.h"
#include "json-editor.h"
#include "editor-gutter.h"
#include "editor-hover.h"
#include "common/loading.h"
#include "common/long-lines.h"
#include "editor.h"
#include "minimap.h"
#include "settings.h"

#define LIGHT_SCHEME "Adwaita"
#define DARK_SCHEME "Adwaita-dark"
/* How much of a file is read to tell binary from text */
#define SNIFF_BYTES 8192
/* Reading runs just below drawing, so the loading spinner and the rest of the window keep painting */
#define LOAD_PRIORITY (GDK_PRIORITY_REDRAW + 1)
/* Room below the last line, so it can be scrolled up from the bottom edge */
#define SCROLL_PAST_END_PIXELS 320
#define JSON_LANGUAGE "json"
#define LENIENT_JSON_LANGUAGE "jsonc"
/* Waiting for a pause in typing keeps large files from being sent to the server on each key */
#define CHANGE_DELAY_MILLISECONDS 250
#define SPLIT_WATCH "split-watch"

/* Setting choices and the GtkSourceView values they stand for */
static const struct {
	const char *id;
	GtkSourceSmartHomeEndType mode;
} HOME_END_MODES[] = {
	{"off", GTK_SOURCE_SMART_HOME_END_DISABLED}, {"before", GTK_SOURCE_SMART_HOME_END_BEFORE},
	{"after", GTK_SOURCE_SMART_HOME_END_AFTER}, {"always", GTK_SOURCE_SMART_HOME_END_ALWAYS},
};

static const struct {
	const char *id;
	GtkWrapMode mode;
} WRAP_MODES[] = {
	{"none", GTK_WRAP_NONE}, {"word", GTK_WRAP_WORD}, {"character", GTK_WRAP_CHAR},
	{"word-character", GTK_WRAP_WORD_CHAR},
};

/* Underline colours for each severity, matching the line number colours */
static const struct {
	const char *tag, *colour;
} PROBLEM_TAGS[] = {
	{NULL, NULL}, {"problem-error", "#e01b24"}, {"problem-warning", "#e5a50a"}, {"problem-information", "#3584e4"},
};

/* The text of one file, shared by each view that shows it, such as the same file in two split panes */
typedef struct {
	GtkSourceBuffer *buffer;
	GtkSourceFile *source_file;
	GCancellable *cancellable;
	gboolean is_loading;
	LspClient *client;
	char *uri;
	guint change_timeout;
	/* Problems with columns already turned into characters, and the most severe level on each line */
	GPtrArray *problems;
	GHashTable *line_severities;
	/* The views showing this text, the first one hearing about loading and saving */
	GPtrArray *views;
	/* The view whose cursor the buffer holds, since GTK keeps one cursor per buffer */
	struct Editor *cursor_owner;
	/* The bytes of a binary file, shown in a hex view instead of the text */
	HexDocument *hex;
} Document;

struct Editor {
	Document *document;
	EditorEvents events;
	GtkWidget *root;
	GtkSourceView *view;
	GtkWidget *map;
	GtkSourceGutterRenderer *line_numbers, *problem_icons;
	/* Vim keys exist only while Vim mode is on */
	GtkIMContext *vim;
	GtkEventController *vim_keys;
	char *mode;
	/* GTK views of one buffer share its cursor, so each view keeps its own while another has focus */
	GtkTextMark *cursor, *selection_bound;
	GtkEventController *focus;
	/* Sees the person's scrolls, clicks and keys before the text view does */
	GtkEventController *input;
	/* The text view and the visual JSON editor take turns in this stack, the visual one made when first asked for */
	GtkWidget *pages, *visual, *hex, *loading, *overlay;
	/* Whether the visual editor was chosen, which can be before its page shows while the rows are still read */
	gboolean is_visual;
	/* Set while Enter is being handled, so only a typed new line copies the indentation, not undo or pasting */
	gboolean is_entering_line;
	/* Set after a jump to a line, which stays in view while the lines are laid out, until the person scrolls or types */
	gboolean holds_cursor_line;
};


/* ==================================== Small helpers ==================================== */


static GtkTextBuffer *get_document_buffer(Document *document) {
	return GTK_TEXT_BUFFER(document->buffer);
}


static GtkTextBuffer *get_buffer(Editor *editor) {
	return get_document_buffer(editor->document);
}


static char *get_document_text(Document *document) {
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(get_document_buffer(document), &start, &end);
	return get_joined_text(get_document_buffer(document), &start, &end);
}


static void notify_view_state(Editor *editor) {
	if (editor->events.state_changed)
		editor->events.state_changed(editor, editor->events.data);
}


/* Title, unsaved state, language and problems belong to the text, so each view showing it hears about them */
static void notify_state(Document *document) {
	for (guint index = 0; index < document->views->len; index++)
		notify_view_state(g_ptr_array_index(document->views, index));
}


static Editor *get_first_view(Document *document) {
	return document->views->len ? g_ptr_array_index(document->views, 0) : NULL;
}


/* Uses the chosen scheme for the current light or dark style, or the default one when it was removed */
static void apply_style_scheme(AdwStyleManager *styles, GParamSpec *property, GtkSourceBuffer *buffer) {
	GtkSourceStyleSchemeManager *schemes = gtk_source_style_scheme_manager_get_default();
	gboolean is_dark = adw_style_manager_get_dark(styles);
	GtkSourceStyleScheme *scheme = gtk_source_style_scheme_manager_get_scheme(schemes,
		get_text_setting(is_dark ? "dark-scheme" : "light-scheme"));
	if (!scheme)
		scheme = gtk_source_style_scheme_manager_get_scheme(schemes, is_dark ? DARK_SCHEME : LIGHT_SCHEME);
	gtk_source_buffer_set_style_scheme(buffer, scheme);
}


static void reapply_style_scheme(GObject *schemes, GParamSpec *property, GtkSourceBuffer *buffer) {
	apply_style_scheme(adw_style_manager_get_default(), NULL, buffer);
}


static int get_document_line_severity(Document *document, int line) {
	return GPOINTER_TO_INT(g_hash_table_lookup(document->line_severities, GINT_TO_POINTER(line)));
}


/* A later piece of a split line shares its file line's problems, which only the first piece shows */
static int get_line_severity(gpointer data, int line) {
	GtkTextBuffer *buffer = get_document_buffer(((Editor *)data)->document);
	if (!has_split_lines(buffer))
		return get_document_line_severity(((Editor *)data)->document, line);
	GtkTextIter iter;
	gtk_text_buffer_get_iter_at_line(buffer, &iter, line);
	if (continues_split_line(&iter))
		return 0;
	return get_document_line_severity(((Editor *)data)->document, get_joined_line(buffer, line));
}


static void refresh_views(Document *document) {
	for (guint index = 0; index < document->views->len; index++) {
		Editor *editor = g_ptr_array_index(document->views, index);
		refresh_line_numbers(editor->line_numbers);
		gtk_widget_queue_draw(GTK_WIDGET(editor->problem_icons));
	}
}


/* ==================================== Problems ==================================== */


/* Moves along a line by the server's column units, bytes or UTF-16 code units */
static void get_iter_at_position(Document *document, GtkTextIter *iter, int line, int column) {
	GtkTextBuffer *buffer = get_document_buffer(document);
	if (line > get_joined_line(buffer, gtk_text_buffer_get_line_count(buffer) - 1)) {
		gtk_text_buffer_get_end_iter(buffer, iter);
		return;
	}
	get_iter_at_joined_line(buffer, iter, line);
	gboolean is_utf8 = document->client && uses_utf8_columns(document->client);
	for (int units = 0; units < column; gtk_text_iter_forward_char(iter)) {
		if (gtk_text_iter_ends_line(iter) && !is_split_break(iter))
			break;
		if (is_split_break(iter))
			continue;
		gunichar character = gtk_text_iter_get_char(iter);
		units += is_utf8 ? g_unichar_to_utf8(character, NULL) : character >= 0x10000 ? 2 : 1;
	}
}


static void clear_problems(Document *document) {
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(get_document_buffer(document), &start, &end);
	for (guint index = 1; index < G_N_ELEMENTS(PROBLEM_TAGS); index++)
		gtk_text_buffer_remove_tag_by_name(get_document_buffer(document), PROBLEM_TAGS[index].tag, &start, &end);
	g_ptr_array_set_size(document->problems, 0);
	g_hash_table_remove_all(document->line_severities);
}


static void mark_problem(Document *document, Diagnostic *problem) {
	GtkTextIter start, end;
	get_iter_at_position(document, &start, problem->start_line, problem->start_column);
	get_iter_at_position(document, &end, problem->end_line, problem->end_column);
	/* An empty range still gets one underlined character, so the spot is visible */
	if (gtk_text_iter_equal(&start, &end) && !gtk_text_iter_ends_line(&end))
		gtk_text_iter_forward_char(&end);
	else if (gtk_text_iter_equal(&start, &end))
		gtk_text_iter_backward_char(&start);
	if (problem->severity < G_N_ELEMENTS(PROBLEM_TAGS))
		gtk_text_buffer_apply_tag_by_name(get_document_buffer(document), PROBLEM_TAGS[problem->severity].tag, &start,
			&end);
	for (int line = problem->start_line; line <= problem->end_line; line++) {
		int known = get_document_line_severity(document, line);
		if (!known || (int)problem->severity < known)
			g_hash_table_insert(document->line_severities, GINT_TO_POINTER(line),
				GINT_TO_POINTER(problem->severity));
	}
}


static void show_problems(GPtrArray *diagnostics, gpointer data) {
	Document *document = data;
	clear_problems(document);
	for (guint index = 0; index < diagnostics->len; index++) {
		Diagnostic *problem = g_new0(Diagnostic, 1);
		copy_diagnostic(g_ptr_array_index(diagnostics, index), problem);
		g_ptr_array_add(document->problems, problem);
		mark_problem(document, problem);
	}
	refresh_views(document);
	notify_state(document);
}


GPtrArray *get_editor_problems(Editor *editor) {
	return editor->document->problems;
}


static void scroll_to_cursor(Editor *editor) {
	GtkTextMark *cursor = gtk_text_buffer_get_insert(get_buffer(editor));
	gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(editor->view), cursor, 0.2, FALSE, 0, 0);
}


/* GTK lays out a new file's lines a part at a time after loading, which moves every line below the measured part */
/* The cursor's line is scrolled to again after each part, so a jump made while loading still ends on its line */
static void on_lines_measured(GtkAdjustment *adjustment, Editor *editor) {
	if (editor->holds_cursor_line)
		scroll_to_cursor(editor);
}


/* Any scroll, click or key of the person's own ends the hold, so the view never fights them */
static gboolean on_input(GtkEventControllerLegacy *controller, GdkEvent *event, Editor *editor) {
	GdkEventType type = gdk_event_get_event_type(event);
	if (type == GDK_SCROLL || type == GDK_BUTTON_PRESS || type == GDK_KEY_PRESS || type == GDK_TOUCH_BEGIN)
		editor->holds_cursor_line = FALSE;
	return GDK_EVENT_PROPAGATE;
}


static void move_cursor_into_view(Editor *editor, const GtkTextIter *target) {
	gtk_text_buffer_place_cursor(get_buffer(editor), target);
	editor->holds_cursor_line = TRUE;
	scroll_to_cursor(editor);
}


void move_editor_to_lsp_position(Editor *editor, int line, int column) {
	GtkTextIter target;
	get_iter_at_position(editor->document, &target, line, column);
	move_cursor_into_view(editor, &target);
	gtk_widget_grab_focus(GTK_WIDGET(editor->view));
	show_editor_cursor_hover(editor);
}


void count_editor_problems(Editor *editor, int *errors, int *warnings) {
	*errors = *warnings = 0;
	for (guint index = 0; index < editor->document->problems->len; index++) {
		Diagnostic *problem = g_ptr_array_index(editor->document->problems, index);
		*errors += problem->severity == DIAGNOSTIC_ERROR;
		*warnings += problem->severity == DIAGNOSTIC_WARNING;
	}
}


GPtrArray *find_editor_problems(Editor *editor, const GtkTextIter *iter, gboolean is_whole_line) {
	GPtrArray *found = g_ptr_array_new();
	Document *document = editor->document;
	int line = get_joined_line(get_buffer(editor), gtk_text_iter_get_line(iter));
	for (guint index = 0; index < document->problems->len; index++) {
		Diagnostic *problem = g_ptr_array_index(document->problems, index);
		GtkTextIter start, end;
		get_iter_at_position(document, &start, problem->start_line, problem->start_column);
		get_iter_at_position(document, &end, problem->end_line, problem->end_column);
		gboolean is_on_line = problem->start_line <= line && line <= problem->end_line;
		gboolean is_inside = gtk_text_iter_compare(&start, iter) <= 0 && gtk_text_iter_compare(iter, &end) <= 0;
		if (is_whole_line ? is_on_line : is_inside)
			g_ptr_array_add(found, problem);
	}
	return found;
}


/* Moves to the next or previous problem after the cursor, going round the file, and shows its details */
gboolean move_editor_to_problem(Editor *editor, gboolean is_forward) {
	Document *document = editor->document;
	GtkTextBuffer *buffer = get_buffer(editor);
	GtkTextIter cursor, best, first, last;
	gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
	gboolean has_best = FALSE, has_any = FALSE;
	for (guint index = 0; index < document->problems->len; index++) {
		Diagnostic *problem = g_ptr_array_index(document->problems, index);
		GtkTextIter start;
		get_iter_at_position(document, &start, problem->start_line, problem->start_column);
		if (!has_any || gtk_text_iter_compare(&start, &first) < 0)
			first = start;
		if (!has_any || gtk_text_iter_compare(&start, &last) > 0)
			last = start;
		has_any = TRUE;
		int order = gtk_text_iter_compare(&start, &cursor);
		gboolean is_ahead = is_forward ? order > 0 : order < 0;
		gboolean is_closer = !has_best || (is_forward ? gtk_text_iter_compare(&start, &best) < 0
			: gtk_text_iter_compare(&start, &best) > 0);
		if (is_ahead && is_closer) {
			best = start;
			has_best = TRUE;
		}
	}
	if (!has_any)
		return FALSE;
	GtkTextIter target = has_best ? best : is_forward ? first : last;
	move_cursor_into_view(editor, &target);
	show_editor_cursor_hover(editor);
	return TRUE;
}


/* Counts the server's column units from the start of the line, the opposite of get_iter_at_position */
gboolean get_editor_lsp_position(Editor *editor, const GtkTextIter *iter, int *line, int *column) {
	Document *document = editor->document;
	if (!document->client)
		return FALSE;
	gboolean is_utf8 = uses_utf8_columns(document->client);
	GtkTextIter walker = *iter;
	move_to_joined_line_start(&walker);
	int units = 0;
	for (; gtk_text_iter_compare(&walker, iter) < 0; gtk_text_iter_forward_char(&walker)) {
		if (is_split_break(&walker))
			continue;
		gunichar character = gtk_text_iter_get_char(&walker);
		units += is_utf8 ? g_unichar_to_utf8(character, NULL) : character >= 0x10000 ? 2 : 1;
	}
	*line = get_joined_line(get_buffer(editor), gtk_text_iter_get_line(iter));
	*column = units;
	return TRUE;
}


const char *get_editor_uri(Editor *editor) {
	return editor->document->uri;
}


/* ==================================== Language server ==================================== */


static gboolean send_changes(gpointer data) {
	Document *document = data;
	g_clear_handle_id(&document->change_timeout, g_source_remove);
	g_autofree char *text = get_document_text(document);
	change_lsp_document(document->client, document->uri, text);
	return G_SOURCE_REMOVE;
}


static void on_text_changed(GtkTextBuffer *buffer, Document *document) {
	if (!document->client || document->is_loading)
		return;
	g_clear_handle_id(&document->change_timeout, g_source_remove);
	document->change_timeout = g_timeout_add(CHANGE_DELAY_MILLISECONDS, send_changes, document);
}


static void detach_language_server(Document *document) {
	if (!document->client)
		return;
	if (document->change_timeout)
		send_changes(document);
	g_clear_handle_id(&document->change_timeout, g_source_remove);
	close_lsp_document(document->client, document->uri);
	document->client = NULL;
	clear_problems(document);
	refresh_views(document);
}


/* Each view of a file asks for its server, so asking again for the same server changes nothing */
void attach_editor_language_server(Editor *editor, LspClient *client) {
	Document *document = editor->document;
	GFile *file = gtk_source_file_get_location(document->source_file);
	g_autofree char *uri = file ? g_file_get_uri(file) : NULL;
	if (client && client == document->client && g_strcmp0(uri, document->uri) == 0)
		return;
	detach_language_server(document);
	if (!client || !file)
		return;
	document->client = client;
	g_set_str(&document->uri, uri);
	g_autofree char *path = g_file_get_path(file);
	g_autofree char *text = get_document_text(document);
	LanguageServer *server = find_server_for_file(path);
	open_lsp_document(client, document->uri, guess_language_id(server, path), text, show_problems, document);
	notify_state(document);
}


void send_editor_changes(Editor *editor) {
	if (editor->document->change_timeout)
		send_changes(editor->document);
}


LspClient *get_editor_language_server(Editor *editor) {
	return editor->document->client;
}


/* ==================================== Vim ==================================== */


static void update_vim_mode(Editor *editor) {
	const char *bar = editor->vim ? gtk_source_vim_im_context_get_command_bar_text(
		GTK_SOURCE_VIM_IM_CONTEXT(editor->vim)) : NULL;
	g_autofree char *mode = NULL;
	if (!editor->vim)
		mode = g_strdup(TEXT_MODE_EASY);
	else if (bar && g_str_has_prefix(bar, "-- ") && g_str_has_suffix(bar, " --"))
		mode = g_strndup(bar + 3, strlen(bar) - 6);
	else if (bar && (*bar == ':' || *bar == '/' || *bar == '?'))
		mode = g_strdup(TEXT_MODE_COMMAND);
	else
		mode = g_strdup(TEXT_MODE_NORMAL);
	g_set_str(&editor->mode, mode);
	if (editor->events.cursor_moved)
		editor->events.cursor_moved(editor, editor->events.data);
}


static void on_vim_text_changed(GObject *vim, GParamSpec *property, Editor *editor) {
	update_vim_mode(editor);
}


/* Registered commands take : commands first, and anything else such as :s or :%d is left to Vim itself */
static gboolean on_vim_command(GtkSourceVimIMContext *vim, const char *command, Editor *editor) {
	return editor->events.command_requested && editor->events.command_requested(editor, command,
		editor->events.data);
}


static void set_vim_mode(Editor *editor, gboolean is_enabled) {
	if (is_enabled == (editor->vim != NULL))
		return;
	if (is_enabled) {
		editor->vim = gtk_source_vim_im_context_new();
		editor->vim_keys = gtk_event_controller_key_new();
		gtk_event_controller_set_propagation_phase(editor->vim_keys, GTK_PHASE_CAPTURE);
		gtk_event_controller_key_set_im_context(GTK_EVENT_CONTROLLER_KEY(editor->vim_keys), editor->vim);
		gtk_widget_add_controller(GTK_WIDGET(editor->view), editor->vim_keys);
		gtk_im_context_set_client_widget(editor->vim, GTK_WIDGET(editor->view));
		g_signal_connect(editor->vim, "notify::command-bar-text", G_CALLBACK(on_vim_text_changed), editor);
		g_signal_connect(editor->vim, "notify::command-text", G_CALLBACK(on_vim_text_changed), editor);
		g_signal_connect(editor->vim, "execute-command", G_CALLBACK(on_vim_command), editor);
	} else {
		/* Taking Vim down moves the selection, which refreshes the status bar, so the editor lets go of Vim
		 * first rather than have that refresh read from it half way through */
		GtkIMContext *vim = g_steal_pointer(&editor->vim);
		g_signal_handlers_disconnect_by_data(vim, editor);
		gtk_widget_remove_controller(GTK_WIDGET(editor->view), editor->vim_keys);
		editor->vim_keys = NULL;
		gtk_im_context_set_client_widget(vim, NULL);
		g_object_unref(vim);
		gtk_text_view_set_overwrite(GTK_TEXT_VIEW(editor->view), FALSE);
	}
	update_vim_mode(editor);
}


const char *get_editor_mode(Editor *editor) {
	return editor->mode;
}


const char *get_editor_command(Editor *editor) {
	if (!editor->vim)
		return "";
	GtkSourceVimIMContext *vim = GTK_SOURCE_VIM_IM_CONTEXT(editor->vim);
	const char *bar = gtk_source_vim_im_context_get_command_bar_text(vim);
	if (bar && (*bar == ':' || *bar == '/' || *bar == '?'))
		return bar;
	const char *pending = gtk_source_vim_im_context_get_command_text(vim);
	return pending ? pending : "";
}


/* ==================================== Loading and saving ==================================== */


/* Strict JSON marks comments as mistakes, so JSON written with comment lines uses the lenient language */
static gboolean has_comment_lines(Document *document) {
	static GRegex *comment;
	if (!comment)
		comment = g_regex_new("^\\s*(//|/\\*)", G_REGEX_MULTILINE | G_REGEX_OPTIMIZE, 0, NULL);
	g_autofree char *text = get_document_text(document);
	return g_regex_match(comment, text, 0, NULL);
}


static GtkSourceLanguage *guess_language(Document *document) {
	GFile *file = gtk_source_file_get_location(document->source_file);
	g_autofree char *name = file ? g_file_get_basename(file) : NULL;
	return name ? gtk_source_language_manager_guess_language(gtk_source_language_manager_get_default(), name, NULL)
		: NULL;
}


static gboolean is_json_language(GtkSourceLanguage *language) {
	const char *id = language ? gtk_source_language_get_id(language) : NULL;
	return g_strcmp0(id, JSON_LANGUAGE) == 0 || g_strcmp0(id, LENIENT_JSON_LANGUAGE) == 0;
}


/* JSON files open in the visual editor when the setting asks for it, which is known from the name before loading */
static gboolean opens_visual(Document *document) {
	return get_switch_setting("visual-editors") && is_json_language(guess_language(document));
}


static void choose_language(Document *document) {
	GtkSourceLanguageManager *languages = gtk_source_language_manager_get_default();
	GtkSourceLanguage *language = guess_language(document);
	if (language && g_strcmp0(gtk_source_language_get_id(language), JSON_LANGUAGE) == 0
		&& has_comment_lines(document))
		language = gtk_source_language_manager_get_language(languages, LENIENT_JSON_LANGUAGE);
	gtk_source_buffer_set_language(document->buffer, has_split_lines(GTK_TEXT_BUFFER(document->buffer)) ? NULL
		: language);
}


static void set_views_editable(Document *document, gboolean is_editable) {
	for (guint index = 0; index < document->views->len; index++)
		gtk_text_view_set_editable(GTK_TEXT_VIEW(((Editor *)g_ptr_array_index(document->views, index))->view),
			is_editable);
}


static void report_failure(Document *document, const char *message) {
	Editor *first = get_first_view(document);
	if (first && first->events.failed)
		first->events.failed(first, message, first->events.data);
}


/* The loading page shows at once with a spinner that only appears when loading takes a while */
static void start_loading(Editor *editor) {
	if (editor->loading)
		gtk_stack_remove(GTK_STACK(editor->pages), editor->loading);
	editor->loading = create_loading_spinner();
	gtk_stack_add_named(GTK_STACK(editor->pages), editor->loading, "loading");
	/* Without a fade, since the text the page leaves is already being replaced by the arriving file */
	gtk_stack_set_visible_child_full(GTK_STACK(editor->pages), "loading", GTK_STACK_TRANSITION_TYPE_NONE);
}


/* The document fades in once ready, as hex for binary files and as text for the rest */
static void finish_loading(Editor *editor) {
	gtk_stack_set_visible_child_name(GTK_STACK(editor->pages), editor->hex ? "hex" : "text");
	if (editor->hex)
		focus_hex_view(editor->hex);
}


static void on_loaded(GObject *source, GAsyncResult *result, gpointer data) {
	Document *document = data;
	g_autoptr(GError) error = NULL;
	gboolean is_loaded = gtk_source_file_loader_load_finish(GTK_SOURCE_FILE_LOADER(source), result, &error);
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;
	document->is_loading = FALSE;
	set_views_editable(document, TRUE);
	if (!is_loaded && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
		for (guint index = 0; index < document->views->len; index++)
			finish_loading(g_ptr_array_index(document->views, index));
		report_failure(document, error->message);
		return;
	}
	GtkTextIter start;
	gtk_text_buffer_get_start_iter(get_document_buffer(document), &start);
	gtk_text_buffer_place_cursor(get_document_buffer(document), &start);
	gtk_text_buffer_set_modified(get_document_buffer(document), FALSE);
	choose_language(document);
	notify_state(document);
	for (guint index = 0; index < document->views->len; index++) {
		Editor *editor = g_ptr_array_index(document->views, index);
		/* A visual view keeps its rows and reads them again from the loaded text, unless that text cannot be shown
		 * as rows after all, such as a file whose long lines were split */
		if (is_editor_visual(editor) && !is_editor_json(editor))
			set_editor_visual(editor, FALSE);
		else if (is_editor_visual(editor))
			reload_json_editor(editor->visual);
		else
			finish_loading(editor);
		if (editor->events.loaded)
			editor->events.loaded(editor, editor->events.data);
	}
}


/* Text shows as soon as its first part is in and the rest streams in below it, while a file opening in the visual
 * editor keeps its loading page until the rows are read */
static void on_load_progress(goffset current_bytes, goffset total_bytes, gpointer data) {
	Document *document = data;
	if (opens_visual(document))
		return;
	for (guint index = 0; index < document->views->len; index++) {
		Editor *editor = g_ptr_array_index(document->views, index);
		if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(editor->pages)), "loading") == 0)
			finish_loading(editor);
	}
}


/* Reads in the background, so the tab shows at once and the window keeps responding for huge files */
static void load_text(Document *document) {
	g_autoptr(GtkSourceFileLoader) loader = gtk_source_file_loader_new(document->buffer, document->source_file);
	gtk_source_file_loader_load_async(loader, LOAD_PRIORITY, document->cancellable, on_load_progress, document, NULL,
		on_loaded, document);
}


static void on_hex_changed(gpointer data) {
	notify_state(((Editor *)data)->document);
}


static void add_hex_page(Editor *editor) {
	if (editor->hex || !editor->document->hex)
		return;
	editor->hex = create_hex_view(editor->document->hex, editor->document->buffer, on_hex_changed, editor);
	gtk_stack_add_named(GTK_STACK(editor->pages), editor->hex, "hex");
}


static void on_hex_loaded(HexDocument *hex, GError *error, gpointer data) {
	Document *document = data;
	document->is_loading = FALSE;
	if (!hex) {
		report_failure(document, error->message);
		for (guint index = 0; index < document->views->len; index++)
			finish_loading(g_ptr_array_index(document->views, index));
		return;
	}
	document->hex = hex;
	notify_state(document);
	for (guint index = 0; index < document->views->len; index++) {
		Editor *editor = g_ptr_array_index(document->views, index);
		editor->is_visual = FALSE;
		add_hex_page(editor);
		finish_loading(editor);
		if (editor->events.loaded)
			editor->events.loaded(editor, editor->events.data);
	}
}


/* A zero byte near the start marks a binary file, which opens in the hex view rather than as text */
static void on_start_read(GObject *source, GAsyncResult *result, gpointer data) {
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) start = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;
	Document *document = data;
	g_input_stream_close(G_INPUT_STREAM(source), NULL, NULL);
	gsize length = 0;
	const guint8 *bytes = start ? g_bytes_get_data(start, &length) : NULL;
	if (bytes && is_binary_start(bytes, length))
		load_hex_document(gtk_source_file_get_location(document->source_file), document->cancellable,
			on_hex_loaded, document);
	else
		load_text(document);
}


static void on_file_opened(GObject *source, GAsyncResult *result, gpointer data) {
	g_autoptr(GError) error = NULL;
	g_autoptr(GFileInputStream) stream = g_file_read_finish(G_FILE(source), result, &error);
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;
	Document *document = data;
	/* A file that cannot be opened here, such as a new one, goes to the text loader, which reports it */
	if (!stream) {
		load_text(document);
		return;
	}
	g_input_stream_read_bytes_async(G_INPUT_STREAM(stream), SNIFF_BYTES, G_PRIORITY_DEFAULT, document->cancellable,
		on_start_read, document);
}


/* Rows read while the file was still loading replace the loading page, so the wait shows one spinner throughout */
static void on_visual_ready(gpointer data) {
	Editor *editor = data;
	if (editor->is_visual)
		gtk_stack_set_visible_child_name(GTK_STACK(editor->pages), "visual");
}


/* The visual editor writes with the same indentation the text view would insert */
static char *get_indent_text(Editor *editor) {
	if (!gtk_source_view_get_insert_spaces_instead_of_tabs(editor->view))
		return g_strdup("\t");
	int width = gtk_source_view_get_indent_width(editor->view);
	return g_strnfill(width > 0 ? (gsize)width : gtk_source_view_get_tab_width(editor->view), ' ');
}


static void add_visual_page(Editor *editor) {
	if (!editor->visual) {
		editor->visual = create_json_editor(get_buffer(editor), on_visual_ready, editor);
		gtk_stack_add_named(GTK_STACK(editor->pages), editor->visual, "visual");
	}
	g_autofree char *indent = get_indent_text(editor);
	set_json_editor_indent(editor->visual, indent);
}


/* A file opening in the visual editor shows its rows read from the file on disk while the text still loads */
static void preview_visual(Editor *editor, GFile *file) {
	editor->is_visual = TRUE;
	add_visual_page(editor);
	preview_json_editor_file(editor->visual, file);
}


void load_editor_file(Editor *editor, GFile *file) {
	Document *document = editor->document;
	gtk_source_file_set_location(document->source_file, file);
	document->is_loading = TRUE;
	set_views_editable(document, FALSE);
	/* The language is set once the text is in, since a language matched every arriving chunk and took seconds for
	 * big files, while once loaded only the shown lines are matched at once */
	gtk_source_buffer_set_language(document->buffer, NULL);
	notify_state(document);
	for (guint index = 0; index < document->views->len; index++) {
		Editor *editor = g_ptr_array_index(document->views, index);
		start_loading(editor);
		if (opens_visual(document))
			preview_visual(editor, file);
	}
	g_file_read_async(file, G_PRIORITY_DEFAULT, document->cancellable, on_file_opened, document);
}


static void finish_saving(Document *document, GError *error) {
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			report_failure(document, error->message);
		return;
	}
	/* After Save As the window hands the file to the server for its new name instead */
	g_autofree char *uri = g_file_get_uri(gtk_source_file_get_location(document->source_file));
	choose_language(document);
	if (document->client && g_strcmp0(uri, document->uri) == 0)
		save_lsp_document(document->client, document->uri);
	notify_state(document);
	Editor *first = get_first_view(document);
	if (first && first->events.saved)
		first->events.saved(first, first->events.data);
}


static void on_saved(GObject *source, GAsyncResult *result, gpointer data) {
	g_autoptr(GError) error = NULL;
	gtk_source_file_saver_save_finish(GTK_SOURCE_FILE_SAVER(source), result, &error);
	finish_saving(data, error);
}


/* Pairs a document with where its split text is being written, so the file's name follows a Save As */
typedef struct {
	Document *document;
	GFile *target;
} SplitSave;


static void on_split_saved(GObject *source, GAsyncResult *result, gpointer data) {
	SplitSave *save = data;
	g_autoptr(GError) error = NULL;
	if (g_file_replace_contents_finish(G_FILE(source), result, NULL, &error)) {
		gtk_source_file_set_location(save->document->source_file, save->target);
		gtk_text_buffer_set_modified(get_document_buffer(save->document), FALSE);
	}
	finish_saving(save->document, error);
	g_object_unref(save->target);
	g_free(save);
}


/* GtkSourceFileSaver reads its buffer back line by line, which crawls through a huge line, so a document with split
 * lines writes the file's own text directly, with the line breaks and character set the file was read with */
static void save_split_text(Document *document, GFile *target) {
	GtkTextBuffer *buffer = get_document_buffer(document);
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(buffer, &start, &end);
	g_autofree char *joined = get_joined_text(buffer, &start, &end);
	GString *text = g_string_new(joined);
	if (gtk_source_buffer_get_implicit_trailing_newline(document->buffer))
		g_string_append_c(text, '\n');
	/* The buffer keeps each line's own break, which all become the file's kind, as GtkSourceFileSaver writes them */
	GtkSourceNewlineType newline = gtk_source_file_get_newline_type(document->source_file);
	g_string_replace(text, "\r\n", "\n", 0);
	g_string_replace(text, "\r", "\n", 0);
	if (newline != GTK_SOURCE_NEWLINE_TYPE_LF)
		g_string_replace(text, "\n", newline == GTK_SOURCE_NEWLINE_TYPE_CR ? "\r" : "\r\n", 0);
	const GtkSourceEncoding *encoding = gtk_source_file_get_encoding(document->source_file);
	const char *charset = encoding ? gtk_source_encoding_get_charset(encoding) : "UTF-8";
	gsize length = text->len;
	g_autofree char *bytes = g_string_free(text, FALSE);
	if (g_ascii_strcasecmp(charset, "UTF-8") != 0) {
		g_autoptr(GError) error = NULL;
		char *converted = g_convert(bytes, (gssize)length, charset, "UTF-8", NULL, &length, &error);
		if (!converted) {
			finish_saving(document, error);
			return;
		}
		g_free(bytes);
		bytes = converted;
	}
	SplitSave *save = g_new0(SplitSave, 1);
	save->document = document;
	save->target = g_object_ref(target);
	g_autoptr(GBytes) contents = g_bytes_new_take(g_steal_pointer(&bytes), length);
	g_file_replace_contents_bytes_async(target, contents, NULL, FALSE, G_FILE_CREATE_NONE, document->cancellable,
		on_split_saved, save);
}


static void trim_trailing_spaces(Editor *editor) {
	GtkTextBuffer *buffer = get_buffer(editor);
	gtk_text_buffer_begin_user_action(buffer);
	for (int line = gtk_text_buffer_get_line_count(buffer) - 1; line >= 0; line--) {
		GtkTextIter end, start;
		gtk_text_buffer_get_iter_at_line(buffer, &end, line);
		if (!gtk_text_iter_ends_line(&end))
			gtk_text_iter_forward_to_line_end(&end);
		/* Spaces before a break between pieces of one line lie inside the file's line */
		if (is_split_break(&end))
			continue;
		start = end;
		while (!gtk_text_iter_starts_line(&start) && gtk_text_iter_backward_char(&start)
			&& (gtk_text_iter_get_char(&start) == ' ' || gtk_text_iter_get_char(&start) == '\t')) {
		}
		if (gtk_text_iter_get_char(&start) != ' ' && gtk_text_iter_get_char(&start) != '\t')
			gtk_text_iter_forward_char(&start);
		if (gtk_text_iter_compare(&start, &end) < 0)
			gtk_text_buffer_delete(buffer, &start, &end);
	}
	gtk_text_buffer_end_user_action(buffer);
}


/* Hex documents write their bytes themselves, and Save As moves the document to its new file */
static void save_hex(Editor *editor, GFile *file) {
	Document *document = editor->document;
	g_autoptr(GError) error = NULL;
	if (!save_hex_document(document->hex, file, &error)) {
		report_failure(document, error->message);
		return;
	}
	if (file)
		gtk_source_file_set_location(document->source_file, file);
	notify_state(document);
	Editor *first = get_first_view(document);
	if (first && first->events.saved)
		first->events.saved(first, first->events.data);
}


gboolean save_editor(Editor *editor, GFile *file) {
	Document *document = editor->document;
	if (document->is_loading)
		return TRUE;
	GFile *target = file ? file : gtk_source_file_get_location(document->source_file);
	g_autoptr(GFile) folder = target ? g_file_get_parent(target) : NULL;
	if (folder && !g_file_query_exists(folder, NULL) && editor->events.folder_missing) {
		editor->events.folder_missing(editor, file, editor->events.data);
		return FALSE;
	}
	if (document->hex) {
		save_hex(editor, file);
		return TRUE;
	}
	if (get_switch_setting("trim-trailing-spaces"))
		trim_trailing_spaces(editor);
	if (has_split_lines(get_document_buffer(document))) {
		save_split_text(document, target);
		return TRUE;
	}
	g_autoptr(GtkSourceFileSaver) saver = file
		? gtk_source_file_saver_new_with_target(document->buffer, document->source_file, file)
		: gtk_source_file_saver_new(document->buffer, document->source_file);
	/* Without a reload prompt yet, a file changed elsewhere is overwritten instead of refusing to save */
	gtk_source_file_saver_set_flags(saver, GTK_SOURCE_FILE_SAVER_FLAGS_IGNORE_MODIFICATION_TIME);
	gtk_source_file_saver_save_async(saver, G_PRIORITY_DEFAULT, document->cancellable, NULL, NULL, NULL, on_saved,
		document);
	return TRUE;
}


GFile *get_editor_file(Editor *editor) {
	return gtk_source_file_get_location(editor->document->source_file);
}


char *get_editor_title(Editor *editor) {
	GFile *file = get_editor_file(editor);
	return file ? g_file_get_basename(file) : g_strdup(TEXT_UNTITLED);
}


gboolean is_editor_loading(Editor *editor) {
	return editor->document->is_loading;
}


/* Text arriving from the file is not a change, so a file still loading never shows as unsaved */
gboolean is_editor_modified(Editor *editor) {
	if (editor->document->is_loading)
		return FALSE;
	if (editor->document->hex)
		return is_hex_document_modified(editor->document->hex);
	return gtk_text_buffer_get_modified(get_buffer(editor));
}


gboolean is_editor_hex(Editor *editor) {
	return editor->document->hex != NULL;
}


GtkWidget *get_editor_hex_view(Editor *editor) {
	return editor->hex;
}


gboolean is_editor_json(Editor *editor) {
	if (editor->document->hex)
		return FALSE;
	return is_json_language(gtk_source_buffer_get_language(editor->document->buffer));
}


gboolean is_editor_visual(Editor *editor) {
	return editor->is_visual;
}


void set_editor_visual(Editor *editor, gboolean is_visual) {
	if (is_visual == is_editor_visual(editor) || (is_visual && !is_editor_json(editor)))
		return;
	editor->is_visual = is_visual;
	if (!is_visual) {
		gtk_stack_set_visible_child_name(GTK_STACK(editor->pages), "text");
		gtk_widget_grab_focus(GTK_WIDGET(editor->view));
		return;
	}
	add_visual_page(editor);
	reload_json_editor(editor->visual);
	if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(editor->pages)), "loading") != 0)
		gtk_stack_set_visible_child_name(GTK_STACK(editor->pages), "visual");
}


const char *get_editor_language_name(Editor *editor) {
	if (editor->document->hex)
		return TEXT_HEX_LANGUAGE;
	GtkSourceLanguage *language = editor->document->is_loading ? guess_language(editor->document)
		: gtk_source_buffer_get_language(editor->document->buffer);
	return language ? gtk_source_language_get_name(language) : TEXT_PLAIN_TEXT;
}


void get_editor_cursor(Editor *editor, int *line, int *column) {
	GtkTextIter cursor;
	gtk_text_buffer_get_iter_at_mark(get_buffer(editor), &cursor, gtk_text_buffer_get_insert(get_buffer(editor)));
	*line = get_joined_line(get_buffer(editor), gtk_text_iter_get_line(&cursor));
	*column = gtk_text_iter_get_line_offset(&cursor);
	/* The pieces before the cursor's on a split line count toward its column, without their breaks */
	GtkTextIter piece = cursor;
	gtk_text_iter_set_line_offset(&piece, 0);
	while (continues_split_line(&piece)) {
		gtk_text_iter_backward_line(&piece);
		*column += gtk_text_iter_get_chars_in_line(&piece) - 1;
	}
}


int get_editor_selection_length(Editor *editor) {
	GtkTextIter start, end;
	if (!gtk_text_buffer_get_selection_bounds(get_buffer(editor), &start, &end))
		return 0;
	return gtk_text_iter_get_offset(&end) - gtk_text_iter_get_offset(&start);
}


/* ==================================== Editing ==================================== */


static gboolean is_commented(GtkTextBuffer *buffer, int line, const char *prefix, gboolean *is_blank) {
	GtkTextIter start, end;
	gtk_text_buffer_get_iter_at_line(buffer, &start, line);
	end = start;
	if (!gtk_text_iter_ends_line(&end))
		gtk_text_iter_forward_to_line_end(&end);
	g_autofree char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
	const char *content = text + strspn(text, " \t");
	*is_blank = *content == '\0';
	return g_str_has_prefix(content, prefix);
}


static void get_content_start(GtkTextBuffer *buffer, GtkTextIter *iter, int line) {
	gtk_text_buffer_get_iter_at_line(buffer, iter, line);
	while (!gtk_text_iter_ends_line(iter) && g_unichar_isspace(gtk_text_iter_get_char(iter)))
		gtk_text_iter_forward_char(iter);
}


static void change_line_comment(GtkTextBuffer *buffer, int line, const char *prefix, gboolean is_adding) {
	GtkTextIter start;
	get_content_start(buffer, &start, line);
	if (is_adding) {
		g_autofree char *inserted = g_strconcat(prefix, " ", NULL);
		gtk_text_buffer_insert(buffer, &start, inserted, -1);
		return;
	}
	GtkTextIter end = start;
	gtk_text_iter_forward_chars(&end, g_utf8_strlen(prefix, -1));
	if (gtk_text_iter_get_char(&end) == ' ')
		gtk_text_iter_forward_char(&end);
	gtk_text_buffer_delete(buffer, &start, &end);
}


/* GtkSourceView keeps the file's last line break out of the text while it writes one when saving */
static gboolean has_implicit_line_break(Editor *editor) {
	return gtk_source_buffer_get_implicit_trailing_newline(editor->document->buffer);
}


char *get_editor_file_text(Editor *editor) {
	char *text = get_document_text(editor->document);
	if (!has_implicit_line_break(editor))
		return text;
	char *file_text = g_strconcat(text, "\n", NULL);
	g_free(text);
	return file_text;
}


/* Only the part between the unchanged start and end is replaced, so the cursor and undo history stay useful */
void replace_editor_text(Editor *editor, const char *file_text) {
	g_autofree char *old_text = get_document_text(editor->document);
	g_autofree char *text = g_strdup(file_text);
	if (has_implicit_line_break(editor) && g_str_has_suffix(text, "\n"))
		text[strlen(text) - 1] = '\0';
	gsize old_length = strlen(old_text), new_length = strlen(text), start = 0, end = 0;
	while (start < old_length && start < new_length && old_text[start] == text[start])
		start++;
	while ((old_text[start] & 0xC0) == 0x80)
		start--;
	while (end < old_length - start && end < new_length - start
		&& old_text[old_length - end - 1] == text[new_length - end - 1])
		end++;
	while (end > 0 && (old_text[old_length - end] & 0xC0) == 0x80)
		end--;
	GtkTextBuffer *buffer = get_buffer(editor);
	GtkTextIter first, last;
	get_iter_at_joined_offset(buffer, &first, (int)g_utf8_pointer_to_offset(old_text, old_text + start));
	get_iter_at_joined_offset(buffer, &last, (int)g_utf8_pointer_to_offset(old_text, old_text + old_length - end));
	gtk_text_buffer_begin_user_action(buffer);
	gtk_text_buffer_delete(buffer, &first, &last);
	gtk_text_buffer_insert(buffer, &first, text + start, new_length - end - start);
	gtk_text_buffer_end_user_action(buffer);
}


/* Comments the selected lines, or uncomments them when each non-blank one is already commented */
void toggle_editor_comment(Editor *editor) {
	GtkSourceLanguage *language = gtk_source_buffer_get_language(editor->document->buffer);
	if (!language)
		return;
	g_autoptr(CommentMarkers) markers = get_comment_markers(gtk_source_language_get_id(language));
	const char *prefix = markers->line;
	if (!*prefix)
		return;
	GtkTextBuffer *buffer = get_buffer(editor);
	GtkTextIter start, end;
	gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
	int first = gtk_text_iter_get_line(&start), last = gtk_text_iter_get_line(&end);
	if (last > first && gtk_text_iter_starts_line(&end))
		last--;
	gboolean is_adding = FALSE;
	for (int line = first; line <= last; line++) {
		gboolean is_blank = FALSE;
		gboolean has_comment = is_commented(buffer, line, prefix, &is_blank);
		is_adding |= !is_blank && !has_comment;
	}
	gtk_text_buffer_begin_user_action(buffer);
	for (int line = first; line <= last; line++) {
		gboolean is_blank = FALSE;
		gboolean has_comment = is_commented(buffer, line, prefix, &is_blank);
		if (!is_blank && has_comment != is_adding)
			change_line_comment(buffer, line, prefix, is_adding);
	}
	gtk_text_buffer_end_user_action(buffer);
}


static void apply_space_drawing(Editor *editor) {
	GtkSourceSpaceDrawer *drawer = gtk_source_view_get_space_drawer(editor->view);
	const char *choice = get_text_setting("draw-spaces");
	GtkSourceSpaceTypeFlags every_kind = GTK_SOURCE_SPACE_TYPE_SPACE | GTK_SOURCE_SPACE_TYPE_TAB
		| GTK_SOURCE_SPACE_TYPE_NBSP;
	gtk_source_space_drawer_set_types_for_locations(drawer, GTK_SOURCE_SPACE_LOCATION_ALL,
		GTK_SOURCE_SPACE_TYPE_NONE);
	gtk_source_space_drawer_set_types_for_locations(drawer, GTK_SOURCE_SPACE_LOCATION_TRAILING, every_kind);
	/* Spaces between words are left out, since a dot between each word only clutters the text */
	if (g_strcmp0(choice, "all") == 0) {
		gtk_source_space_drawer_set_types_for_locations(drawer, GTK_SOURCE_SPACE_LOCATION_LEADING, every_kind);
		gtk_source_space_drawer_set_types_for_locations(drawer, GTK_SOURCE_SPACE_LOCATION_INSIDE_TEXT,
			GTK_SOURCE_SPACE_TYPE_TAB | GTK_SOURCE_SPACE_TYPE_NBSP);
	}
	gtk_source_space_drawer_set_enable_matrix(drawer, g_strcmp0(choice, "none") != 0);
}


/* The setting's choices in the order of MinimapSize */
static void apply_minimap_size(Editor *editor) {
	static const char *const sizes[] = {"small", "medium", "large"};
	for (guint index = 0; index < G_N_ELEMENTS(sizes); index++)
		if (g_strcmp0(get_text_setting("minimap-size"), sizes[index]) == 0)
			set_minimap_size(editor->map, (MinimapSize)index);
}


static void apply_layout_settings(Editor *editor) {
	GtkTextView *text = GTK_TEXT_VIEW(editor->view);
	int spacing = get_number_setting("line-spacing");
	gtk_text_view_set_pixels_above_lines(text, spacing / 2);
	gtk_text_view_set_pixels_below_lines(text, spacing - spacing / 2);
	gtk_text_view_set_pixels_inside_wrap(text, spacing);
	gtk_text_view_set_bottom_margin(text, get_switch_setting("scroll-past-end") ? SCROLL_PAST_END_PIXELS : 0);
	for (guint index = 0; index < G_N_ELEMENTS(WRAP_MODES); index++)
		if (g_strcmp0(WRAP_MODES[index].id, get_text_setting("wrap")) == 0)
			gtk_text_view_set_wrap_mode(text, WRAP_MODES[index].mode);
	gtk_widget_set_visible(editor->map, get_switch_setting("show-minimap"));
	apply_minimap_size(editor);
	gtk_widget_set_visible(GTK_WIDGET(editor->line_numbers), get_switch_setting("show-line-numbers"));
	gtk_widget_set_visible(GTK_WIDGET(editor->problem_icons), get_switch_setting("show-problem-icons"));
	apply_space_drawing(editor);
}


static void apply_editor_settings(Editor *editor) {
	gtk_source_view_set_tab_width(editor->view, (guint)get_number_setting("tab-width"));
	gtk_source_view_set_indent_width(editor->view, -1);
	gtk_source_view_set_insert_spaces_instead_of_tabs(editor->view, get_switch_setting("insert-spaces"));
	/* The editor indents new lines itself, since the source view's own way depends on how the input method
	 * reports Enter and does nothing on some systems */
	gtk_source_view_set_auto_indent(editor->view, FALSE);
	gtk_source_view_set_indent_on_tab(editor->view, get_switch_setting("indent-on-tab"));
	gtk_source_view_set_smart_backspace(editor->view, get_switch_setting("smart-backspace"));
	for (guint index = 0; index < G_N_ELEMENTS(HOME_END_MODES); index++)
		if (g_strcmp0(HOME_END_MODES[index].id, get_text_setting("smart-home-end")) == 0)
			gtk_source_view_set_smart_home_end(editor->view, HOME_END_MODES[index].mode);
	gtk_source_view_set_highlight_current_line(editor->view, get_switch_setting("highlight-current-line"));
	gtk_source_buffer_set_highlight_matching_brackets(editor->document->buffer, get_switch_setting("matching-brackets"));
	gtk_source_buffer_set_implicit_trailing_newline(editor->document->buffer, get_switch_setting("final-newline"));
	apply_style_scheme(adw_style_manager_get_default(), NULL, editor->document->buffer);
	apply_layout_settings(editor);
	set_vim_mode(editor, get_switch_setting("vim-mode"));
	refresh_line_numbers(editor->line_numbers);
}


/* Drawing settings such as indentation guides change nothing GTK watches, so the view redraws itself */
static void on_setting_changed(const char *key, gpointer data) {
	Editor *editor = data;
	apply_editor_settings(editor);
	gtk_widget_queue_draw(GTK_WIDGET(editor->view));
}


/* ==================================== Creation ==================================== */


static void on_cursor_moved(GObject *buffer, GParamSpec *property, Editor *editor) {
	if (editor->document->is_loading)
		return;
	if (editor->events.cursor_moved)
		editor->events.cursor_moved(editor, editor->events.data);
}


static void create_problem_tags(Document *document) {
	for (guint index = 1; index < G_N_ELEMENTS(PROBLEM_TAGS); index++) {
		GdkRGBA colour;
		gdk_rgba_parse(&colour, PROBLEM_TAGS[index].colour);
		gtk_text_buffer_create_tag(get_document_buffer(document), PROBLEM_TAGS[index].tag, "underline",
			PANGO_UNDERLINE_ERROR_LINE, "underline-rgba", &colour, NULL);
	}
}


static void notify_document_state(GtkTextBuffer *buffer, Document *document) {
	notify_state(document);
}


/* GtkSourceView's parser spends seconds on each split line even with colours off, so a document with one loses
 * its language, as minified code gains little from colours */
static void notice_split_lines(GtkTextBuffer *buffer, GtkTextIter *end, char *text, int length, gpointer data) {
	if (!has_split_lines(buffer))
		return;
	gtk_source_buffer_set_language(GTK_SOURCE_BUFFER(buffer), NULL);
	gtk_source_buffer_set_highlight_matching_brackets(GTK_SOURCE_BUFFER(buffer), FALSE);
	g_signal_handlers_disconnect_by_data(buffer, SPLIT_WATCH);
}


static Document *create_document(void) {
	Document *document = g_new0(Document, 1);
	document->buffer = gtk_source_buffer_new(NULL);
	document->source_file = gtk_source_file_new();
	document->cancellable = g_cancellable_new();
	document->problems = g_ptr_array_new_with_free_func((GDestroyNotify)free_diagnostic);
	document->line_severities = g_hash_table_new(NULL, NULL);
	document->views = g_ptr_array_new();
	create_problem_tags(document);
	g_signal_connect_object(adw_style_manager_get_default(), "notify::dark", G_CALLBACK(apply_style_scheme),
		document->buffer, 0);
	/* Saving a theme file rescans the schemes, and the buffer then needs the new copy of its scheme */
	g_signal_connect_object(gtk_source_style_scheme_manager_get_default(), "notify::scheme-ids",
		G_CALLBACK(reapply_style_scheme), document->buffer, 0);
	g_signal_connect(document->buffer, "changed", G_CALLBACK(on_text_changed), document);
	g_signal_connect(document->buffer, "modified-changed", G_CALLBACK(notify_document_state), document);
	split_long_lines(GTK_TEXT_BUFFER(document->buffer));
	g_signal_connect_after(document->buffer, "insert-text", G_CALLBACK(notice_split_lines), SPLIT_WATCH);
	watch_colour_codes(document->buffer);
	return document;
}


static void free_document(Document *document) {
	g_cancellable_cancel(document->cancellable);
	detach_language_server(document);
	g_signal_handlers_disconnect_by_data(document->buffer, document);
	unwatch_colour_codes(document->buffer);
	g_clear_handle_id(&document->change_timeout, g_source_remove);
	free_hex_document(document->hex);
	g_object_unref(document->buffer);
	g_object_unref(document->source_file);
	g_object_unref(document->cancellable);
	g_ptr_array_unref(document->problems);
	g_hash_table_unref(document->line_severities);
	g_ptr_array_unref(document->views);
	g_free(document->uri);
	g_free(document);
}


/* Puts this view's own cursor back into the shared buffer when it gets focus, and keeps it when it loses it */
static void on_view_focus_entered(GtkEventControllerFocus *focus, Editor *editor) {
	Document *document = editor->document;
	Editor *previous = document->cursor_owner;
	document->cursor_owner = editor;
	/* Only another view's cursor is swapped out, so moves made from the find bar meanwhile are kept */
	if (!previous || previous == editor)
		return;
	GtkTextIter cursor, bound;
	gtk_text_buffer_get_iter_at_mark(get_buffer(editor), &cursor, editor->cursor);
	gtk_text_buffer_get_iter_at_mark(get_buffer(editor), &bound, editor->selection_bound);
	gtk_text_buffer_select_range(get_buffer(editor), &cursor, &bound);
}


static void on_view_focus_left(GtkEventControllerFocus *focus, Editor *editor) {
	GtkTextBuffer *buffer = get_buffer(editor);
	GtkTextIter cursor, bound;
	gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
	gtk_text_buffer_get_iter_at_mark(buffer, &bound, gtk_text_buffer_get_selection_bound(buffer));
	gtk_text_buffer_move_mark(buffer, editor->cursor, &cursor);
	gtk_text_buffer_move_mark(buffer, editor->selection_bound, &bound);
}


static void on_problem_icon_activated(gpointer data, int line, const GdkRectangle *area) {
	show_editor_line_hover(data, line, area);
}


static void build_view(Editor *editor) {
	editor->view = create_code_view(editor->document->buffer);
	gtk_widget_add_css_class(GTK_WIDGET(editor->view), "code-text");
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(editor->view), TRUE);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(editor->view), EDITOR_LEFT_MARGIN_PIXELS);
	gtk_text_view_set_editable(GTK_TEXT_VIEW(editor->view), !editor->document->is_loading);
	editor->line_numbers = add_line_numbers(editor->view, get_line_severity, editor);
	editor->problem_icons = add_problem_icons(editor->view, get_line_severity, on_problem_icon_activated, editor);
	add_editor_hover(editor);
	add_editor_completion(editor);
	GtkTextBuffer *buffer = get_buffer(editor);
	GtkTextIter cursor;
	gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
	editor->cursor = gtk_text_buffer_create_mark(buffer, NULL, &cursor, FALSE);
	editor->selection_bound = gtk_text_buffer_create_mark(buffer, NULL, &cursor, FALSE);
	editor->focus = gtk_event_controller_focus_new();
	g_signal_connect(editor->focus, "enter", G_CALLBACK(on_view_focus_entered), editor);
	g_signal_connect(editor->focus, "leave", G_CALLBACK(on_view_focus_left), editor);
	gtk_widget_add_controller(GTK_WIDGET(editor->view), editor->focus);
}


/* Vim's K shows details of the word under the cursor, which the Vim engine itself does not know about, so this
 * runs on the scroller just before the engine sees the key */
static gboolean on_vim_key(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType state,
	Editor *editor) {
	gboolean is_plain = !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK));
	if (!editor->vim || key != GDK_KEY_K || !is_plain || g_strcmp0(editor->mode, TEXT_MODE_NORMAL) != 0
		|| *get_editor_command(editor))
		return GDK_EVENT_PROPAGATE;
	show_editor_cursor_hover(editor);
	return GDK_EVENT_STOP;
}


static gboolean on_enter_key(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType state,
	Editor *editor) {
	editor->is_entering_line = key == GDK_KEY_Return || key == GDK_KEY_KP_Enter || key == GDK_KEY_ISO_Enter;
	return GDK_EVENT_PROPAGATE;
}


/* A typed new line starts as far in as the line above it, using the same tabs and spaces */
static void on_text_inserted(GtkTextBuffer *buffer, GtkTextIter *location, char *text, int length, Editor *editor) {
	gboolean is_new_line = g_strcmp0(text, "\n") == 0 || g_strcmp0(text, "\r\n") == 0;
	if (!editor->is_entering_line || !is_new_line || !gtk_widget_has_focus(GTK_WIDGET(editor->view)))
		return;
	editor->is_entering_line = FALSE;
	if (!get_switch_setting("auto-indent"))
		return;
	GtkTextIter start = *location, end;
	if (!gtk_text_iter_backward_line(&start))
		return;
	end = start;
	while (!gtk_text_iter_ends_line(&end) && (gtk_text_iter_get_char(&end) == ' ' || gtk_text_iter_get_char(&end) == '\t'))
		gtk_text_iter_forward_char(&end);
	g_autofree char *indent = gtk_text_iter_get_text(&start, &end);
	if (*indent)
		gtk_text_buffer_insert(buffer, location, indent, -1);
}


/* The minimap takes the place of the scroll bar, which stays hidden while scrolling still works */
static void build_layout(Editor *editor) {
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_EXTERNAL);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), GTK_WIDGET(editor->view));
	gtk_widget_set_hexpand(scroller, TRUE);
	GtkEventController *vim_keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(vim_keys, GTK_PHASE_CAPTURE);
	g_signal_connect(vim_keys, "key-pressed", G_CALLBACK(on_vim_key), editor);
	gtk_widget_add_controller(scroller, vim_keys);
	add_editor_completion_keys(editor, scroller);
	add_editor_cursors(GTK_TEXT_VIEW(editor->view), scroller);
	GtkEventController *enter_keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(enter_keys, GTK_PHASE_CAPTURE);
	g_signal_connect(enter_keys, "key-pressed", G_CALLBACK(on_enter_key), editor);
	gtk_widget_add_controller(scroller, enter_keys);
	editor->map = create_minimap(editor->view);
	GtkWidget *text_page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(text_page), scroller);
	gtk_box_append(GTK_BOX(text_page), editor->map);
	g_signal_connect(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(editor->view)), "changed",
		G_CALLBACK(on_lines_measured), editor);
	editor->input = gtk_event_controller_legacy_new();
	gtk_event_controller_set_propagation_phase(editor->input, GTK_PHASE_CAPTURE);
	g_signal_connect(editor->input, "event", G_CALLBACK(on_input), editor);
	gtk_widget_add_controller(text_page, editor->input);
	editor->pages = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(editor->pages), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_add_named(GTK_STACK(editor->pages), text_page, "text");
	gtk_widget_set_hexpand(editor->pages, TRUE);
	gtk_widget_set_vexpand(editor->pages, TRUE);
	/* Hover cards lie on this layer over the editor, placed by their own code rather than as popups */
	editor->overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(editor->overlay), editor->pages);
	/* Popups such as suggestions belong to this box, which places them as GTK requires of popover parents */
	editor->root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(editor->root), editor->overlay);
	g_object_ref_sink(editor->root);
}


static Editor *create_view(Document *document, const EditorEvents *events) {
	Editor *editor = g_new0(Editor, 1);
	editor->document = document;
	editor->events = *events;
	editor->mode = g_strdup(TEXT_MODE_EASY);
	g_ptr_array_add(document->views, editor);
	build_view(editor);
	build_layout(editor);
	g_signal_connect(document->buffer, "notify::cursor-position", G_CALLBACK(on_cursor_moved), editor);
	g_signal_connect_after(document->buffer, "insert-text", G_CALLBACK(on_text_inserted), editor);
	g_signal_connect(document->buffer, "notify::has-selection", G_CALLBACK(on_cursor_moved), editor);
	apply_editor_settings(editor);
	watch_settings(on_setting_changed, editor);
	/* A second view of a binary or still loading file shows what the first one does */
	add_hex_page(editor);
	if (document->is_loading)
		start_loading(editor);
	else if (document->hex)
		finish_loading(editor);
	return editor;
}


Editor *create_editor(const EditorEvents *events) {
	return create_view(create_document(), events);
}


Editor *create_editor_view(Editor *editor, const EditorEvents *events) {
	return create_view(editor->document, events);
}


guint count_editor_views(Editor *editor) {
	return editor->document->views->len;
}


gboolean is_same_document(Editor *first, Editor *second) {
	return first->document == second->document;
}


void free_editor(Editor *editor) {
	Document *document = editor->document;
	editor->events = (EditorEvents){0};
	unwatch_settings(editor);
	set_vim_mode(editor, FALSE);
	/* The tab view may keep the widgets a moment longer, so nothing in them may call back into this editor */
	gtk_widget_remove_controller(GTK_WIDGET(editor->view), editor->focus);
	disconnect_minimap(editor->map);
	g_signal_handlers_disconnect_by_data(editor->input, editor);
	g_signal_handlers_disconnect_by_data(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(editor->view)), editor);
	disconnect_line_numbers(editor->line_numbers);
	disconnect_problem_icons(editor->problem_icons);
	remove_editor_hover(editor);
	remove_editor_completion(editor);
	remove_editor_cursors(GTK_TEXT_VIEW(editor->view));
	if (editor->visual)
		free_json_editor(editor->visual);
	if (editor->hex)
		free_hex_view(editor->hex);
	g_signal_handlers_disconnect_by_data(document->buffer, editor);
	gtk_text_buffer_delete_mark(get_buffer(editor), editor->cursor);
	gtk_text_buffer_delete_mark(get_buffer(editor), editor->selection_bound);
	g_ptr_array_remove(document->views, editor);
	if (document->cursor_owner == editor)
		document->cursor_owner = NULL;
	g_object_unref(editor->root);
	g_free(editor->mode);
	g_free(editor);
	if (document->views->len == 0)
		free_document(document);
}


GtkWidget *get_editor_overlay(Editor *editor) {
	return editor->overlay;
}


GtkWidget *get_editor_widget(Editor *editor) {
	return editor->root;
}


GtkSourceView *get_editor_view(Editor *editor) {
	return editor->view;
}
