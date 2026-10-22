#include <glob.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>

#include "language-servers.h"

#define CUSTOM_KEY "custom"

/* Servers known to work without setup, in the order they are tried for a file, with folders they are often
 * installed in outside PATH and a program they usually sit next to, such as the bmk of a BlitzMax install */
static const struct {
	const char *id, *name, *command, *patterns, *folders, *companion;
} BUILTIN_SERVERS[] = {
	{"clangd", "C and C++ (clangd)", "clangd", "*.c *.h *.cc *.cpp *.cxx *.hpp *.hh *.hxx *.m *.mm", NULL, NULL},
	{"rust-analyzer", "Rust (rust-analyzer)", "rust-analyzer", "*.rs", NULL, NULL},
	{"pyright", "Python (Pyright)", "pyright-langserver --stdio", "*.py *.pyi", NULL, NULL},
	{"basedpyright", "Python (basedpyright)", "basedpyright-langserver --stdio", "*.py *.pyi", NULL, NULL},
	{"pylsp", "Python (python-lsp-server)", "pylsp", "*.py *.pyi", NULL, NULL},
	{"typescript", "TypeScript and JavaScript", "typescript-language-server --stdio",
		"*.ts *.tsx *.js *.jsx *.mjs *.cjs *.mts *.cts", NULL, NULL},
	{"gopls", "Go (gopls)", "gopls", "*.go go.mod", NULL, NULL},
	{"lua", "Lua", "lua-language-server", "*.lua", NULL, NULL},
	{"bash", "Shell scripts", "bash-language-server start", "*.sh *.bash", NULL, NULL},
	{"json", "JSON", "vscode-json-language-server --stdio", "*.json *.jsonc", NULL, NULL},
	{"html", "HTML", "vscode-html-language-server --stdio", "*.html *.htm", NULL, NULL},
	{"css", "CSS", "vscode-css-language-server --stdio", "*.css *.scss *.less", NULL, NULL},
	{"zls", "Zig (zls)", "zls", "*.zig *.zon", NULL, NULL},
	{"ols", "Odin (ols)", "ols", "*.odin", NULL, NULL},
	{"blitzmax", "BlitzMax NG (bls)", "bls", "*.bmx",
		"~/BlitzMax/bin ~/BlitzMax*/bin ~/.local/share/blitzmax-ng/bin ~/Applications/BlitzMax*/bin /opt/BlitzMax*/bin "
		"/usr/local/BlitzMax*/bin", "bmk"},
	{"serve-d", "D (serve-d)", "serve-d", "*.d *.di", "~/.dub/bin ~/.local/share/code-d/bin", NULL},
	{"jdtls", "Java (jdtls)", "jdtls", "*.java", NULL, NULL},
	{"csharp", "C# (csharp-ls)", "csharp-ls", "*.cs", NULL, NULL},
	{"kotlin", "Kotlin", "kotlin-language-server", "*.kt *.kts", NULL, NULL},
	{"ruby", "Ruby (ruby-lsp)", "ruby-lsp", "*.rb", NULL, NULL},
	{"php", "PHP (Intelephense)", "intelephense --stdio", "*.php", NULL, NULL},
	{"dart", "Dart", "dart language-server", "*.dart", NULL, NULL},
	{"haskell", "Haskell", "haskell-language-server-wrapper --lsp", "*.hs", NULL, NULL},
	{"marksman", "Markdown (Marksman)", "marksman server", "*.md *.markdown", NULL, NULL},
	{"yaml", "YAML", "yaml-language-server --stdio", "*.yaml *.yml", NULL, NULL},
	{"taplo", "TOML (Taplo)", "taplo lsp stdio", "*.toml", NULL, NULL},
	{"nil", "Nix (nil)", "nil", "*.nix", NULL, NULL},
	{"cmake", "CMake", "cmake-language-server", "CMakeLists.txt *.cmake", NULL, NULL},
	{"meson", "Meson", "mesonlsp --lsp", "meson.build meson.options meson_options.txt", NULL, NULL},
};

/* Language ids from the protocol specification, by file name ending */
static const struct {
	const char *ending, *language_id;
} LANGUAGE_IDS[] = {
	{".c", "c"}, {".h", "c"}, {".cc", "cpp"}, {".cpp", "cpp"}, {".cxx", "cpp"}, {".hpp", "cpp"},
	{".hh", "cpp"}, {".hxx", "cpp"}, {".m", "objective-c"}, {".mm", "objective-cpp"}, {".rs", "rust"},
	{".py", "python"}, {".pyi", "python"}, {".ts", "typescript"}, {".mts", "typescript"},
	{".cts", "typescript"}, {".tsx", "typescriptreact"}, {".js", "javascript"}, {".mjs", "javascript"},
	{".cjs", "javascript"}, {".jsx", "javascriptreact"}, {".go", "go"}, {"go.mod", "go.mod"}, {".lua", "lua"},
	{".sh", "shellscript"}, {".bash", "shellscript"}, {".json", "json"}, {".jsonc", "jsonc"},
	{".html", "html"}, {".htm", "html"}, {".css", "css"}, {".scss", "scss"}, {".less", "less"},
	{".zig", "zig"}, {".d", "d"}, {".di", "d"}, {".odin", "odin"}, {".bmx", "blitzmax"}, {".java", "java"},
	{".cs", "csharp"},
	{".kt", "kotlin"}, {".kts", "kotlin"}, {".rb", "ruby"}, {".php", "php"}, {".dart", "dart"},
	{".hs", "haskell"}, {".md", "markdown"}, {".markdown", "markdown"}, {".yaml", "yaml"}, {".yml", "yaml"},
	{".toml", "toml"}, {".nix", "nix"}, {"CMakeLists.txt", "cmake"}, {".cmake", "cmake"},
	{"meson.build", "meson"},
};

/* Folders that language installers commonly use but that are often missing from PATH for desktop apps */
static const char *const EXTRA_FOLDERS[] = {
	".local/bin", ".cargo/bin", "go/bin", ".npm-global/bin", ".bun/bin", ".deno/bin", ".dotnet/tools",
	".ghcup/bin", ".nimble/bin", "bin",
};

/* A function that hears about saved changes */
typedef struct {
	void (*on_changed)(gpointer data);
	gpointer data;
} Watcher;

static GPtrArray *servers, *watchers;


static char *get_servers_path(void) {
	return g_build_filename(g_get_user_config_dir(), "crimson-code", "language-servers.ini", NULL);
}


static void free_server(LanguageServer *server) {
	g_free(server->id);
	g_free(server->name);
	g_free(server->command);
	g_free(server->patterns);
	g_free(server->language_id);
	g_free(server->initialization_options);
	g_free(server);
}


static int find_builtin_index(const char *id) {
	for (guint index = 0; index < G_N_ELEMENTS(BUILTIN_SERVERS); index++)
		if (g_strcmp0(BUILTIN_SERVERS[index].id, id) == 0)
			return (int)index;
	return -1;
}


static void reset_server(LanguageServer *server, int builtin) {
	g_set_str(&server->name, BUILTIN_SERVERS[builtin].name);
	g_set_str(&server->command, BUILTIN_SERVERS[builtin].command);
	g_set_str(&server->patterns, BUILTIN_SERVERS[builtin].patterns);
	g_set_str(&server->language_id, "");
	g_set_str(&server->initialization_options, "");
	server->is_enabled = TRUE;
	server->is_builtin = TRUE;
}


static void read_text_key(GKeyFile *file, const char *group, const char *key, char **target) {
	g_autofree char *value = g_key_file_get_string(file, group, key, NULL);
	if (value)
		g_set_str(target, value);
}


static void apply_saved_values(LanguageServer *server, GKeyFile *file, const char *group) {
	read_text_key(file, group, "name", &server->name);
	read_text_key(file, group, "command", &server->command);
	read_text_key(file, group, "patterns", &server->patterns);
	read_text_key(file, group, "language", &server->language_id);
	read_text_key(file, group, "initialization-options", &server->initialization_options);
	g_autoptr(GError) error = NULL;
	gboolean is_enabled = g_key_file_get_boolean(file, group, "enabled", &error);
	if (!error)
		server->is_enabled = is_enabled;
}


static void load_language_servers(void) {
	servers = g_ptr_array_new_with_free_func((GDestroyNotify)free_server);
	for (guint index = 0; index < G_N_ELEMENTS(BUILTIN_SERVERS); index++) {
		LanguageServer *server = g_new0(LanguageServer, 1);
		server->id = g_strdup(BUILTIN_SERVERS[index].id);
		reset_server(server, (int)index);
		g_ptr_array_add(servers, server);
	}
	g_autofree char *path = get_servers_path();
	g_autoptr(GKeyFile) file = g_key_file_new();
	if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL))
		return;
	g_auto(GStrv) groups = g_key_file_get_groups(file, NULL);
	for (int index = 0; groups[index]; index++) {
		int builtin = find_builtin_index(groups[index]);
		LanguageServer *server = builtin >= 0 ? g_ptr_array_index(servers, builtin) : NULL;
		if (!server && !g_key_file_get_boolean(file, groups[index], CUSTOM_KEY, NULL))
			continue;
		if (!server) {
			server = g_new0(LanguageServer, 1);
			server->id = g_strdup(groups[index]);
			server->name = g_strdup(groups[index]);
			server->command = g_strdup("");
			server->patterns = g_strdup("");
			server->language_id = g_strdup("");
			server->initialization_options = g_strdup("");
			server->is_enabled = TRUE;
			g_ptr_array_add(servers, server);
		}
		apply_saved_values(server, file, groups[index]);
	}
}


GPtrArray *get_language_servers(void) {
	if (!servers)
		load_language_servers();
	return servers;
}


static gboolean is_changed_builtin(const LanguageServer *server) {
	int builtin = find_builtin_index(server->id);
	return g_strcmp0(server->name, BUILTIN_SERVERS[builtin].name) != 0
		|| g_strcmp0(server->command, BUILTIN_SERVERS[builtin].command) != 0
		|| g_strcmp0(server->patterns, BUILTIN_SERVERS[builtin].patterns) != 0
		|| *server->language_id || *server->initialization_options || !server->is_enabled;
}


/* Built in servers are written only when changed, so later versions can improve their defaults */
void save_language_servers(void) {
	g_autoptr(GKeyFile) file = g_key_file_new();
	for (guint index = 0; index < get_language_servers()->len; index++) {
		LanguageServer *server = g_ptr_array_index(servers, index);
		if (server->is_builtin && !is_changed_builtin(server))
			continue;
		g_key_file_set_string(file, server->id, "name", server->name);
		g_key_file_set_string(file, server->id, "command", server->command);
		g_key_file_set_string(file, server->id, "patterns", server->patterns);
		g_key_file_set_string(file, server->id, "language", server->language_id);
		g_key_file_set_string(file, server->id, "initialization-options", server->initialization_options);
		g_key_file_set_boolean(file, server->id, "enabled", server->is_enabled);
		if (!server->is_builtin)
			g_key_file_set_boolean(file, server->id, CUSTOM_KEY, TRUE);
	}
	g_autofree char *path = get_servers_path();
	g_autofree char *directory = g_path_get_dirname(path);
	g_autoptr(GError) error = NULL;
	if (g_mkdir_with_parents(directory, 0700) != 0 || !g_key_file_save_to_file(file, path, &error))
		g_warning("Language servers were not saved to %s", path);
	for (guint index = 0; watchers && index < watchers->len; index++) {
		Watcher *watcher = g_ptr_array_index(watchers, index);
		watcher->on_changed(watcher->data);
	}
}


void watch_language_servers(void (*on_changed)(gpointer data), gpointer data) {
	if (!watchers)
		watchers = g_ptr_array_new_with_free_func(g_free);
	Watcher *watcher = g_new0(Watcher, 1);
	watcher->on_changed = on_changed;
	watcher->data = data;
	g_ptr_array_add(watchers, watcher);
}


void unwatch_language_servers(gpointer data) {
	for (guint index = watchers ? watchers->len : 0; index > 0; index--)
		if (((Watcher *)g_ptr_array_index(watchers, index - 1))->data == data)
			g_ptr_array_remove_index(watchers, index - 1);
}


LanguageServer *add_language_server(void) {
	GPtrArray *list = get_language_servers();
	LanguageServer *server = g_new0(LanguageServer, 1);
	for (guint number = 1; !server->id; number++) {
		g_autofree char *id = g_strdup_printf("custom-%u", number);
		gboolean is_taken = FALSE;
		for (guint index = 0; index < list->len; index++)
			is_taken |= g_strcmp0(((LanguageServer *)g_ptr_array_index(list, index))->id, id) == 0;
		if (!is_taken)
			server->id = g_steal_pointer(&id);
	}
	server->name = g_strdup("");
	server->command = g_strdup("");
	server->patterns = g_strdup("");
	server->language_id = g_strdup("");
	server->initialization_options = g_strdup("");
	server->is_enabled = TRUE;
	g_ptr_array_add(list, server);
	return server;
}


void remove_language_server(LanguageServer *server) {
	int builtin = find_builtin_index(server->id);
	if (server->is_builtin)
		reset_server(server, builtin);
	else
		g_ptr_array_remove(get_language_servers(), server);
}


/* Looks in folders that may hold wildcards such as ~/BlitzMax*, newest match last as glob sorts them */
static char *find_in_folders(const char *folders, const char *program) {
	g_auto(GStrv) patterns = g_strsplit(folders ? folders : "", " ", -1);
	char *found = NULL;
	for (int index = 0; patterns[index] && !found; index++) {
		if (!*patterns[index])
			continue;
		g_autofree char *pattern = g_str_has_prefix(patterns[index], "~/")
			? g_build_filename(g_get_home_dir(), patterns[index] + 2, NULL) : g_strdup(patterns[index]);
		glob_t matches = {0};
		if (glob(pattern, GLOB_NOSORT | GLOB_ONLYDIR, NULL, &matches) == 0) {
			for (size_t match = 0; match < matches.gl_pathc && !found; match++) {
				g_autofree char *path = g_build_filename(matches.gl_pathv[match], program, NULL);
				if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
					found = g_steal_pointer(&path);
			}
		}
		globfree(&matches);
	}
	return found;
}


static char *find_next_to_companion(const char *companion, const char *program) {
	g_autofree char *companion_path = companion ? g_find_program_in_path(companion) : NULL;
	if (!companion_path)
		return NULL;
	g_autofree char *real_path = realpath(companion_path, NULL);
	g_autofree char *directory = g_path_get_dirname(real_path ? real_path : companion_path);
	g_autofree char *path = g_build_filename(directory, program, NULL);
	return g_file_test(path, G_FILE_TEST_IS_EXECUTABLE) ? g_steal_pointer(&path) : NULL;
}


char *find_language_server_program(const LanguageServer *server) {
	g_auto(GStrv) arguments = NULL;
	if (!g_shell_parse_argv(server->command, NULL, &arguments, NULL))
		return NULL;
	g_autofree char *expanded = g_str_has_prefix(arguments[0], "~/")
		? g_build_filename(g_get_home_dir(), arguments[0] + 2, NULL) : g_strdup(arguments[0]);
	if (g_path_is_absolute(expanded))
		return g_file_test(expanded, G_FILE_TEST_IS_EXECUTABLE) ? g_steal_pointer(&expanded) : NULL;
	char *found = g_find_program_in_path(expanded);
	for (guint index = 0; !found && index < G_N_ELEMENTS(EXTRA_FOLDERS); index++) {
		g_autofree char *path = g_build_filename(g_get_home_dir(), EXTRA_FOLDERS[index], expanded, NULL);
		if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
			found = g_steal_pointer(&path);
	}
	int builtin = find_builtin_index(server->id);
	if (!found && builtin >= 0)
		found = find_in_folders(BUILTIN_SERVERS[builtin].folders, expanded);
	if (!found && builtin >= 0)
		found = find_next_to_companion(BUILTIN_SERVERS[builtin].companion, expanded);
	return found;
}


gboolean matches_language_server(const LanguageServer *server, const char *file_name) {
	g_autofree char *base_name = g_path_get_basename(file_name);
	g_auto(GStrv) patterns = g_strsplit_set(server->patterns, " ;,", -1);
	for (int index = 0; patterns[index]; index++)
		if (*patterns[index] && g_pattern_match_simple(patterns[index], base_name))
			return TRUE;
	return FALSE;
}


LanguageServer *find_server_for_file(const char *file_name) {
	GPtrArray *list = get_language_servers();
	for (guint index = 0; index < list->len; index++) {
		LanguageServer *server = g_ptr_array_index(list, index);
		if (!server->is_enabled || !matches_language_server(server, file_name))
			continue;
		g_autofree char *program = find_language_server_program(server);
		if (program)
			return server;
	}
	return NULL;
}


const char *guess_language_id(const LanguageServer *server, const char *file_name) {
	if (server && *server->language_id)
		return server->language_id;
	for (guint index = 0; index < G_N_ELEMENTS(LANGUAGE_IDS); index++)
		if (g_str_has_suffix(file_name, LANGUAGE_IDS[index].ending))
			return LANGUAGE_IDS[index].language_id;
	return "plaintext";
}
