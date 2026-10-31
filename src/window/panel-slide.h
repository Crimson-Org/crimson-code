#pragma once

#include <gtk/gtk.h>

/* Slides one side of a split open and closed by moving its divider, instead of the panel just appearing */
typedef struct PanelSlide PanelSlide;

/* The panel is the paned's start child, or its end child when is_end is set */
PanelSlide *create_panel_slide(GtkPaned *paned, gboolean is_end);
void free_panel_slide(PanelSlide *slide);
/* Opens the panel to the size in pixels */
void show_panel_slide(PanelSlide *slide, int size);
void hide_panel_slide(PanelSlide *slide);
/* Whether the panel is open or opening, not counting one that is closing */
gboolean is_panel_slide_shown(PanelSlide *slide);
/* The panel's size while it is fully open, for remembering it */
int get_panel_slide_size(PanelSlide *slide);
