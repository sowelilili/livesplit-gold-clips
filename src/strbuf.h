/*
 * Growable string.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

struct strbuf {
	char *s;
	size_t len;
	size_t cap;
	bool failed; /* an allocation failed; contents are incomplete */
};

void sb_init(struct strbuf *sb);
void sb_free(struct strbuf *sb);
void sb_append_n(struct strbuf *sb, const char *s, size_t n);
void sb_append(struct strbuf *sb, const char *s);
void sb_append_ch(struct strbuf *sb, char c);
