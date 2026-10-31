#pragma once

#include <gtksourceview/gtksource.h>

#include "find-bar.h"

/* The bytes of a binary file, mapped rather than read so any size opens at once, with edits kept as patches on
 * top, shared by every view of the file */
typedef struct HexDocument HexDocument;

typedef void (*HexLoadedFunction)(HexDocument *document, GError *error, gpointer data);
/* Called when the bytes or their saved state change, so titles and status can follow */
typedef void (*HexChangedFunction)(gpointer data);

/* Maps the file in a worker thread and calls back on the main thread */
void load_hex_document(GFile *file, GCancellable *cancellable, HexLoadedFunction on_loaded, gpointer data);
void free_hex_document(HexDocument *document);
gboolean is_hex_document_modified(HexDocument *document);
/* Writes the bytes with every patch to the file, or to the original one when file is NULL */
gboolean save_hex_document(HexDocument *document, GFile *file, GError **error);
/* Guesses from the start of a file whether it is binary, as a zero byte almost never appears in text */
gboolean is_binary_start(const guint8 *bytes, gsize length);

/* A view of the document as offsets, hex and characters beside a data inspector, coloured from the scheme of the
 * given buffer */
GtkWidget *create_hex_view(HexDocument *document, GtkSourceBuffer *colours, HexChangedFunction on_changed,
	gpointer data);
void free_hex_view(GtkWidget *view);
void focus_hex_view(GtkWidget *view);
/* How the find bar searches and replaces in a hex view, with the view as the target's data */
const FindTarget *get_hex_find_target(void);
