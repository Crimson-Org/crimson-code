#pragma once

#include <gtk/gtk.h>

/* Opens a popover leftwards from its button's end, shifted so its arrow clears the rounded corner */
void place_popover_at_button_end(GtkPopover *popover);

/* Removes a popover made for one opening, once it has closed */
void remove_popover_later(GtkPopover *popover);
