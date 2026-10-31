#pragma once

#include <glib.h>

#include "build/json.h"
#include "language-servers.h"

/* Severity values as the protocol numbers them */
typedef enum {
	DIAGNOSTIC_ERROR = 1,
	DIAGNOSTIC_WARNING,
	DIAGNOSTIC_INFORMATION,
	DIAGNOSTIC_HINT,
} DiagnosticSeverity;

/* A problem a server reported, with columns in the encoding the server agreed to */
typedef struct {
	int start_line, start_column, end_line, end_column;
	DiagnosticSeverity severity;
	char *message;
	/* The tool that found it, such as clang or shellcheck, and its code for the problem, or NULL */
	char *source, *code;
} Diagnostic;

typedef void (*DiagnosticsHandler)(GPtrArray *diagnostics, gpointer data);
/* Gets a request's result, or NULL when it failed or the server stopped before answering */
typedef void (*LspResultHandler)(JsonValue *result, gpointer data);

/* A running language server process speaking JSON-RPC over its standard streams */
typedef struct LspClient LspClient;
/* The running servers of one project, started when the first matching file opens */
typedef struct LspManager LspManager;

const char *get_lsp_client_name(const LspClient *client);
/* Columns count bytes when true and UTF-16 code units otherwise */
gboolean uses_utf8_columns(const LspClient *client);
void open_lsp_document(LspClient *client, const char *uri, const char *language_id, const char *text,
	DiagnosticsHandler handler, gpointer data);
void change_lsp_document(LspClient *client, const char *uri, const char *text);
void save_lsp_document(LspClient *client, const char *uri);
void close_lsp_document(LspClient *client, const char *uri);
/* Asks the server something, such as textDocument/hover, taking ownership of params */
void request_lsp(LspClient *client, const char *method, JsonValue *params, LspResultHandler handler, gpointer data);
JsonValue *make_lsp_position_params(const char *uri, int line, int column);
void copy_diagnostic(const Diagnostic *source, Diagnostic *copy);
void free_diagnostic(Diagnostic *diagnostic);

LspManager *create_lsp_manager(void);
void set_lsp_manager_root(LspManager *manager, const char *root_directory);
/* Returns the server for the file, starting it when needed, or NULL when none is installed */
LspClient *get_lsp_client_for_file(LspManager *manager, const char *path);
void stop_lsp_clients(LspManager *manager);
/* Stops one server until restart_lsp_clients, so opening more of its files does not start it again */
void stop_lsp_client(LspManager *manager, LspClient *client);
/* Stops every server and forgets which ones were stopped, so the next files start them afresh */
void restart_lsp_clients(LspManager *manager);
/* Whether the server for this file was stopped by the person */
gboolean is_lsp_server_stopped(LspManager *manager, const char *path);
void free_lsp_manager(LspManager *manager);
