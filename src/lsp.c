#include <string.h>
#include <unistd.h>

#include <gio/gio.h>

#include "../lang/en.h"
#include "build/json.h"
#include "lsp.h"

#define CONTENT_LENGTH_HEADER "Content-Length:"
#define STOP_GRACE_MILLISECONDS 2000
#define METHOD_NOT_FOUND -32601
#define FULL_TEXT_SYNC 1

typedef void (*ResponseHandler)(LspClient *client, JsonValue *result, gpointer data);

/* Where the answer to a request goes, for requests made inside this file or through request_lsp */
typedef struct {
	ResponseHandler handler;
	LspResultHandler result_handler;
	gpointer data;
} PendingRequest;

/* An open file the server knows about */
typedef struct {
	int version;
	DiagnosticsHandler handler;
	gpointer data;
} Document;

/* Shared by reference between the manager and each pending stream operation, so the last one frees it */
struct LspClient {
	char *name, *root_uri, *root_path, *initialization_options;
	GSubprocess *process;
	GDataInputStream *input, *errors;
	GOutputStream *output;
	GCancellable *cancellable;
	/* Encoded messages waiting for the stream, and messages waiting for the server to finish starting */
	GQueue *outgoing, *waiting;
	GBytes *writing;
	gboolean is_initialized, is_running, uses_utf8;
	GHashTable *requests, *documents;
	int next_id;
	gsize content_length;
	char *body;
};

struct LspManager {
	char *root;
	/* Server id to its running client */
	GHashTable *clients;
	/* Ids of servers the person stopped, which stay stopped until they are restarted */
	GHashTable *stopped;
};

static void read_header(LspClient *client);
static void flush_outgoing(LspClient *client);


/* ==================================== Client lifetime ==================================== */


static void clear_client(gpointer data) {
	LspClient *client = data;
	g_free(client->name);
	g_free(client->root_uri);
	g_free(client->root_path);
	g_free(client->initialization_options);
	g_free(client->body);
	g_clear_object(&client->process);
	g_clear_object(&client->input);
	g_clear_object(&client->errors);
	g_clear_object(&client->cancellable);
	g_queue_free_full(client->outgoing, (GDestroyNotify)g_bytes_unref);
	g_queue_free_full(client->waiting, (GDestroyNotify)free_json);
	g_clear_pointer(&client->writing, g_bytes_unref);
	g_hash_table_unref(client->requests);
	g_hash_table_unref(client->documents);
}


static LspClient *hold_client(LspClient *client) {
	return g_rc_box_acquire(client);
}


static void release_client(LspClient *client) {
	g_rc_box_release_full(client, clear_client);
}


/* Requests that will never be answered still get a reply, so whoever waits can clean up */
static void fail_pending_requests(LspClient *client) {
	GHashTableIter iterator;
	gpointer request = NULL;
	g_autoptr(GPtrArray) waiting = g_ptr_array_new_with_free_func(g_free);
	g_hash_table_iter_init(&iterator, client->requests);
	while (g_hash_table_iter_next(&iterator, NULL, &request)) {
		g_ptr_array_add(waiting, request);
		g_hash_table_iter_steal(&iterator);
	}
	for (guint index = 0; index < waiting->len; index++) {
		PendingRequest *pending = g_ptr_array_index(waiting, index);
		if (pending->result_handler)
			pending->result_handler(NULL, pending->data);
	}
}


static void mark_client_stopped(LspClient *client) {
	if (!client->is_running)
		return;
	client->is_running = FALSE;
	g_cancellable_cancel(client->cancellable);
	fail_pending_requests(client);
	g_message("%s stopped", client->name);
}


const char *get_lsp_client_name(const LspClient *client) {
	return client->name;
}


gboolean uses_utf8_columns(const LspClient *client) {
	return client->uses_utf8;
}


/* ==================================== Writing ==================================== */


static void on_message_written(GObject *source, GAsyncResult *result, gpointer data) {
	LspClient *client = data;
	g_autoptr(GError) error = NULL;
	g_clear_pointer(&client->writing, g_bytes_unref);
	if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL, &error))
		mark_client_stopped(client);
	else
		flush_outgoing(client);
	release_client(client);
}


/* Writes one message at a time, so a slow server never blocks the window while it catches up */
static void flush_outgoing(LspClient *client) {
	if (client->writing || !client->is_running || g_queue_is_empty(client->outgoing))
		return;
	client->writing = g_queue_pop_head(client->outgoing);
	gsize length = 0;
	const void *bytes = g_bytes_get_data(client->writing, &length);
	g_output_stream_write_all_async(client->output, bytes, length, G_PRIORITY_DEFAULT, client->cancellable,
		on_message_written, hold_client(client));
}


static void send_now(LspClient *client, JsonValue *message) {
	set_json_member(message, "jsonrpc", new_json_string("2.0"));
	g_autofree char *body = write_json(message);
	free_json(message);
	char *text = g_strdup_printf(CONTENT_LENGTH_HEADER " %zu\r\n\r\n%s", strlen(body), body);
	g_queue_push_tail(client->outgoing, g_bytes_new_take(text, strlen(text)));
	flush_outgoing(client);
}


/* Messages wait until the server has answered initialize, as the protocol requires */
static void send_when_ready(LspClient *client, JsonValue *message) {
	if (client->is_initialized)
		send_now(client, message);
	else
		g_queue_push_tail(client->waiting, message);
}


static JsonValue *make_notification(const char *method, JsonValue *params) {
	JsonValue *message = new_json_object();
	set_json_member(message, "method", new_json_string(method));
	set_json_member(message, "params", params);
	return message;
}


static void send_request(LspClient *client, const char *method, JsonValue *params, ResponseHandler handler,
	gpointer data, gboolean is_immediate) {
	int id = ++client->next_id;
	PendingRequest *request = g_new0(PendingRequest, 1);
	request->handler = handler;
	request->data = data;
	g_hash_table_insert(client->requests, GINT_TO_POINTER(id), request);
	JsonValue *message = make_notification(method, params);
	set_json_member(message, "id", new_json_number(id));
	if (is_immediate)
		send_now(client, message);
	else
		send_when_ready(client, message);
}


static JsonValue *copy_id(const JsonValue *id) {
	return id->kind == JSON_STRING ? new_json_string(id->string) : new_json_number(id->number);
}


static void send_response(LspClient *client, const JsonValue *id, JsonValue *result) {
	JsonValue *message = new_json_object();
	set_json_member(message, "id", copy_id(id));
	set_json_member(message, "result", result ? result : new_json_null());
	send_now(client, message);
}


/* ==================================== Reading ==================================== */


static JsonValue *make_workspace_folders(LspClient *client) {
	JsonValue *folder = new_json_object();
	set_json_member(folder, "uri", new_json_string(client->root_uri));
	g_autofree char *name = g_path_get_basename(client->root_path);
	set_json_member(folder, "name", new_json_string(name));
	return add_json_item(new_json_array(), folder);
}


/* Servers wait for answers to their own requests, so each gets a neutral reply instead of silence */
static void answer_server_request(LspClient *client, const JsonValue *id, const char *method,
	const JsonValue *params) {
	if (strcmp(method, "workspace/configuration") == 0) {
		JsonValue *answers = new_json_array();
		for (guint index = 0; index < get_json_length(get_json_member(params, "items")); index++)
			add_json_item(answers, new_json_null());
		send_response(client, id, answers);
	} else if (strcmp(method, "workspace/workspaceFolders") == 0) {
		send_response(client, id, make_workspace_folders(client));
	} else if (strcmp(method, "client/registerCapability") == 0 || strcmp(method, "client/unregisterCapability") == 0
		|| strcmp(method, "window/workDoneProgress/create") == 0) {
		send_response(client, id, NULL);
	} else {
		JsonValue *message = new_json_object();
		set_json_member(message, "id", copy_id(id));
		JsonValue *error = new_json_object();
		set_json_member(error, "code", new_json_number(METHOD_NOT_FOUND));
		set_json_member(error, "message", new_json_string("Method not supported"));
		set_json_member(message, "error", error);
		send_now(client, message);
	}
}


void free_diagnostic(Diagnostic *diagnostic) {
	g_free(diagnostic->message);
	g_free(diagnostic->source);
	g_free(diagnostic->code);
	g_free(diagnostic);
}


void copy_diagnostic(const Diagnostic *source, Diagnostic *copy) {
	*copy = *source;
	copy->message = g_strdup(source->message);
	copy->source = g_strdup(source->source);
	copy->code = g_strdup(source->code);
}


/* Codes come as numbers from some servers and as text from others */
static char *read_code(const JsonValue *code) {
	if (code && code->kind == JSON_NUMBER)
		return g_strdup_printf("%.0f", code->number);
	return g_strdup(get_json_string(code, NULL));
}


static void publish_diagnostics(LspClient *client, const JsonValue *params) {
	Document *document = g_hash_table_lookup(client->documents, get_json_string(get_json_member(params, "uri"), ""));
	if (!document)
		return;
	const JsonValue *items = get_json_member(params, "diagnostics");
	g_autoptr(GPtrArray) diagnostics = g_ptr_array_new_with_free_func((GDestroyNotify)free_diagnostic);
	for (guint index = 0; index < get_json_length(items); index++) {
		const JsonValue *item = get_json_item(items, index);
		Diagnostic *diagnostic = g_new0(Diagnostic, 1);
		diagnostic->start_line = (int)get_json_number(find_json_path(item, "range.start.line"), 0);
		diagnostic->start_column = (int)get_json_number(find_json_path(item, "range.start.character"), 0);
		diagnostic->end_line = (int)get_json_number(find_json_path(item, "range.end.line"), 0);
		diagnostic->end_column = (int)get_json_number(find_json_path(item, "range.end.character"), 0);
		int severity = (int)get_json_number(get_json_member(item, "severity"), DIAGNOSTIC_ERROR);
		diagnostic->severity = severity >= DIAGNOSTIC_ERROR && severity <= DIAGNOSTIC_HINT ? severity
			: DIAGNOSTIC_ERROR;
		diagnostic->message = g_strdup(get_json_string(get_json_member(item, "message"), ""));
		diagnostic->source = g_strdup(get_json_string(get_json_member(item, "source"), NULL));
		diagnostic->code = read_code(get_json_member(item, "code"));
		g_ptr_array_add(diagnostics, diagnostic);
	}
	document->handler(diagnostics, document->data);
}


static void handle_message(LspClient *client, const JsonValue *message) {
	const JsonValue *id = get_json_member(message, "id");
	const char *method = get_json_string(get_json_member(message, "method"), NULL);
	const JsonValue *params = get_json_member(message, "params");
	if (method && id) {
		answer_server_request(client, id, method, params);
	} else if (method && strcmp(method, "textDocument/publishDiagnostics") == 0) {
		publish_diagnostics(client, params);
	} else if (method && strcmp(method, "window/logMessage") == 0) {
		g_debug("%s: %s", client->name, get_json_string(get_json_member(params, "message"), ""));
	} else if (id && id->kind == JSON_NUMBER) {
		gpointer key = GINT_TO_POINTER((int)id->number);
		PendingRequest *request = g_hash_table_lookup(client->requests, key);
		const JsonValue *error = get_json_member(message, "error");
		if (error)
			g_warning("%s answered with an error: %s", client->name,
				get_json_string(get_json_member(error, "message"), ""));
		if (request && request->handler)
			request->handler(client, get_json_member(message, "result"), request->data);
		if (request && request->result_handler)
			request->result_handler(error ? NULL : get_json_member(message, "result"), request->data);
		g_hash_table_remove(client->requests, key);
	}
}


static void on_body_read(GObject *source, GAsyncResult *result, gpointer data) {
	LspClient *client = data;
	g_autoptr(GError) error = NULL;
	gsize length = 0;
	if (!g_input_stream_read_all_finish(G_INPUT_STREAM(source), result, &length, &error)
		|| length != client->content_length) {
		mark_client_stopped(client);
		release_client(client);
		return;
	}
	g_autoptr(JsonValue) message = parse_json(client->body, (gssize)length, &error);
	g_clear_pointer(&client->body, g_free);
	client->content_length = 0;
	if (message)
		handle_message(client, message);
	else
		g_warning("%s sent a message that is not JSON: %s", client->name, error->message);
	if (client->is_running)
		read_header(client);
	release_client(client);
}


static void on_header_read(GObject *source, GAsyncResult *result, gpointer data) {
	LspClient *client = data;
	g_autoptr(GError) error = NULL;
	g_autofree char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(source), result, NULL,
		&error);
	if (!line) {
		mark_client_stopped(client);
		release_client(client);
		return;
	}
	if (g_ascii_strncasecmp(line, CONTENT_LENGTH_HEADER, strlen(CONTENT_LENGTH_HEADER)) == 0)
		client->content_length = g_ascii_strtoull(line + strlen(CONTENT_LENGTH_HEADER), NULL, 10);
	if (*line || client->content_length == 0) {
		read_header(client);
	} else {
		client->body = g_malloc(client->content_length + 1);
		g_input_stream_read_all_async(G_INPUT_STREAM(client->input), client->body, client->content_length,
			G_PRIORITY_DEFAULT, client->cancellable, on_body_read, hold_client(client));
	}
	release_client(client);
}


static void read_header(LspClient *client) {
	g_data_input_stream_read_line_async(client->input, G_PRIORITY_DEFAULT, client->cancellable, on_header_read,
		hold_client(client));
}


static void on_error_line_read(GObject *source, GAsyncResult *result, gpointer data) {
	LspClient *client = data;
	g_autofree char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(source), result, NULL,
		NULL);
	if (line) {
		g_debug("%s: %s", client->name, line);
		g_data_input_stream_read_line_async(client->errors, G_PRIORITY_LOW, client->cancellable,
			on_error_line_read, hold_client(client));
	}
	release_client(client);
}


static void on_process_exited(GObject *source, GAsyncResult *result, gpointer data) {
	LspClient *client = data;
	g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);
	mark_client_stopped(client);
	release_client(client);
}


/* ==================================== Starting and stopping ==================================== */


static void on_initialize_answered(LspClient *client, JsonValue *result, gpointer data) {
	const char *encoding = get_json_string(find_json_path(result, "capabilities.positionEncoding"), "utf-16");
	client->uses_utf8 = strcmp(encoding, "utf-8") == 0;
	send_now(client, make_notification("initialized", new_json_object()));
	client->is_initialized = TRUE;
	g_message("%s is ready", client->name);
	while (!g_queue_is_empty(client->waiting))
		send_now(client, g_queue_pop_head(client->waiting));
}


static JsonValue *make_capabilities(void) {
	JsonValue *encodings = add_json_item(add_json_item(new_json_array(), new_json_string("utf-8")),
		new_json_string("utf-16"));
	JsonValue *general = set_json_member(new_json_object(), "positionEncodings", encodings);
	JsonValue *synchronization = set_json_member(new_json_object(), "didSave", new_json_boolean(TRUE));
	JsonValue *diagnostics = set_json_member(new_json_object(), "relatedInformation", new_json_boolean(FALSE));
	/* Servers send plain text unless told that Markdown is welcome */
	JsonValue *formats = add_json_item(add_json_item(new_json_array(), new_json_string("markdown")),
		new_json_string("plaintext"));
	JsonValue *hover = set_json_member(new_json_object(), "contentFormat", formats);
	JsonValue *document = new_json_object();
	set_json_member(document, "synchronization", synchronization);
	set_json_member(document, "hover", hover);
	/* Snippets are left out, so servers send plain text that can be inserted as it is */
	JsonValue *item = set_json_member(new_json_object(), "snippetSupport", new_json_boolean(FALSE));
	set_json_member(item, "documentationFormat", add_json_item(add_json_item(new_json_array(),
		new_json_string("markdown")), new_json_string("plaintext")));
	set_json_member(document, "completion", set_json_member(new_json_object(), "completionItem", item));
	set_json_member(document, "publishDiagnostics", diagnostics);
	JsonValue *workspace = new_json_object();
	set_json_member(workspace, "workspaceFolders", new_json_boolean(TRUE));
	set_json_member(workspace, "configuration", new_json_boolean(TRUE));
	JsonValue *capabilities = new_json_object();
	set_json_member(capabilities, "general", general);
	set_json_member(capabilities, "textDocument", document);
	set_json_member(capabilities, "workspace", workspace);
	return capabilities;
}


static void send_initialize(LspClient *client) {
	JsonValue *params = new_json_object();
	set_json_member(params, "processId", new_json_number(getpid()));
	JsonValue *information = set_json_member(new_json_object(), "name", new_json_string(TEXT_APPLICATION_NAME));
	set_json_member(params, "clientInfo", set_json_member(information, "version",
		new_json_string(APPLICATION_VERSION)));
	set_json_member(params, "rootUri", new_json_string(client->root_uri));
	set_json_member(params, "rootPath", new_json_string(client->root_path));
	set_json_member(params, "workspaceFolders", make_workspace_folders(client));
	set_json_member(params, "capabilities", make_capabilities());
	if (*client->initialization_options) {
		g_autoptr(GError) error = NULL;
		JsonValue *options = parse_json(client->initialization_options, -1, &error);
		if (options)
			set_json_member(params, "initializationOptions", options);
		else
			g_warning("Initialization options for %s are not JSON: %s", client->name, error->message);
	}
	send_request(client, "initialize", params, on_initialize_answered, NULL, TRUE);
}


static LspClient *start_client(const LanguageServer *server, const char *root) {
	g_autofree char *program = find_language_server_program(server);
	g_auto(GStrv) arguments = NULL;
	if (!program || !g_shell_parse_argv(server->command, NULL, &arguments, NULL))
		return NULL;
	g_set_str(&arguments[0], program);
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE
		| G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_set_cwd(launcher, root);
	g_autoptr(GError) error = NULL;
	GSubprocess *process = g_subprocess_launcher_spawnv(launcher, (const char *const *)arguments, &error);
	if (!process) {
		g_warning("%s did not start: %s", server->name, error->message);
		return NULL;
	}
	LspClient *client = g_rc_box_new0(LspClient);
	client->name = g_strdup(server->name);
	client->root_path = g_strdup(root);
	client->root_uri = g_filename_to_uri(root, NULL, NULL);
	client->initialization_options = g_strdup(server->initialization_options);
	client->process = process;
	client->input = g_data_input_stream_new(g_subprocess_get_stdout_pipe(process));
	g_data_input_stream_set_newline_type(client->input, G_DATA_STREAM_NEWLINE_TYPE_CR_LF);
	client->errors = g_data_input_stream_new(g_subprocess_get_stderr_pipe(process));
	client->output = g_subprocess_get_stdin_pipe(process);
	client->cancellable = g_cancellable_new();
	client->outgoing = g_queue_new();
	client->waiting = g_queue_new();
	client->requests = g_hash_table_new_full(NULL, NULL, NULL, g_free);
	client->documents = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	client->is_running = TRUE;
	g_message("Starting %s with %s in %s", server->name, program, root);
	g_subprocess_wait_async(process, NULL, on_process_exited, hold_client(client));
	read_header(client);
	g_data_input_stream_read_line_async(client->errors, G_PRIORITY_LOW, client->cancellable, on_error_line_read,
		hold_client(client));
	send_initialize(client);
	return client;
}


static gboolean force_client_exit(gpointer data) {
	LspClient *client = data;
	if (client->is_running)
		g_subprocess_force_exit(client->process);
	release_client(client);
	return G_SOURCE_REMOVE;
}


static void on_shutdown_answered(LspClient *client, JsonValue *result, gpointer data) {
	send_now(client, make_notification("exit", new_json_null()));
}


/* Asks the server to quit and ends the process if it has not quit after a short grace period */
static void stop_client(LspClient *client) {
	g_hash_table_remove_all(client->documents);
	if (client->is_running && client->is_initialized)
		send_request(client, "shutdown", new_json_null(), on_shutdown_answered, NULL, TRUE);
	g_timeout_add(STOP_GRACE_MILLISECONDS, force_client_exit, hold_client(client));
	release_client(client);
}


void request_lsp(LspClient *client, const char *method, JsonValue *params, LspResultHandler handler, gpointer data) {
	if (!client->is_running) {
		free_json(params);
		handler(NULL, data);
		return;
	}
	int id = ++client->next_id;
	PendingRequest *request = g_new0(PendingRequest, 1);
	request->result_handler = handler;
	request->data = data;
	g_hash_table_insert(client->requests, GINT_TO_POINTER(id), request);
	JsonValue *message = make_notification(method, params);
	set_json_member(message, "id", new_json_number(id));
	send_when_ready(client, message);
}


JsonValue *make_lsp_position_params(const char *uri, int line, int column) {
	JsonValue *position = set_json_member(new_json_object(), "line", new_json_number(line));
	set_json_member(position, "character", new_json_number(column));
	JsonValue *params = set_json_member(new_json_object(), "textDocument",
		set_json_member(new_json_object(), "uri", new_json_string(uri)));
	return set_json_member(params, "position", position);
}


/* ==================================== Documents ==================================== */


static JsonValue *make_document_identifier(const char *uri) {
	return set_json_member(new_json_object(), "uri", new_json_string(uri));
}


void open_lsp_document(LspClient *client, const char *uri, const char *language_id, const char *text,
	DiagnosticsHandler handler, gpointer data) {
	Document *document = g_new0(Document, 1);
	document->version = 1;
	document->handler = handler;
	document->data = data;
	g_hash_table_replace(client->documents, g_strdup(uri), document);
	JsonValue *item = make_document_identifier(uri);
	set_json_member(item, "languageId", new_json_string(language_id));
	set_json_member(item, "version", new_json_number(document->version));
	set_json_member(item, "text", new_json_string(text));
	send_when_ready(client, make_notification("textDocument/didOpen",
		set_json_member(new_json_object(), "textDocument", item)));
}


/* Sends the whole text each time, which each server accepts and keeps the editor side simple */
void change_lsp_document(LspClient *client, const char *uri, const char *text) {
	Document *document = g_hash_table_lookup(client->documents, uri);
	if (!document)
		return;
	JsonValue *identifier = make_document_identifier(uri);
	set_json_member(identifier, "version", new_json_number(++document->version));
	JsonValue *change = set_json_member(new_json_object(), "text", new_json_string(text));
	JsonValue *params = set_json_member(new_json_object(), "textDocument", identifier);
	set_json_member(params, "contentChanges", add_json_item(new_json_array(), change));
	send_when_ready(client, make_notification("textDocument/didChange", params));
}


void save_lsp_document(LspClient *client, const char *uri) {
	if (!g_hash_table_contains(client->documents, uri))
		return;
	send_when_ready(client, make_notification("textDocument/didSave",
		set_json_member(new_json_object(), "textDocument", make_document_identifier(uri))));
}


void close_lsp_document(LspClient *client, const char *uri) {
	if (!g_hash_table_remove(client->documents, uri))
		return;
	send_when_ready(client, make_notification("textDocument/didClose",
		set_json_member(new_json_object(), "textDocument", make_document_identifier(uri))));
}


/* ==================================== Manager ==================================== */


LspManager *create_lsp_manager(void) {
	LspManager *manager = g_new0(LspManager, 1);
	manager->clients = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)stop_client);
	manager->stopped = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	return manager;
}


void set_lsp_manager_root(LspManager *manager, const char *root_directory) {
	g_set_str(&manager->root, root_directory);
}


LspClient *get_lsp_client_for_file(LspManager *manager, const char *path) {
	LanguageServer *server = find_server_for_file(path);
	if (!server || g_hash_table_contains(manager->stopped, server->id))
		return NULL;
	LspClient *client = g_hash_table_lookup(manager->clients, server->id);
	if (client && client->is_running)
		return client;
	g_autofree char *directory = g_path_get_dirname(path);
	client = start_client(server, manager->root ? manager->root : directory);
	if (client)
		g_hash_table_replace(manager->clients, g_strdup(server->id), client);
	else
		g_hash_table_remove(manager->clients, server->id);
	return client;
}


void stop_lsp_clients(LspManager *manager) {
	g_hash_table_remove_all(manager->clients);
}


void stop_lsp_client(LspManager *manager, LspClient *client) {
	GHashTableIter iter;
	gpointer id = NULL, value = NULL;
	g_hash_table_iter_init(&iter, manager->clients);
	while (g_hash_table_iter_next(&iter, &id, &value))
		if (value == client) {
			g_hash_table_add(manager->stopped, g_strdup(id));
			g_hash_table_iter_remove(&iter);
			return;
		}
}


gboolean is_lsp_server_stopped(LspManager *manager, const char *path) {
	LanguageServer *server = find_server_for_file(path);
	return server && g_hash_table_contains(manager->stopped, server->id);
}


void restart_lsp_clients(LspManager *manager) {
	g_hash_table_remove_all(manager->stopped);
	g_hash_table_remove_all(manager->clients);
}


void free_lsp_manager(LspManager *manager) {
	g_hash_table_unref(manager->clients);
	g_hash_table_unref(manager->stopped);
	g_free(manager->root);
	g_free(manager);
}
