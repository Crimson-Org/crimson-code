#include <string.h>

#include "../lang/en.h"
#include "command-palette.h"
#include "settings.h"
#include "sidebar.h"
#include "window-private.h"

#define PALETTE_WIDTH 640
#define PALETTE_HEIGHT 480
#define PALETTE_MARGIN 12
#define ROW_SPACING 12
#define MAXIMUM_ROWS 100
/* Stops collecting in a huge folder, such as a home folder opened as a project */
#define MAXIMUM_FILES 200000
#define LISTING_ATTRIBUTES "standard::name,standard::type"
#define WORD_BREAKS "/\\_-. "
/* A match inside the file's own name ranks above one that needs its folders */
#define NAME_MATCH_BONUS 1000
#define OPEN_FILE_BONUS 50
#define RUN_BONUS 5
#define WORD_START_BONUS 8

/* The open palette and the project files it searches */
typedef struct {
	Window *window;
	GtkWidget *dialog, *entry, *scroller, *list, *placeholder;
	/* Paths inside the project, or NULL while they are still being read */
	GPtrArray *files;
	GCancellable *cancellable;
	/* Where the pointer was when the palette appeared, in window surface coordinates */
	graphene_point_t pointer;
	gboolean has_pointer;
} Palette;

/* A row waiting to be shown, with how well it matched and its place in its source list */
typedef struct {
	int score;
	gsize length;
	guint order;
	gconstpointer item;
} Match;

/* What collecting the project's file paths away from the main thread needs */
typedef struct {
	GFile *root;
	GStrv excluded;
	gboolean shows_hidden;
	GPtrArray *paths;
} FileListing;


/* ==================================== Matching ==================================== */


static gboolean starts_word(const char *text, const char *position) {
	if (position == text || strchr(WORD_BREAKS, position[-1]))
		return TRUE;
	return g_ascii_isupper(*position) && g_ascii_islower(position[-1]);
}


/* How well the typed letters appear in order, higher for letters side by side and at word starts, or -1 when some
 * letter is missing */
static int score_match(const char *text, const char *query) {
	int score = 0;
	const char *position = text, *previous = NULL;
	for (const char *wanted = query; *wanted; wanted++) {
		if (*wanted == ' ')
			continue;
		while (*position && g_ascii_tolower(*position) != g_ascii_tolower(*wanted))
			position++;
		if (!*position)
			return -1;
		score += 1 + (previous && position == previous + 1 ? RUN_BONUS : 0)
			+ (starts_word(text, position) ? WORD_START_BONUS : 0);
		previous = position++;
	}
	return score;
}


static int compare_matches(gconstpointer first, gconstpointer second) {
	const Match *one = first, *other = second;
	if (one->score != other->score)
		return other->score - one->score;
	if (one->length != other->length)
		return one->length < other->length ? -1 : 1;
	return one->order < other->order ? -1 : one->order > other->order;
}


static void add_match(GArray *matches, int score, const char *text, gconstpointer item) {
	if (score < 0)
		return;
	Match match = {score, strlen(text), matches->len, item};
	g_array_append_val(matches, match);
}


/* ==================================== Rows ==================================== */


static GtkWidget *add_row(Palette *palette, const char *title, const char *detail, float detail_alignment) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, ROW_SPACING);
	GtkWidget *name = gtk_label_new(title);
	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(box), name);
	GtkWidget *more = gtk_label_new(detail);
	gtk_label_set_xalign(GTK_LABEL(more), detail_alignment);
	gtk_label_set_ellipsize(GTK_LABEL(more), PANGO_ELLIPSIZE_START);
	gtk_widget_set_hexpand(more, TRUE);
	gtk_widget_add_css_class(more, "dim-label");
	gtk_box_append(GTK_BOX(box), more);
	GtkWidget *row = gtk_list_box_row_new();
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	gtk_list_box_append(GTK_LIST_BOX(palette->list), row);
	return row;
}


/* The name shown for a file, which is its path inside the project when it lies there */
static char *get_file_name(Window *window, GFile *file) {
	g_autoptr(GFile) root = window->project ? g_file_new_for_path(window->project) : NULL;
	char *name = root ? g_file_get_relative_path(root, file) : NULL;
	return name ? name : g_file_get_path(file);
}


static void add_file_row(Palette *palette, const char *name) {
	const char *slash = strrchr(name, G_DIR_SEPARATOR);
	g_autofree char *folder = slash ? g_strndup(name, slash - name) : g_strdup("");
	GtkWidget *row = add_row(palette, slash ? slash + 1 : name, folder, 0);
	char *path = g_path_is_absolute(name) ? g_strdup(name) : g_build_filename(palette->window->project, name, NULL);
	g_object_set_data_full(G_OBJECT(row), "path", path, g_free);
}


static int score_file(const char *name, const char *query) {
	const char *slash = strrchr(name, G_DIR_SEPARATOR);
	int score = score_match(slash ? slash + 1 : name, query);
	return score >= 0 ? score + NAME_MATCH_BONUS : score_match(name, query);
}


/* Open files come first, then the project's files, best matches first once something is typed */
static void add_file_rows(Palette *palette, const char *query) {
	g_autoptr(GPtrArray) open_names = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GHashTable) is_open = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(GPtrArray) editors = list_window_editors(palette->window);
	for (guint index = 0; index < editors->len; index++) {
		GFile *file = get_editor_file(g_ptr_array_index(editors, index));
		char *name = file ? get_file_name(palette->window, file) : NULL;
		if (!name || g_hash_table_contains(is_open, name)) {
			g_free(name);
			continue;
		}
		g_ptr_array_add(open_names, name);
		g_hash_table_add(is_open, name);
	}
	g_autoptr(GArray) matches = g_array_new(FALSE, FALSE, sizeof(Match));
	for (guint index = 0; index < open_names->len; index++) {
		const char *name = g_ptr_array_index(open_names, index);
		int score = score_file(name, query);
		add_match(matches, score < 0 ? score : score + OPEN_FILE_BONUS, name, name);
	}
	for (guint index = 0; palette->files && index < palette->files->len; index++) {
		const char *name = g_ptr_array_index(palette->files, index);
		if (!g_hash_table_contains(is_open, name))
			add_match(matches, score_file(name, query), name, name);
	}
	if (*query)
		g_array_sort(matches, compare_matches);
	for (guint index = 0; index < matches->len && index < MAXIMUM_ROWS; index++)
		add_file_row(palette, g_array_index(matches, Match, index).item);
}


static gboolean can_run(Palette *palette, const Command *command) {
	return !command->is_available || command->is_available(palette->window);
}


static char *get_shortcut_label(const Command *command) {
	guint key = 0;
	GdkModifierType modifiers = 0;
	if (!command->shortcuts[0] || !gtk_accelerator_parse(command->shortcuts[0], &key, &modifiers))
		return g_strdup("");
	return gtk_accelerator_get_label(key, modifiers);
}


static void add_command_rows(Palette *palette, const char *query) {
	GPtrArray *commands = list_commands();
	g_autoptr(GArray) matches = g_array_new(FALSE, FALSE, sizeof(Match));
	for (guint index = 0; index < commands->len; index++) {
		const Command *command = g_ptr_array_index(commands, index);
		if (can_run(palette, command))
			add_match(matches, score_match(command->label, query), command->label, command);
	}
	if (*query)
		g_array_sort(matches, compare_matches);
	for (guint index = 0; index < matches->len; index++) {
		const Command *command = g_array_index(matches, Match, index).item;
		g_autofree char *shortcut = get_shortcut_label(command);
		GtkWidget *row = add_row(palette, command->label, shortcut, 1);
		g_object_set_data(G_OBJECT(row), "command", (gpointer)command);
	}
}


static gboolean has_vim_name(const Command *command, const char *name) {
	for (int index = 0; index < 4 && command->vim_names[index]; index++)
		if (strcmp(command->vim_names[index], name) == 0)
			return TRUE;
	return FALSE;
}


/* The first : name of the command that starts with the typed one, or NULL */
static const char *find_vim_name(const Command *command, const char *typed) {
	for (int index = 0; index < 4 && command->vim_names[index]; index++)
		if (g_str_has_prefix(command->vim_names[index], typed))
			return command->vim_names[index];
	return NULL;
}


/* Lists the commands with a : name starting with the typed one, and running a row keeps the typed ! and argument */
static void add_command_line_rows(Palette *palette, const char *text) {
	g_autofree char *typed = g_strstrip(g_strdup(text));
	char *space = strpbrk(typed, " \t");
	const char *argument = space ? g_strstrip(space + 1) : "";
	if (space)
		*space = '\0';
	gboolean is_forced = g_str_has_suffix(typed, "!");
	if (is_forced)
		typed[strlen(typed) - 1] = '\0';
	GPtrArray *commands = list_commands();
	for (guint index = 0; index < commands->len; index++) {
		const Command *command = g_ptr_array_index(commands, index);
		const char *name = find_vim_name(command, typed);
		if (!name || !can_run(palette, command))
			continue;
		/* A full name the person typed, such as w for write, is kept as typed */
		const char *shown = has_vim_name(command, typed) ? typed : name;
		g_autofree char *line = g_strdup_printf("%s%s%s%s%s", COMMAND_LINE_PREFIX, shown, is_forced ? "!" : "",
			*argument ? " " : "", argument);
		GtkWidget *row = add_row(palette, line, command->label, 1);
		g_object_set_data_full(G_OBJECT(row), "line", g_steal_pointer(&line), g_free);
	}
}


static void fill_rows(Palette *palette) {
	const char *text = gtk_editable_get_text(GTK_EDITABLE(palette->entry));
	gboolean is_command = g_str_has_prefix(text, COMMAND_PALETTE_PREFIX);
	gboolean is_command_line = g_str_has_prefix(text, COMMAND_LINE_PREFIX);
	gtk_list_box_remove_all(GTK_LIST_BOX(palette->list));
	g_autofree char *query = g_strstrip(g_strdup(text + (is_command ? strlen(COMMAND_PALETTE_PREFIX) : 0)));
	if (is_command)
		add_command_rows(palette, query);
	else if (is_command_line)
		add_command_line_rows(palette, text + strlen(COMMAND_LINE_PREFIX));
	else
		add_file_rows(palette, query);
	gboolean is_reading = !is_command && !is_command_line && palette->window->project && !palette->files;
	GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(palette->list), 0);
	gtk_label_set_text(GTK_LABEL(palette->placeholder), is_reading ? TEXT_PALETTE_READING : TEXT_PALETTE_NO_MATCHES);
	gtk_widget_set_visible(palette->placeholder, !first);
	gtk_list_box_select_row(GTK_LIST_BOX(palette->list), first);
	gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(palette->scroller)), 0);
}


/* ==================================== Project files ==================================== */


static void add_listed_file(GFile *file, GFileInfo *info, gpointer data) {
	FileListing *listing = data;
	if (listing->paths->len < MAXIMUM_FILES)
		g_ptr_array_add(listing->paths, g_file_get_relative_path(listing->root, file));
}


static void list_files_in_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	FileListing *listing = data;
	listing->paths = g_ptr_array_new_with_free_func(g_free);
	visit_project_files(listing->root, listing->excluded, listing->shows_hidden, LISTING_ATTRIBUTES, add_listed_file, listing,
		cancellable);
	g_task_return_pointer(task, g_steal_pointer(&listing->paths), (GDestroyNotify)g_ptr_array_unref);
}


static void free_file_listing(gpointer data) {
	FileListing *listing = data;
	g_object_unref(listing->root);
	g_strfreev(listing->excluded);
	g_free(listing);
}


static void on_files_read(GObject *source, GAsyncResult *result, gpointer data) {
	GPtrArray *files = g_task_propagate_pointer(G_TASK(result), NULL);
	/* Closing the palette cancels the listing and frees the palette before this runs */
	if (!files)
		return;
	Palette *palette = data;
	palette->files = files;
	fill_rows(palette);
}


static void start_reading_files(Palette *palette) {
	FileListing *listing = g_new0(FileListing, 1);
	listing->root = g_file_new_for_path(palette->window->project);
	listing->excluded = list_excluded_project_names();
	listing->shows_hidden = get_switch_setting("show-hidden-files");
	g_autoptr(GTask) task = g_task_new(NULL, palette->cancellable, on_files_read, palette);
	g_task_set_task_data(task, listing, free_file_listing);
	g_task_run_in_thread(task, list_files_in_thread);
}


/* ==================================== Running ==================================== */


/* The palette closes first, so a command that opens a dialog or closes the window runs without it */
static void run_row(Palette *palette, GtkListBoxRow *row) {
	if (!row)
		return;
	Window *window = palette->window;
	const Command *command = g_object_get_data(G_OBJECT(row), "command");
	g_autofree char *line = g_strdup(g_object_get_data(G_OBJECT(row), "line"));
	g_autofree char *path = g_strdup(g_object_get_data(G_OBJECT(row), "path"));
	adw_dialog_close(ADW_DIALOG(palette->dialog));
	if (path) {
		g_autoptr(GFile) file = g_file_new_for_path(path);
		show_window_file(window, file);
	} else if (line) {
		run_window_command_line(window, line);
	} else if (command) {
		activate_window_command(window, command);
	}
}


static void on_entry_activated(GtkSearchEntry *entry, Palette *palette) {
	run_row(palette, gtk_list_box_get_selected_row(GTK_LIST_BOX(palette->list)));
}


static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, Palette *palette) {
	run_row(palette, row);
}


static void scroll_to_row(Palette *palette, GtkListBoxRow *row) {
	graphene_rect_t bounds;
	if (!gtk_widget_compute_bounds(GTK_WIDGET(row), palette->list, &bounds))
		return;
	GtkAdjustment *scroll = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(palette->scroller));
	double top = bounds.origin.y, bottom = top + bounds.size.height;
	double value = gtk_adjustment_get_value(scroll), page = gtk_adjustment_get_page_size(scroll);
	if (top < value)
		gtk_adjustment_set_value(scroll, top);
	else if (bottom > value + page)
		gtk_adjustment_set_value(scroll, bottom - page);
}


/* Up and Down move through the rows while typing continues in the search field */
static gboolean on_entry_key(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType state,
	Palette *palette) {
	int step = key == GDK_KEY_Down ? 1 : key == GDK_KEY_Up ? -1 : 0;
	if (!step)
		return GDK_EVENT_PROPAGATE;
	GtkListBox *list = GTK_LIST_BOX(palette->list);
	GtkListBoxRow *selected = gtk_list_box_get_selected_row(list);
	GtkListBoxRow *row = gtk_list_box_get_row_at_index(list, selected ? gtk_list_box_row_get_index(selected) + step
		: 0);
	if (row) {
		gtk_list_box_select_row(list, row);
		scroll_to_row(palette, row);
	}
	return GDK_EVENT_STOP;
}


static void on_stop_search(GtkSearchEntry *entry, Palette *palette) {
	adw_dialog_close(ADW_DIALOG(palette->dialog));
}


static void on_closed(AdwDialog *dialog, Palette *palette) {
	g_cancellable_cancel(palette->cancellable);
	g_object_unref(palette->cancellable);
	g_clear_pointer(&palette->files, g_ptr_array_unref);
	g_free(palette);
}


/* ==================================== Creation ==================================== */


static gboolean move_cursor_to_end(gpointer data) {
	GtkWidget *entry = data;
	gtk_editable_set_position(GTK_EDITABLE(entry), -1);
	g_object_unref(entry);
	return G_SOURCE_REMOVE;
}


/* Focusing places the cursor or selects the text after the focus has moved, which would let the first typed letter
 * go before or replace a > the palette opened with, so the cursor moves to the end once that is done */
static void on_entry_focused(GtkEventControllerFocus *focus, GtkWidget *entry) {
	g_idle_add(move_cursor_to_end, g_object_ref(entry));
	g_signal_handlers_disconnect_by_data(focus, entry);
}


static GtkWidget *create_entry(Palette *palette, const char *start_text) {
	GtkWidget *entry = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(entry), TEXT_PALETTE_HINT);
	gtk_search_entry_set_search_delay(GTK_SEARCH_ENTRY(entry), 0);
	gtk_editable_set_text(GTK_EDITABLE(entry), start_text);
	gtk_widget_set_margin_start(entry, PALETTE_MARGIN);
	gtk_widget_set_margin_end(entry, PALETTE_MARGIN);
	gtk_widget_set_margin_top(entry, PALETTE_MARGIN);
	gtk_widget_set_margin_bottom(entry, PALETTE_MARGIN);
	g_signal_connect_swapped(entry, "search-changed", G_CALLBACK(fill_rows), palette);
	g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activated), palette);
	g_signal_connect(entry, "stop-search", G_CALLBACK(on_stop_search), palette);
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_entry_key), palette);
	gtk_widget_add_controller(entry, keys);
	GtkEventController *focus = gtk_event_controller_focus_new();
	g_signal_connect(focus, "enter", G_CALLBACK(on_entry_focused), entry);
	gtk_widget_add_controller(entry, focus);
	return entry;
}


/* A pointer resting over the list as it opens may only reach the rows once it really moves. The position is kept
 * in surface coordinates, since the opening animation moves the list under a still pointer */
static void notice_pointer(GtkEventControllerMotion *motion, double x, double y, Palette *palette) {
	GtkNative *native = gtk_widget_get_native(palette->scroller);
	graphene_point_t local = GRAPHENE_POINT_INIT(x, y), point;
	if (!gtk_widget_compute_point(palette->scroller, GTK_WIDGET(native), &local, &point))
		return;
	double shift_x, shift_y;
	gtk_native_get_surface_transform(native, &shift_x, &shift_y);
	point.x += shift_x;
	point.y += shift_y;
	if (!palette->has_pointer) {
		palette->pointer = point;
		palette->has_pointer = TRUE;
		return;
	}
	if (graphene_point_equal(&point, &palette->pointer))
		return;
	gtk_widget_set_can_target(palette->list, TRUE);
	g_signal_handlers_disconnect_by_data(motion, palette);
}


static GtkWidget *create_list(Palette *palette) {
	palette->list = gtk_list_box_new();
	gtk_widget_add_css_class(palette->list, "navigation-sidebar");
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(palette->list), GTK_SELECTION_BROWSE);
	palette->placeholder = gtk_label_new(NULL);
	gtk_widget_add_css_class(palette->placeholder, "dim-label");
	gtk_widget_set_margin_top(palette->placeholder, PALETTE_MARGIN);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(box), palette->list);
	gtk_box_append(GTK_BOX(box), palette->placeholder);
	g_signal_connect(palette->list, "row-activated", G_CALLBACK(on_row_activated), palette);
	gtk_widget_set_can_target(palette->list, FALSE);
	palette->scroller = gtk_scrolled_window_new();
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "enter", G_CALLBACK(notice_pointer), palette);
	g_signal_connect(motion, "motion", G_CALLBACK(notice_pointer), palette);
	gtk_widget_add_controller(palette->scroller, motion);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(palette->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(palette->scroller), box);
	gtk_widget_set_vexpand(palette->scroller, TRUE);
	return palette->scroller;
}


/* Opening under a still pointer sends it no motion, so its starting place is read from the pointer itself */
static void remember_pointer(Palette *palette) {
	GdkSurface *surface = gtk_native_get_surface(gtk_widget_get_native(palette->window->window));
	GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_surface_get_display(surface)));
	double x, y;
	if (!pointer || !gdk_surface_get_device_position(surface, pointer, &x, &y, NULL))
		return;
	palette->pointer = GRAPHENE_POINT_INIT(x, y);
	palette->has_pointer = TRUE;
}


/* Opens at once with the open files and commands, and adds the project's files when they have been read */
void show_command_palette(Window *window, const char *start_text) {
	Palette *palette = g_new0(Palette, 1);
	palette->window = window;
	palette->cancellable = g_cancellable_new();
	palette->entry = create_entry(palette, start_text);
	GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(content), palette->entry);
	gtk_box_append(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	gtk_box_append(GTK_BOX(content), create_list(palette));
	palette->dialog = GTK_WIDGET(adw_dialog_new());
	adw_dialog_set_title(ADW_DIALOG(palette->dialog), TEXT_PALETTE_TITLE);
	adw_dialog_set_content_width(ADW_DIALOG(palette->dialog), PALETTE_WIDTH);
	adw_dialog_set_content_height(ADW_DIALOG(palette->dialog), PALETTE_HEIGHT);
	adw_dialog_set_child(ADW_DIALOG(palette->dialog), content);
	g_signal_connect(palette->dialog, "closed", G_CALLBACK(on_closed), palette);
	fill_rows(palette);
	if (window->project)
		start_reading_files(palette);
	adw_dialog_set_focus(ADW_DIALOG(palette->dialog), palette->entry);
	adw_dialog_present(ADW_DIALOG(palette->dialog), window->window);
	remember_pointer(palette);
}
