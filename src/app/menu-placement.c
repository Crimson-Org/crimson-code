#include "menu-placement.h"
#include "common/widgets/spacing.h"

/* How far the popover moves past the button's end, which keeps its arrow on the flat edge beside the corner */
#define ARROW_CLEARANCE SPACING_NORMAL


/* Menus near the window's right edge keep clear of it, and the arrow still points at the button */
void place_popover_at_button_end(GtkPopover *popover) {
	gtk_widget_set_halign(GTK_WIDGET(popover), GTK_ALIGN_END);
	gtk_popover_set_offset(popover, ARROW_CLEARANCE, 0);
}


static void remove_popover(gpointer popover) {
	/* A parent that went meanwhile, such as a list row its own menu removed, has let go of the popover already */
	if (gtk_widget_get_parent(popover))
		gtk_widget_unparent(popover);
	g_object_unref(popover);
}


/* A closed menu still runs the item that closed it, so the popover goes once that is done, and is held until then */
void remove_popover_later(GtkPopover *popover) {
	g_idle_add_once(remove_popover, g_object_ref(popover));
}
