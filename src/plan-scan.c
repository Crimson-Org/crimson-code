#include <string.h>

#include <gtksourceview/gtksource.h>

#include "comment-markers.h"
#include "plan-items.h"
#include "plan-scan.h"
#include "settings.h"
#include "sidebar.h"

#define SCAN_ATTRIBUTES "standard::name,standard::type,standard::size"
/* Larger files are rarely written by hand, and reading them would slow the scan */
#define MAXIMUM_FILE_BYTES (1024 * 1024)
#define SNIFF_BYTES 8192

/* How files matching a name pattern write comments */
typedef struct {
	char *pattern;
	CommentMarkers *markers;
} CommentRule;

/* Everything the scan needs, copied from the settings and languages before it moves off the main thread */
typedef struct {
	GFile *root;
	char *only_path;
	GStrv excluded, plan_names, tags;
	gboolean shows_hidden;
	GPtrArray *rules;
} PlanScan;


static void free_rule(gpointer data) {
	CommentRule *rule = data;
	g_free(rule->pattern);
	free_comment_markers(rule->markers);
	g_free(rule);
}


static void free_scan(gpointer data) {
	PlanScan *scan = data;
	g_object_unref(scan->root);
	g_free(scan->only_path);
	g_strfreev(scan->excluded);
	g_strfreev(scan->plan_names);
	g_strfreev(scan->tags);
	g_ptr_array_unref(scan->rules);
	g_free(scan);
}


static GStrv split_words(const char *text) {
	g_auto(GStrv) parts = g_strsplit_set(text, " ;,", -1);
	g_autoptr(GStrvBuilder) words = g_strv_builder_new();
	for (guint index = 0; parts[index]; index++)
		if (*parts[index])
			g_strv_builder_add(words, parts[index]);
	return g_strv_builder_end(words);
}


static gboolean matches_any(GStrv patterns, const char *name) {
	for (guint index = 0; patterns[index]; index++)
		if (g_pattern_match_simple(patterns[index], name))
			return TRUE;
	return FALSE;
}


/* GtkSourceView is only used on the main thread, so each language's name patterns are copied for the scan */
static GPtrArray *list_comment_rules(void) {
	GPtrArray *rules = g_ptr_array_new_with_free_func(free_rule);
	GtkSourceLanguageManager *manager = gtk_source_language_manager_get_default();
	const char *const *ids = gtk_source_language_manager_get_language_ids(manager);
	for (guint index = 0; ids && ids[index]; index++) {
		g_autoptr(CommentMarkers) markers = get_comment_markers(ids[index]);
		if (!has_comment_markers(markers))
			continue;
		g_auto(GStrv) patterns = gtk_source_language_get_globs(gtk_source_language_manager_get_language(manager,
			ids[index]));
		for (guint pattern = 0; patterns && patterns[pattern]; pattern++) {
			CommentRule *rule = g_new(CommentRule, 1);
			rule->pattern = g_strdup(patterns[pattern]);
			rule->markers = get_comment_markers(ids[index]);
			g_ptr_array_add(rules, rule);
		}
	}
	return rules;
}


static CommentMarkers *find_markers(PlanScan *scan, const char *name) {
	for (guint index = 0; index < scan->rules->len; index++) {
		CommentRule *rule = g_ptr_array_index(scan->rules, index);
		if (g_pattern_match_simple(rule->pattern, name))
			return rule->markers;
	}
	return NULL;
}


/* ==================================== Finding tags ==================================== */


static gboolean is_word_character(char character) {
	return g_ascii_isalnum(character) || character == '_';
}


/* The first tag written as its own word in the comment text, with the text after it */
static const char *find_tag(PlanScan *scan, const char *start, const char *end, const char **after) {
	const char *found = NULL, *found_tag = NULL;
	for (guint index = 0; scan->tags[index]; index++) {
		gsize length = strlen(scan->tags[index]);
		for (const char *place = start; place + length <= end; place++) {
			place = g_strstr_len(place, end - place, scan->tags[index]);
			if (!place || place + length > end)
				break;
			gboolean is_whole = (place == start || !is_word_character(place[-1]))
				&& (place + length == end || !is_word_character(place[length]));
			if (!is_whole)
				continue;
			if (!found || place < found) {
				found = place;
				found_tag = scan->tags[index];
				*after = place + length;
			}
			break;
		}
	}
	return found_tag;
}


/* Skips what usually follows a tag, such as "(name):" */
static char *clean_comment_text(const char *start, const char *end) {
	if (start < end && *start == '(') {
		const char *close = memchr(start, ')', end - start);
		if (close)
			start = close + 1;
	}
	while (start < end && (*start == ':' || *start == ' ' || *start == '\t' || *start == '-'))
		start++;
	g_autofree char *text = g_strndup(start, end - start);
	return g_utf8_make_valid(g_strstrip(text), -1);
}


static void find_line_tag(PlanScan *scan, PlanItem *source, const char *start, const char *end, int line) {
	const char *after = NULL;
	const char *tag = find_tag(scan, start, end, &after);
	if (!tag)
		return;
	g_autofree char *text = clean_comment_text(after, end);
	add_plan_comment(source, tag, text, line);
}


static const char *find_in_line(const char *start, const char *end, const char *marker) {
	return *marker ? g_strstr_len(start, end - start, marker) : NULL;
}


/* Goes through the comments of one line, remembering a block comment that continues on the next line */
static void scan_line(PlanScan *scan, CommentMarkers *markers, PlanItem *source, const char *start,
	const char *end, int line, gboolean *is_in_block) {
	gboolean has_blocks = *markers->block_start && *markers->block_end;
	while (start < end) {
		if (*is_in_block) {
			const char *close = find_in_line(start, end, markers->block_end);
			find_line_tag(scan, source, start, close ? close : end, line);
			if (!close)
				return;
			*is_in_block = FALSE;
			start = close + strlen(markers->block_end);
			continue;
		}
		const char *line_comment = find_in_line(start, end, markers->line);
		const char *block = has_blocks ? find_in_line(start, end, markers->block_start) : NULL;
		if (line_comment && (!block || line_comment < block)) {
			find_line_tag(scan, source, line_comment + strlen(markers->line), end, line);
			return;
		}
		if (!block)
			return;
		*is_in_block = TRUE;
		start = block + strlen(markers->block_start);
	}
}


static PlanItem *scan_source(PlanScan *scan, GFile *file, const char *base_name, const char *name,
	const char *text, gsize length) {
	CommentMarkers *markers = find_markers(scan, base_name);
	if (!markers || memchr(text, '\0', MIN(length, SNIFF_BYTES)))
		return NULL;
	PlanItem *source = create_plan_source(file, name);
	gboolean is_in_block = FALSE;
	int line = 0;
	for (const char *start = text, *end; start < text + length; start = end + 1, line++) {
		end = memchr(start, '\n', text + length - start);
		if (!end)
			end = text + length;
		scan_line(scan, markers, source, start, end, line, &is_in_block);
	}
	if (source->open_count > 0)
		return source;
	g_object_unref(source);
	return NULL;
}


/* ==================================== Visiting the project =================================== */


static void scan_file(PlanScan *scan, GFile *file, const char *base_name, goffset size, GPtrArray *found,
	GCancellable *cancellable) {
	gboolean is_plan = matches_any(scan->plan_names, base_name);
	if (size > MAXIMUM_FILE_BYTES || (!is_plan && !find_markers(scan, base_name)))
		return;
	g_autofree char *contents = NULL;
	gsize length = 0;
	if (!g_file_load_contents(file, cancellable, &contents, &length, NULL, NULL))
		return;
	g_autofree char *name = g_file_get_relative_path(scan->root, file);
	if (!name)
		name = g_strdup(base_name);
	if (!is_plan) {
		PlanItem *source = scan_source(scan, file, base_name, name, contents, length);
		if (source)
			g_ptr_array_add(found, source);
		return;
	}
	g_autofree char *text = g_utf8_make_valid(contents, length);
	g_ptr_array_add(found, parse_plan_file(file, name, text));
}


/* What scanning one found file needs besides the file */
typedef struct {
	PlanScan *scan;
	GPtrArray *found;
	GCancellable *cancellable;
} ScanVisit;


static void visit_file(GFile *file, GFileInfo *info, gpointer data) {
	ScanVisit *visit = data;
	scan_file(visit->scan, file, g_file_info_get_name(info), g_file_info_get_size(info), visit->found,
		visit->cancellable);
}


/* A saved file is only looked at when the project tree would show it too */
static gboolean is_path_shown(PlanScan *scan, const char *name) {
	g_auto(GStrv) parts = g_strsplit(name, G_DIR_SEPARATOR_S, -1);
	for (guint index = 0; parts[index]; index++)
		if (!is_project_name_shown(parts[index], scan->excluded, scan->shows_hidden))
			return FALSE;
	return TRUE;
}


static void run_scan(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	PlanScan *scan = data;
	GPtrArray *found = g_ptr_array_new_with_free_func(g_object_unref);
	if (scan->only_path) {
		g_autoptr(GFile) file = g_file_new_for_path(scan->only_path);
		g_autofree char *name = g_file_get_relative_path(scan->root, file);
		if (!name || !is_path_shown(scan, name)) {
			g_task_return_pointer(task, found, (GDestroyNotify)g_ptr_array_unref);
			return;
		}
		g_autoptr(GFileInfo) info = g_file_query_info(file, SCAN_ATTRIBUTES, G_FILE_QUERY_INFO_NONE, cancellable,
			NULL);
		if (info && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR)
			scan_file(scan, file, g_file_info_get_name(info), g_file_info_get_size(info), found, cancellable);
	} else {
		ScanVisit visit = {scan, found, cancellable};
		visit_project_files(scan->root, scan->excluded, scan->shows_hidden, SCAN_ATTRIBUTES, visit_file, &visit,
			cancellable);
	}
	g_task_return_pointer(task, found, (GDestroyNotify)g_ptr_array_unref);
}


void start_plan_scan(const char *root, const char *only_path, GCancellable *cancellable,
	GAsyncReadyCallback on_done, gpointer data) {
	PlanScan *scan = g_new0(PlanScan, 1);
	scan->root = g_file_new_for_path(root);
	scan->only_path = g_strdup(only_path);
	scan->excluded = list_excluded_project_names();
	scan->shows_hidden = get_switch_setting("show-hidden-files");
	scan->plan_names = split_words(get_text_setting("plan-files"));
	scan->tags = split_words(get_text_setting("comment-tags"));
	scan->rules = list_comment_rules();
	g_autoptr(GTask) task = g_task_new(NULL, cancellable, on_done, data);
	g_task_set_task_data(task, scan, free_scan);
	g_task_run_in_thread(task, run_scan);
}


GPtrArray *finish_plan_scan(GAsyncResult *result, GError **error) {
	return g_task_propagate_pointer(G_TASK(result), error);
}
