/**
* Touhou Community Reliant Automatic Patcher
* Tasogare Frontier support plugin
*
* ----
*
* On-the-fly th145 pl patcher
*/

#include <thcrap.h>
#include "thcrap_tasofro.h"
#include "pl.h"

#define MOVE_BUFF(buff, size, n) (buff) += n; (size) -= n;
#define PUT_CHAR(buff, size, c) *(buff) = c; MOVE_BUFF(buff, size, 1);
#define PUT_STR(buff, size, str) strcpy(buff, str); MOVE_BUFF(buff, size, strlen(str));

static balloon_t balloon;

void balloon_set_character(BYTE* line, size_t size)
{
	unsigned int i;

	for (i = 0; i < size && line[i] != '#' && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n'; i++);
	if (i < size) {
		line[i] = '\0';
		strcpy(balloon.owner, line);
		line[i] = '\t';
	}
}

balloon_t *balloon_get()
{
	balloon.cur_line = 1;
	balloon.nb_lines = 0;
	return &balloon;
}

static void next_line(BYTE **file, size_t *size)
{
	while (*size > 0 && **file != '\n') {
		MOVE_BUFF(*file, *size, 1);
	}
	if (*size > 0) {
		MOVE_BUFF(*file, *size, 1);
	}
}

static BOOL ignore_line(BYTE *file, size_t size)
{
	while (size > 0 && *file == ' ') {
		MOVE_BUFF(file, size, 1);
	}
	if (size == 0) {
		return TRUE;
	}
	if (*file == '#' ||
		*file == ':' ||
		*file == ',' ||
		*file == '\r' ||
		*file == '\n') {
		if (strncmp(file, ",SetFocus,", 10) == 0) {
			balloon_set_character(file + 10, size - 10);
		}
		return TRUE;
	}
	return FALSE;
}

static void copy_line(BYTE *file_in, size_t size_in, BYTE *file_out, size_t size_out)
{
	size_t i;

	for (i = 0; i < size_in && i < size_out && file_in[i] != '\n'; i++) {
		file_out[i] = file_in[i];
	}
	if (i < size_out) {
		file_out[i] = '\n';
	}
}

static int balloon_gen_name(balloon_t *balloon, int cur_line, json_t *lines)
{
	// TODO: actually look for a nice balloon size
	const char *line;

	line = json_array_get_string(lines, cur_line);
	if (strncmp(line, "<balloon", 8) == 0) {
		if (line[8] == '$') {
			strncpy(balloon->name, line + 9, 5);
		}
		balloon->cur_line = 0;
		balloon->nb_lines = 0;
		return 0;
	}

	if (balloon->nb_lines == 0) {
		unsigned int i;
		for (i = cur_line + 1; i < json_array_size(lines); i++) {
			line = json_array_get_string(lines, i);
			if (strncmp(line, "<balloon", 8) == 0) {
				break;
			}
		}
		balloon->nb_lines = i - cur_line;
	}

	strcpy(balloon->name + 5, "_1_1");
	balloon->name[6] = balloon->cur_line + '0';
	balloon->name[8] = balloon->nb_lines + '0';
	return 1;
}

static void replace_line(BYTE *file_in, size_t size_in, BYTE **file_out, size_t *size_out, unsigned int balloon_nb, json_t *lines)
{
	balloon_t *balloon = balloon_get();
	unsigned int i;

	// Skip the original text
	if (*file_in != '"') {
		while (size_in > 0 && *file_in != '\n' && *file_in != ',') {
			MOVE_BUFF(file_in, size_in, 1);
		}
		balloon->has_pause = file_in[-1] == '\\';
	}
	else {
		MOVE_BUFF(file_in, size_in, 1);
		while (size_in > 0 && *file_in != '\n' && *file_in != '"') {
			MOVE_BUFF(file_in, size_in, 1);
		}
		balloon->has_pause = file_in[-1] == '\\';
		MOVE_BUFF(file_in, size_in, 1);
	}
	MOVE_BUFF(file_in, size_in, 1);

	// Copy the balloon name from the original file
	for (i = 0; size_in > 0 && *file_in != '\n' && *file_in != ',' && i < 10; i++, file_in++, size_in--) {
		balloon->name[i] = *file_in;
	}

	// Write to the output file
	unsigned int cur_line = 0;
	unsigned int nb_lines = json_array_size(lines);
	int ignore_clear_balloon = 1;

	for (; cur_line < nb_lines; cur_line++, balloon->cur_line++) {
		if (balloon_gen_name(balloon, cur_line, lines) == 0)
			continue;
		const char *json_line = json_array_get_string(lines, cur_line);

		// Add the "ClearBalloon" command if needed
		if (ignore_clear_balloon == 0 && balloon->cur_line == 1) {
			PUT_STR(*file_out, *size_out, ",ClearBalloon,");
			PUT_STR(*file_out, *size_out, balloon->owner);
			PUT_CHAR(*file_out, *size_out, '\r');
			PUT_CHAR(*file_out, *size_out, '\n');
		}
		if (ignore_clear_balloon == 1) {
			ignore_clear_balloon = 0;
		}

		// Writing the replacement text
		PUT_CHAR(*file_out, *size_out, '"');
		PUT_STR(*file_out, *size_out, json_line);
			if (balloon->has_pause && balloon->cur_line == balloon->nb_lines) {
			PUT_CHAR(*file_out, *size_out, '\\');
		}
		PUT_CHAR(*file_out, *size_out, '"');
		PUT_CHAR(*file_out, *size_out, ',');

		// Writing the balloon name
		PUT_STR(*file_out, *size_out, balloon->name);

		// Appenfing the end of the original text
		for (i = 0; i < size_in && file_in[i] != '\n' && *size_out > 0; i++) {
			PUT_CHAR(*file_out, *size_out, file_in[i]);
		}
		PUT_CHAR(*file_out, *size_out, '\n');

		// Ensure we won't make a 4-lines balloon
		if (balloon->cur_line == 3) {
			balloon->cur_line = 0;
		}
	}
}

int patch_pl(BYTE *file_inout, size_t size_out, size_t size_in, json_t *patch)
{
	BYTE* buffer_in;
	BYTE* file_in;
	size_t balloon;
	json_t* line_data;
	json_t* lines;
	BOOL need_to_copy_line;

#ifdef _DEBUG
	BYTE *file_inout_b = file_inout;
#endif

	// Make a copy of the input buffer
	buffer_in = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size_in);
	memcpy(buffer_in, file_inout, size_in);
	file_in = buffer_in;

	balloon = 1;

	for (; size_in > 0; next_line(&file_in, &size_in)) {
		need_to_copy_line = TRUE;
		if (!ignore_line(file_in, size_in)) {

			// The line contains replacable text
			line_data = json_object_numkey_get(patch, balloon);
			balloon++;
			if (json_is_object(line_data)) {

				// We have a corresponding entry in our jdiff file
				lines = json_object_get(line_data, "lines");
				if (json_is_array(lines)) {

					// We have an array of lines; we can replace our text.
					replace_line(file_in, size_in, &file_inout, &size_out, balloon, lines);
					need_to_copy_line = FALSE;

				}
				json_decref_safe(lines);
			}
			json_decref_safe(line_data);
		}

		if (need_to_copy_line) {
			copy_line(file_in, size_in, file_inout, size_out);
			next_line(&file_inout, &size_out);
		}
	}

	free(buffer_in);

#ifdef _DEBUG
	FILE* fd = fopen("out.pl", "wb");
	if (fd) {
		fwrite(file_inout_b, file_inout - file_inout_b, 1, fd);
		fclose(fd);
	}
#endif

	return 0;
}