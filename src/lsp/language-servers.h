#pragma once

#include <glib.h>

/* A language server the editor can start, either built in or added by the person using it */
typedef struct {
	char *id, *name;
	/* Program and arguments as typed on a command line */
	char *command;
	/* File name patterns separated by spaces, such as "*.c *.h CMakeLists.txt" */
	char *patterns;
	/* Language id sent to the server, or empty to guess it from the file name */
	char *language_id;
	/* JSON text sent as initializationOptions, or empty */
	char *initialization_options;
	gboolean is_enabled, is_builtin;
} LanguageServer;

/* Each known server, built in first, owned by this module */
GPtrArray *get_language_servers(void);
/* Saves the list and tells each watcher, so windows restart the servers they use */
void save_language_servers(void);
void watch_language_servers(void (*on_changed)(gpointer data), gpointer data);
void unwatch_language_servers(gpointer data);
LanguageServer *add_language_server(void);
/* Removes an added server, or returns a built in one to its original values */
void remove_language_server(LanguageServer *server);
/* Full path of the server program, searching the usual install folders as well as PATH */
char *find_language_server_program(const LanguageServer *server);
gboolean matches_language_server(const LanguageServer *server, const char *file_name);
/* The first enabled server for the file whose program is installed */
LanguageServer *find_server_for_file(const char *file_name);
const char *guess_language_id(const LanguageServer *server, const char *file_name);
