#pragma once

#include <gtksourceview/gtksource.h>

/* How large the minimap draws each character, from the smallest */
typedef enum {
	MINIMAP_SMALL,
	MINIMAP_MEDIUM,
	MINIMAP_LARGE,
} MinimapSize;

/* A picture of the whole file beside the view, with lines squeezed together when the file is taller than the map */
GtkWidget *create_minimap(GtkSourceView *view);
void set_minimap_size(GtkWidget *minimap, MinimapSize size);
/* Stops the minimap following the view, since the tab may keep the widgets a moment after the editor closes */
void disconnect_minimap(GtkWidget *minimap);
