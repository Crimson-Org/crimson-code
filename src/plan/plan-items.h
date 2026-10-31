#pragma once

#include <gio/gio.h>

/* What a row of the plan stands for */
typedef enum {
	/* A plan file such as TODO, whose children are its lines */
	PLAN_ITEM_PLAN_FILE,
	/* A source file whose children are its tagged comments */
	PLAN_ITEM_SOURCE_FILE,
	PLAN_ITEM_HEADING,
	PLAN_ITEM_TASK,
	/* A line without a box, such as a category label */
	PLAN_ITEM_NOTE,
	/* A comment such as // TODO: fix this */
	PLAN_ITEM_COMMENT,
} PlanItemKind;

#define PLAN_TYPE_ITEM plan_item_get_type()
G_DECLARE_FINAL_TYPE(PlanItem, plan_item, PLAN, ITEM, GObject)

/* One row of the plan, with the lines of its file it covers */
struct _PlanItem {
	GObject parent;
	PlanItemKind kind;
	/* Markdown text of a line, a comment's text or a file's path inside the project */
	char *text;
	/* The tag of a comment, such as FIXME */
	char *tag;
	GFile *file;
	/* Line numbers from 0, the last one including the item's children */
	int line, last_line;
	/* Names the row by file and the text of it and each parent, so it can be opened again after reloading */
	char *key;
	gboolean is_checked, is_archived;
	/* Set on items a later reading added, so their rows fade in once */
	gboolean is_new;
	/* Unchecked tasks of a plan file, or comments of a source file */
	guint open_count;
	/* The child items, or NULL without any, while files always have a list */
	GListStore *children;
	/* The text the file had when read, to check it is unchanged before editing it */
	char *file_text;
};

/* Reads a plan file's lines, nesting them by indentation and headings */
PlanItem *parse_plan_file(GFile *file, const char *name, const char *text);
/* Gives a file shown in the plan the lines and children of its new reading, telling its rows through "changed" */
void update_plan_file(PlanItem *file, PlanItem *fresh);
/* Whether the item is the parent or one of its children, which tells an item from an earlier reading */
gboolean contains_plan_item(PlanItem *parent, PlanItem *item);
/* A source file and its comments, which are added with add_plan_comment */
PlanItem *create_plan_source(GFile *file, const char *name);
void add_plan_comment(PlanItem *source, const char *tag, const char *text, int line);
/* Whether a file name looks like Markdown, which decides the style of new lines */
gboolean is_markdown_name(const char *name);

/* Each change takes the file's text and returns the new text, or NULL when the file changed since it was read */
char *toggle_plan_task(const char *text, PlanItem *file, PlanItem *item);
char *remove_plan_item(const char *text, PlanItem *file, PlanItem *item);
/* Moves the item and its children to the Archive section at the end, adding the section when missing */
char *archive_plan_item(const char *text, PlanItem *file, PlanItem *item);
/* Adds an unchecked task inside the parent, or at the end of the file before the archive when parent is the file */
char *add_plan_task(const char *text, PlanItem *file, PlanItem *parent, const char *task);
