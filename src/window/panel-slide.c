#include <adwaita.h>

#include "panel-slide.h"

#define SLIDE_MILLISECONDS 200

struct PanelSlide {
	GtkPaned *paned;
	gboolean is_end, is_hiding;
	AdwAnimation *animation;
	/* The paned's own shrink rule, lifted while sliding so the panel can pass below its smallest size */
	gboolean could_shrink;
};


static GtkWidget *get_panel(PanelSlide *slide) {
	return slide->is_end ? gtk_paned_get_end_child(slide->paned) : gtk_paned_get_start_child(slide->paned);
}


static int get_length(PanelSlide *slide) {
	GtkOrientation orientation = gtk_orientable_get_orientation(GTK_ORIENTABLE(slide->paned));
	GtkWidget *widget = GTK_WIDGET(slide->paned);
	return orientation == GTK_ORIENTATION_HORIZONTAL ? gtk_widget_get_width(widget) : gtk_widget_get_height(widget);
}


static void set_shrinkable(PanelSlide *slide, gboolean can_shrink) {
	if (slide->is_end)
		gtk_paned_set_shrink_end_child(slide->paned, can_shrink);
	else
		gtk_paned_set_shrink_start_child(slide->paned, can_shrink);
}


/* The animation runs over the panel's size, which becomes a divider position counted from the panel's side */
static void on_step(double size, gpointer data) {
	PanelSlide *slide = data;
	if (!slide->paned)
		return;
	int position = slide->is_end ? get_length(slide) - (int)size : (int)size;
	gtk_paned_set_position(slide->paned, MAX(0, position));
}


static void on_done(AdwAnimation *animation, PanelSlide *slide) {
	if (!slide->paned)
		return;
	set_shrinkable(slide, slide->could_shrink);
	if (slide->is_hiding)
		gtk_widget_set_visible(get_panel(slide), FALSE);
	slide->is_hiding = FALSE;
}


PanelSlide *create_panel_slide(GtkPaned *paned, gboolean is_end) {
	PanelSlide *slide = g_new0(PanelSlide, 1);
	slide->paned = paned;
	/* The window takes the paned down before the slide is freed, and a running slide may still step meanwhile */
	g_object_add_weak_pointer(G_OBJECT(paned), (gpointer *)&slide->paned);
	slide->is_end = is_end;
	slide->could_shrink = is_end ? gtk_paned_get_shrink_end_child(paned) : gtk_paned_get_shrink_start_child(paned);
	AdwAnimationTarget *target = adw_callback_animation_target_new(on_step, slide, NULL);
	slide->animation = adw_timed_animation_new(GTK_WIDGET(paned), 0, 0, SLIDE_MILLISECONDS, target);
	adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(slide->animation), ADW_EASE_OUT_CUBIC);
	g_signal_connect(slide->animation, "done", G_CALLBACK(on_done), slide);
	return slide;
}


void free_panel_slide(PanelSlide *slide) {
	g_signal_handlers_disconnect_by_data(slide->animation, slide);
	if (slide->paned)
		g_object_remove_weak_pointer(G_OBJECT(slide->paned), (gpointer *)&slide->paned);
	adw_animation_reset(slide->animation);
	g_object_unref(slide->animation);
	g_free(slide);
}


int get_panel_slide_size(PanelSlide *slide) {
	int position = gtk_paned_get_position(slide->paned);
	return slide->is_end ? get_length(slide) - position : position;
}


static void slide_to(PanelSlide *slide, double from, double to) {
	AdwTimedAnimation *animation = ADW_TIMED_ANIMATION(slide->animation);
	set_shrinkable(slide, TRUE);
	adw_timed_animation_set_value_from(animation, from);
	adw_timed_animation_set_value_to(animation, to);
	/* Resetting stops a slide still running the other way */
	adw_animation_reset(slide->animation);
	adw_animation_play(slide->animation);
}


void show_panel_slide(PanelSlide *slide, int size) {
	GtkWidget *panel = get_panel(slide);
	gboolean was_shown = gtk_widget_get_visible(panel);
	double from = was_shown ? get_panel_slide_size(slide) : 0;
	slide->is_hiding = FALSE;
	gtk_widget_set_visible(panel, TRUE);
	on_step(from, slide);
	slide_to(slide, from, size);
}


void hide_panel_slide(PanelSlide *slide) {
	if (!gtk_widget_get_visible(get_panel(slide)) || slide->is_hiding)
		return;
	slide->is_hiding = TRUE;
	slide_to(slide, get_panel_slide_size(slide), 0);
}


gboolean is_panel_slide_shown(PanelSlide *slide) {
	return gtk_widget_get_visible(get_panel(slide)) && !slide->is_hiding;
}
