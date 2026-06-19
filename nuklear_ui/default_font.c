/*
 Default UI font for BlastEm's nuklear interface.

 Replaces the per-platform system-font scanners (font_mac.m / font.c / font_win.c)
 with a single embedded font (Inter Medium, SIL OFL — see Inter-LICENSE.txt) used on
 every platform. This is fast (no filesystem scan), deterministic (identical UI on
 macOS/Linux/Windows), and never fails to find a font.

 The macOS scanner in particular could spend ~50s enumerating /System/Library/Fonts,
 spam stdout with "Skipping <name>" for each non-TrueType file (which corrupted the
 gdb-remote pipe, since that uses stdout), and fatal_error if nothing matched.

 Set BLASTEM_FONT=/path/to/font.ttf to override with a TrueType font of your choice.
 The returned buffer is heap-allocated; nuklear takes ownership (ttf_data_owned_by_atlas).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "font.h"

// embedded Inter-Medium.ttf (generated from the .ttf by bin2c.py at build time)
extern const unsigned char inter_medium_ttf[];
extern const unsigned int inter_medium_ttf_len;

static uint8_t *read_font_file(const char *path, uint32_t *size_out)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	uint8_t *buf = NULL;
	if (!fseek(f, 0, SEEK_END)) {
		long size = ftell(f);
		if (size > 0 && !fseek(f, 0, SEEK_SET)) {
			buf = malloc(size);
			if (buf && fread(buf, 1, size, f) == (size_t)size) {
				*size_out = (uint32_t)size;
			} else {
				free(buf);
				buf = NULL;
			}
		}
	}
	fclose(f);
	return buf;
}

uint8_t *default_font(uint32_t *size_out)
{
	// Runtime override (note: warning goes to stderr so it can't corrupt the gdb stdout pipe)
	const char *override = getenv("BLASTEM_FONT");
	if (override && *override) {
		uint8_t *buf = read_font_file(override, size_out);
		if (buf) {
			return buf;
		}
		fprintf(stderr, "BLASTEM_FONT=%s could not be loaded; using the built-in font\n", override);
	}

	// Built-in font (a heap copy: nuklear frees it via ttf_data_owned_by_atlas)
	uint8_t *buf = malloc(inter_medium_ttf_len);
	if (buf) {
		memcpy(buf, inter_medium_ttf, inter_medium_ttf_len);
		*size_out = inter_medium_ttf_len;
	}
	return buf;
}
