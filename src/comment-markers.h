#pragma once

#include <glib.h>

/* How a language writes comments, with empty text where it has no such comment */
typedef struct {
	char *line, *block_start, *block_end;
} CommentMarkers;

/* Adds the comment markers of each language to the settings window */
void register_comment_markers(void);
/* The markers of a GtkSourceView language id, the person's own taking the place of the built in ones */
CommentMarkers *get_comment_markers(const char *language_id);
void free_comment_markers(CommentMarkers *markers);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(CommentMarkers, free_comment_markers)
/* Whether the language has any comment, so files without one can be skipped */
gboolean has_comment_markers(const CommentMarkers *markers);
void watch_comment_markers(void (*on_changed)(gpointer data), gpointer data);
void unwatch_comment_markers(gpointer data);
