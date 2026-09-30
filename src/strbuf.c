#include "strbuf.h"

#include <stdlib.h>
#include <string.h>

void sb_init(struct strbuf *sb)
{
	memset(sb, 0, sizeof(*sb));
}

void sb_free(struct strbuf *sb)
{
	free(sb->s);
	sb_init(sb);
}

void sb_append_n(struct strbuf *sb, const char *s, size_t n)
{
	if (sb->failed)
		return;
	if (sb->len + n + 1 > sb->cap) {
		size_t cap = sb->cap ? sb->cap : 64;
		char *p;
		while (sb->len + n + 1 > cap)
			cap *= 2;
		p = realloc(sb->s, cap);
		if (!p) {
			sb->failed = true;
			return;
		}
		sb->s = p;
		sb->cap = cap;
	}
	memcpy(sb->s + sb->len, s, n);
	sb->len += n;
	sb->s[sb->len] = '\0';
}

void sb_append(struct strbuf *sb, const char *s)
{
	sb_append_n(sb, s, strlen(s));
}

void sb_append_ch(struct strbuf *sb, char c)
{
	sb_append_n(sb, &c, 1);
}
