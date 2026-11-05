#pragma once

#include <gtk/gtk.h>

/* Opens a popover leftwards from its button's end, shifted so its arrow clears the rounded corner */
void place_popover_at_button_end(GtkPopover *popover);
