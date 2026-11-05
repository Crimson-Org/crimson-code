#include "menu-placement.h"
#include "common/widgets/spacing.h"

/* How far the popover moves past the button's end, which keeps its arrow on the flat edge beside the corner */
#define ARROW_CLEARANCE SPACING_NORMAL


/* Menus near the window's right edge keep clear of it, and the arrow still points at the button */
void place_popover_at_button_end(GtkPopover *popover) {
	gtk_widget_set_halign(GTK_WIDGET(popover), GTK_ALIGN_END);
	gtk_popover_set_offset(popover, ARROW_CLEARANCE, 0);
}
