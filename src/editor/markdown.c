#include "markdown.h"


char *convert_markdown_line(const char *line) {
	g_autofree char *escaped = g_markup_escape_text(line, -1);
	GString *markup = g_string_new(NULL);
	const char *text = escaped;
	int heading = 0;
	while (text[heading] == '#')
		heading++;
	if (heading > 0 && text[heading] == ' ')
		text += heading + 1;
	else
		heading = 0;
	gboolean is_code = FALSE, is_bold = FALSE, is_italic = FALSE, is_struck = FALSE;
	for (const char *character = text; *character; character++) {
		if (*character == '`') {
			g_string_append(markup, is_code ? "</tt>" : "<tt>");
			is_code = !is_code;
		} else if (!is_code && character[0] == '*' && character[1] == '*') {
			g_string_append(markup, is_bold ? "</b>" : "<b>");
			is_bold = !is_bold;
			character++;
		} else if (!is_code && character[0] == '~' && character[1] == '~') {
			g_string_append(markup, is_struck ? "</s>" : "<s>");
			is_struck = !is_struck;
			character++;
		} else if (!is_code && *character == '*' && (is_italic || g_ascii_isalnum(character[1]))) {
			g_string_append(markup, is_italic ? "</i>" : "<i>");
			is_italic = !is_italic;
		} else if (!is_code && *character == '\\' && character[1]) {
			g_string_append_c(markup, *++character);
		} else {
			g_string_append_c(markup, *character);
		}
	}
	if (is_code)
		g_string_append(markup, "</tt>");
	if (is_bold)
		g_string_append(markup, "</b>");
	if (is_italic)
		g_string_append(markup, "</i>");
	if (is_struck)
		g_string_append(markup, "</s>");
	if (heading > 0) {
		g_string_prepend(markup, "<b>");
		g_string_append(markup, "</b>");
	}
	return g_string_free(markup, FALSE);
}
