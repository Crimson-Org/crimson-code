#include <string.h>

#include "plan-items.h"

#define TAB_WIDTH 4
#define ARCHIVE_NAME "Archive"
#define MAXIMUM_HEADING 6

/* A parent still open for the lines that follow, by indentation or heading level */
typedef struct {
	PlanItem *item;
	int indent, heading;
} OpenParent;

G_DEFINE_FINAL_TYPE(PlanItem, plan_item, G_TYPE_OBJECT)


static void plan_item_finalize(GObject *object) {
	PlanItem *item = PLAN_ITEM(object);
	g_free(item->text);
	g_free(item->tag);
	g_free(item->key);
	g_free(item->file_text);
	g_clear_object(&item->file);
	g_clear_object(&item->children);
	G_OBJECT_CLASS(plan_item_parent_class)->finalize(object);
}


static void plan_item_class_init(PlanItemClass *class) {
	G_OBJECT_CLASS(class)->finalize = plan_item_finalize;
	g_signal_new("changed", PLAN_TYPE_ITEM, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}


static void plan_item_init(PlanItem *item) {
}


static PlanItem *create_item(PlanItemKind kind, GFile *file, const char *text, int line) {
	PlanItem *item = g_object_new(PLAN_TYPE_ITEM, NULL);
	item->kind = kind;
	item->file = g_object_ref(file);
	item->text = g_strdup(text);
	item->line = item->last_line = line;
	return item;
}


static void add_child(PlanItem *parent, PlanItem *child) {
	if (!parent->children)
		parent->children = g_list_store_new(PLAN_TYPE_ITEM);
	child->key = g_strconcat(parent->key, "\n", child->text, NULL);
	g_list_store_append(parent->children, child);
	g_object_unref(child);
}


/* ==================================== Reading lines ==================================== */


static const char *skip_space(const char *text) {
	while (*text == ' ' || *text == '\t')
		text++;
	return text;
}


static int measure_indent(const char *line) {
	int width = 0;
	for (; *line == ' ' || *line == '\t'; line++)
		width += *line == '\t' ? TAB_WIDTH - width % TAB_WIDTH : 1;
	return width;
}


/* Steps past a Markdown list mark such as "- ", "* " or "1. " */
static const char *skip_list_mark(const char *text) {
	if ((*text == '-' || *text == '*' || *text == '+') && text[1] == ' ')
		return text + 2;
	const char *digits = text;
	while (g_ascii_isdigit(*digits))
		digits++;
	if (digits > text && (*digits == '.' || *digits == ')') && digits[1] == ' ')
		return digits + 2;
	return text;
}


/* Reads a box such as "[ ]" or "[x]" and gives the text after it */
static gboolean read_box(const char *text, gboolean *is_checked, const char **rest) {
	if (text[0] != '[' || !strchr(" xX", text[1]) || !text[1] || text[2] != ']')
		return FALSE;
	*is_checked = text[1] != ' ';
	*rest = skip_space(text + 3);
	return TRUE;
}


static int count_heading(const char *text) {
	int level = 0;
	while (text[level] == '#' && level <= MAXIMUM_HEADING)
		level++;
	return level > 0 && level <= MAXIMUM_HEADING && (text[level] == ' ' || !text[level]) ? level : 0;
}


static PlanItem *read_line(GFile *file, const char *content, int line, int heading) {
	if (heading)
		return create_item(PLAN_ITEM_HEADING, file, skip_space(content + heading), line);
	const char *body = skip_list_mark(content), *rest = NULL;
	gboolean is_checked = FALSE;
	if (!read_box(body, &is_checked, &rest))
		return create_item(PLAN_ITEM_NOTE, file, content, line);
	PlanItem *item = create_item(PLAN_ITEM_TASK, file, rest, line);
	item->is_checked = is_checked;
	return item;
}


static gboolean is_archive_name(const char *text) {
	g_autofree char *name = g_strdup(text);
	g_strchomp(name);
	if (g_str_has_suffix(name, ":"))
		name[strlen(name) - 1] = '\0';
	return g_ascii_strcasecmp(name, ARCHIVE_NAME) == 0;
}


static void mark_archived(PlanItem *item) {
	item->is_archived = TRUE;
	for (guint index = 0; item->children && index < g_list_model_get_n_items(G_LIST_MODEL(item->children)); index++) {
		g_autoptr(PlanItem) child = g_list_model_get_item(G_LIST_MODEL(item->children), index);
		mark_archived(child);
	}
}


static guint count_open_tasks(PlanItem *item) {
	guint count = item->kind == PLAN_ITEM_TASK && !item->is_checked && !item->is_archived;
	for (guint index = 0; item->children && index < g_list_model_get_n_items(G_LIST_MODEL(item->children)); index++) {
		g_autoptr(PlanItem) child = g_list_model_get_item(G_LIST_MODEL(item->children), index);
		count += count_open_tasks(child);
	}
	return count;
}


/* The archive is a top level line or a heading at any level called Archive */
static PlanItem *find_archive_section(PlanItem *parent, gboolean is_top) {
	for (guint index = 0; parent->children && index < g_list_model_get_n_items(G_LIST_MODEL(parent->children));
		index++) {
		g_autoptr(PlanItem) child = g_list_model_get_item(G_LIST_MODEL(parent->children), index);
		gboolean is_section = child->kind == PLAN_ITEM_HEADING || (is_top && child->kind == PLAN_ITEM_NOTE);
		if (is_section && is_archive_name(child->text))
			return child;
		PlanItem *found = child->kind == PLAN_ITEM_HEADING ? find_archive_section(child, FALSE) : NULL;
		if (found)
			return found;
	}
	return NULL;
}


static PlanItem *find_archive(PlanItem *file) {
	PlanItem *archive = find_archive_section(file, TRUE);
	return archive && archive->is_archived ? archive : NULL;
}


/* Headings close at the next heading of the same level or higher, other lines at the next line indented as little */
PlanItem *parse_plan_file(GFile *file, const char *name, const char *text) {
	PlanItem *root = create_item(PLAN_ITEM_PLAN_FILE, file, name, 0);
	root->key = g_file_get_path(file);
	root->file_text = g_strdup(text);
	root->children = g_list_store_new(PLAN_TYPE_ITEM);
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	g_autoptr(GArray) parents = g_array_new(FALSE, FALSE, sizeof(OpenParent));
	g_array_append_val(parents, ((OpenParent){root, -1, 0}));
	for (int line = 0; lines[line]; line++) {
		g_strchomp(lines[line]);
		const char *content = skip_space(lines[line]);
		if (!*content)
			continue;
		int indent = measure_indent(lines[line]), heading = count_heading(content);
		for (OpenParent *top = &g_array_index(parents, OpenParent, parents->len - 1); parents->len > 1;
			top = &g_array_index(parents, OpenParent, parents->len - 1)) {
			gboolean is_closed = heading ? !top->heading || top->heading >= heading
				: !top->heading && top->indent >= indent;
			if (!is_closed)
				break;
			g_array_set_size(parents, parents->len - 1);
		}
		PlanItem *item = read_line(file, content, line, heading);
		add_child(g_array_index(parents, OpenParent, parents->len - 1).item, item);
		g_array_append_val(parents, ((OpenParent){item, heading ? -1 : indent, heading}));
		for (guint index = 0; index < parents->len; index++)
			g_array_index(parents, OpenParent, index).item->last_line = line;
	}
	PlanItem *archive = find_archive_section(root, TRUE);
	if (archive)
		mark_archived(archive);
	root->open_count = count_open_tasks(root);
	return root;
}


static void merge_children(GListStore *shown, GListStore *fresh);


/* The same line in both readings, which keeps its row, while a change in having children needs a new row */
static gboolean is_same_item(PlanItem *shown, PlanItem *fresh) {
	return shown->kind == fresh->kind && g_strcmp0(shown->key, fresh->key) == 0
		&& !shown->children == !fresh->children;
}


static void copy_item_state(PlanItem *shown, PlanItem *fresh) {
	gboolean is_changed = shown->is_checked != fresh->is_checked || shown->is_archived != fresh->is_archived
		|| shown->open_count != fresh->open_count || shown->line != fresh->line;
	shown->line = fresh->line;
	shown->last_line = fresh->last_line;
	shown->is_checked = fresh->is_checked;
	shown->is_archived = fresh->is_archived;
	shown->open_count = fresh->open_count;
	if (shown->children)
		merge_children(shown->children, fresh->children);
	if (is_changed)
		g_signal_emit_by_name(shown, "changed");
}


static PlanItem *get_child(GListStore *store, guint index) {
	PlanItem *item = g_list_model_get_item(G_LIST_MODEL(store), index);
	g_object_unref(item);
	return item;
}


/* Rows that match at the start and end keep their widgets, so only the lines that really changed are replaced */
static void merge_children(GListStore *shown, GListStore *fresh) {
	guint shown_count = g_list_model_get_n_items(G_LIST_MODEL(shown));
	guint fresh_count = g_list_model_get_n_items(G_LIST_MODEL(fresh));
	guint start = 0, end = 0;
	while (start < shown_count && start < fresh_count && is_same_item(get_child(shown, start), get_child(fresh, start)))
		start++;
	while (end < shown_count - start && end < fresh_count - start
		&& is_same_item(get_child(shown, shown_count - end - 1), get_child(fresh, fresh_count - end - 1)))
		end++;
	for (guint index = 0; index < start; index++)
		copy_item_state(get_child(shown, index), get_child(fresh, index));
	for (guint index = 0; index < end; index++)
		copy_item_state(get_child(shown, shown_count - end + index), get_child(fresh, fresh_count - end + index));
	g_autoptr(GPtrArray) added = g_ptr_array_new_with_free_func(g_object_unref);
	for (guint index = start; index < fresh_count - end; index++) {
		PlanItem *item = g_list_model_get_item(G_LIST_MODEL(fresh), index);
		item->is_new = TRUE;
		g_ptr_array_add(added, item);
	}
	if (start < shown_count - end || added->len > 0)
		g_list_store_splice(shown, start, shown_count - end - start, added->pdata, added->len);
}


/* A plan file read again with the same text needs no change, while source files keep no text to compare */
void update_plan_file(PlanItem *file, PlanItem *fresh) {
	if (file->file_text && g_strcmp0(file->file_text, fresh->file_text) == 0)
		return;
	g_set_str(&file->text, fresh->text);
	g_set_str(&file->file_text, fresh->file_text);
	file->open_count = fresh->open_count;
	merge_children(file->children, fresh->children);
	g_signal_emit_by_name(file, "changed");
}


gboolean contains_plan_item(PlanItem *parent, PlanItem *item) {
	if (parent == item)
		return TRUE;
	for (guint index = 0; parent->children && index < g_list_model_get_n_items(G_LIST_MODEL(parent->children));
		index++) {
		g_autoptr(PlanItem) child = g_list_model_get_item(G_LIST_MODEL(parent->children), index);
		if (contains_plan_item(child, item))
			return TRUE;
	}
	return FALSE;
}


PlanItem *create_plan_source(GFile *file, const char *name) {
	PlanItem *source = create_item(PLAN_ITEM_SOURCE_FILE, file, name, 0);
	source->key = g_file_get_path(file);
	source->children = g_list_store_new(PLAN_TYPE_ITEM);
	return source;
}


void add_plan_comment(PlanItem *source, const char *tag, const char *text, int line) {
	PlanItem *comment = create_item(PLAN_ITEM_COMMENT, source->file, text, line);
	comment->tag = g_strdup(tag);
	add_child(source, comment);
	source->open_count++;
}


gboolean is_markdown_name(const char *name) {
	return g_str_has_suffix(name, ".md") || g_str_has_suffix(name, ".markdown");
}


/* ==================================== Changing the file ==================================== */


static gboolean has_heading(PlanItem *file) {
	for (guint index = 0; file->children && index < g_list_model_get_n_items(G_LIST_MODEL(file->children)); index++) {
		g_autoptr(PlanItem) child = g_list_model_get_item(G_LIST_MODEL(file->children), index);
		if (child->kind == PLAN_ITEM_HEADING)
			return TRUE;
	}
	return FALSE;
}


static gboolean is_markdown_file(PlanItem *file) {
	return is_markdown_name(file->text) || has_heading(file);
}


static char *get_leading_space(const char *line) {
	return g_strndup(line, skip_space(line) - line);
}


static char *get_list_mark(const char *line) {
	const char *content = skip_space(line);
	return g_strndup(content, skip_list_mark(content) - content);
}


static PlanItem *find_first_task(PlanItem *item) {
	for (guint index = 0; item->children && index < g_list_model_get_n_items(G_LIST_MODEL(item->children)); index++) {
		g_autoptr(PlanItem) child = g_list_model_get_item(G_LIST_MODEL(item->children), index);
		if (child->kind == PLAN_ITEM_TASK && !child->is_archived)
			return child;
		PlanItem *found = find_first_task(child);
		if (found)
			return found;
	}
	return NULL;
}


/* New tasks copy the list mark of the file's first task, such as "- ", so the file keeps one style */
static char *get_file_list_mark(GStrv lines, PlanItem *file) {
	PlanItem *task = find_first_task(file);
	if (task)
		return get_list_mark(lines[task->line]);
	return g_strdup(is_markdown_file(file) ? "- " : "");
}


/* One step of indentation as the file already writes it, or a tab */
static char *get_indent_step(GStrv lines) {
	for (int line = 0; lines[line]; line++) {
		const char *content = skip_space(lines[line]);
		if (content == lines[line] || !*content)
			continue;
		return lines[line][0] == '\t' ? g_strdup("\t") : g_strndup(lines[line], content - lines[line]);
	}
	return g_strdup("\t");
}


static PlanItem *find_first_line_child(PlanItem *item) {
	for (guint index = 0; item->children && index < g_list_model_get_n_items(G_LIST_MODEL(item->children)); index++) {
		g_autoptr(PlanItem) child = g_list_model_get_item(G_LIST_MODEL(item->children), index);
		if (child->kind != PLAN_ITEM_HEADING)
			return child;
	}
	return NULL;
}


static PlanItem *find_first_task_child(PlanItem *item) {
	for (guint index = 0; item->children && index < g_list_model_get_n_items(G_LIST_MODEL(item->children)); index++) {
		g_autoptr(PlanItem) child = g_list_model_get_item(G_LIST_MODEL(item->children), index);
		if (child->kind == PLAN_ITEM_TASK)
			return child;
	}
	return NULL;
}


static gboolean is_blank(const char *line) {
	return !*skip_space(line);
}


static int find_last_content_line(GStrv lines) {
	int last = -1;
	for (int line = 0; lines[line]; line++)
		if (!is_blank(lines[line]))
			last = line;
	return last;
}


static char *join_lines(GPtrArray *lines) {
	g_ptr_array_add(lines, NULL);
	return g_strjoinv("\n", (GStrv)lines->pdata);
}


static gboolean is_unchanged(const char *text, PlanItem *file) {
	return g_strcmp0(text, file->file_text) == 0;
}


char *toggle_plan_task(const char *text, PlanItem *file, PlanItem *item) {
	if (!is_unchanged(text, file) || item->kind != PLAN_ITEM_TASK)
		return NULL;
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	char *box = (char *)skip_list_mark(skip_space(lines[item->line]));
	box[1] = item->is_checked ? ' ' : 'x';
	return g_strjoinv("\n", lines);
}


char *remove_plan_item(const char *text, PlanItem *file, PlanItem *item) {
	if (!is_unchanged(text, file))
		return NULL;
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	g_autoptr(GPtrArray) kept = g_ptr_array_new();
	for (int line = 0; lines[line]; line++)
		if (line < item->line || line > item->last_line)
			g_ptr_array_add(kept, lines[line]);
	return join_lines(kept);
}


/* The item's lines with its own indentation swapped for the archive's */
static void add_moved_lines(GPtrArray *out, GStrv lines, PlanItem *item, const char *indent, GPtrArray *owned) {
	g_autofree char *base = get_leading_space(lines[item->line]);
	for (int line = item->line; line <= item->last_line; line++) {
		const char *rest = g_str_has_prefix(lines[line], base) ? lines[line] + strlen(base) : lines[line];
		char *moved = is_blank(rest) ? g_strdup("") : g_strconcat(indent, rest, NULL);
		g_ptr_array_add(owned, moved);
		g_ptr_array_add(out, moved);
	}
}


char *archive_plan_item(const char *text, PlanItem *file, PlanItem *item) {
	if (!is_unchanged(text, file) || item->is_archived)
		return NULL;
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	PlanItem *archive = find_archive(file);
	PlanItem *first_archived = archive ? find_first_line_child(archive) : NULL;
	gboolean is_markdown = is_markdown_file(file);
	g_autofree char *indent = first_archived ? get_leading_space(lines[first_archived->line])
		: g_strdup(is_markdown ? "" : "\t");
	g_autoptr(GPtrArray) out = g_ptr_array_new(), owned = g_ptr_array_new_with_free_func(g_free);
	int last_content = find_last_content_line(lines);
	for (int line = 0; lines[line]; line++) {
		gboolean is_moved = line >= item->line && line <= item->last_line;
		if (!is_moved && (archive || line <= last_content))
			g_ptr_array_add(out, lines[line]);
		if (archive && line == archive->last_line)
			add_moved_lines(out, lines, item, indent, owned);
	}
	if (!archive) {
		g_ptr_array_add(out, "");
		g_ptr_array_add(out, is_markdown ? "## " ARCHIVE_NAME : ARCHIVE_NAME);
		add_moved_lines(out, lines, item, indent, owned);
		g_ptr_array_add(out, "");
	}
	return join_lines(out);
}


/* The line after a parent's own lines, which come before any subheadings it has */
static int find_end_of_lines(PlanItem *parent) {
	int end = parent->line;
	for (guint index = 0; parent->children && index < g_list_model_get_n_items(G_LIST_MODEL(parent->children));
		index++) {
		g_autoptr(PlanItem) child = g_list_model_get_item(G_LIST_MODEL(parent->children), index);
		if (child->kind == PLAN_ITEM_HEADING)
			break;
		end = child->last_line;
	}
	return end + 1;
}


/* Where a new task goes and how it is indented and marked, matching the lines around it */
static int place_new_task(GStrv lines, PlanItem *file, PlanItem *parent, char **indent, char **mark) {
	if (parent == file) {
		*indent = g_strdup("");
		*mark = get_file_list_mark(lines, file);
		PlanItem *archive = find_archive(file);
		if (!archive)
			return find_last_content_line(lines) + 1;
		int line = archive->line;
		while (line > 0 && is_blank(lines[line - 1]))
			line--;
		return line;
	}
	PlanItem *first_child = find_first_task_child(parent);
	if (first_child) {
		*indent = get_leading_space(lines[first_child->line]);
		*mark = get_list_mark(lines[first_child->line]);
	} else if ((first_child = find_first_line_child(parent))) {
		*indent = get_leading_space(lines[first_child->line]);
		*mark = get_file_list_mark(lines, file);
	} else if (parent->kind == PLAN_ITEM_HEADING) {
		*indent = g_strdup("");
		*mark = get_file_list_mark(lines, file);
	} else {
		g_autofree char *base = get_leading_space(lines[parent->line]), *step = get_indent_step(lines);
		g_autofree char *parent_mark = get_list_mark(lines[parent->line]);
		*indent = g_strconcat(base, step, NULL);
		*mark = *parent_mark ? g_strdup(parent_mark) : get_file_list_mark(lines, file);
	}
	return find_end_of_lines(parent);
}


char *add_plan_task(const char *text, PlanItem *file, PlanItem *parent, const char *task) {
	if (!is_unchanged(text, file))
		return NULL;
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	g_autofree char *indent = NULL, *mark = NULL;
	int place = place_new_task(lines, file, parent, &indent, &mark);
	g_autofree char *one_line = g_strdelimit(g_strdup(task), "\r\n", ' ');
	g_autofree char *added = g_strconcat(indent, mark, "[ ] ", g_strstrip(one_line), NULL);
	g_autoptr(GPtrArray) out = g_ptr_array_new();
	int count = g_strv_length(lines);
	for (int line = 0; line <= count; line++) {
		if (line == place)
			g_ptr_array_add(out, added);
		if (line < count)
			g_ptr_array_add(out, lines[line]);
	}
	return join_lines(out);
}
