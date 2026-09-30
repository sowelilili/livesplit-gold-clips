#include "lstime.h"

#include <stdio.h>
#include <string.h>

static bool is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Parses 1..max_digits decimal digits in [s, e). */
static bool parse_digits(const char *s, const char *e, int max_digits, int64_t *out)
{
	int64_t v = 0;

	if (s >= e || e - s > max_digits)
		return false;
	for (; s < e; s++) {
		if (*s < '0' || *s > '9')
			return false;
		v = v * 10 + (*s - '0');
	}
	*out = v;
	return true;
}

bool lstime_parse(const char *text, int64_t *ticks)
{
	const char *s, *e, *p;
	const char *field[3], *field_end[3];
	int n = 0;
	bool negative = false;
	int64_t days = 0, hours = 0, minutes = 0, seconds = 0, fraction = 0;

	if (!text)
		return false;

	s = text;
	e = text + strlen(text);
	while (s < e && is_space(*s))
		s++;
	while (e > s && is_space(e[-1]))
		e--;

	if (s < e && *s == '-') {
		negative = true;
		s++;
	}
	if (s >= e)
		return false; /* "", "-" */

	/* Split into at most three ':'-separated fields: [[h]:m:]s */
	p = s;
	for (;;) {
		const char *c = p;
		while (c < e && *c != ':')
			c++;
		if (n == 3)
			return false;
		field[n] = p;
		field_end[n] = c;
		n++;
		if (c == e)
			break;
		p = c + 1;
	}

	/* Seconds, with an optional fraction (at most 7 digits are significant). */
	{
		const char *ss = field[n - 1], *se = field_end[n - 1];
		const char *dot = ss;
		while (dot < se && *dot != '.' && *dot != ',')
			dot++;
		if (!parse_digits(ss, dot, 9, &seconds))
			return false;
		if (dot < se) {
			const char *f = dot + 1;
			int digits = 0;
			if (f >= se)
				return false;
			for (; f < se; f++) {
				if (*f < '0' || *f > '9')
					return false;
				if (digits < 7) {
					fraction = fraction * 10 + (*f - '0');
					digits++;
				}
			}
			for (; digits < 7; digits++)
				fraction *= 10;
		}
	}

	if (n >= 2 && !parse_digits(field[n - 2], field_end[n - 2], 8, &minutes))
		return false;

	if (n == 3) {
		const char *hs = field[0], *he = field_end[0];
		const char *dot = hs;
		while (dot < he && *dot != '.')
			dot++;
		if (dot < he) {
			if (!parse_digits(hs, dot, 6, &days) || !parse_digits(dot + 1, he, 6, &hours))
				return false;
		} else if (!parse_digits(hs, he, 6, &hours)) {
			return false;
		}
	}

	{
		int64_t total = (((days * 24 + hours) * 60 + minutes) * 60 + seconds) * LSTIME_TICKS_PER_SECOND +
				fraction;
		*ticks = negative ? -total : total;
	}
	return true;
}

struct parts {
	const char *sign;
	long long h, m, s, cs;
};

static struct parts split_ticks(int64_t ticks)
{
	struct parts p;
	long long hundredths;

	p.sign = "";
	if (ticks < 0) {
		p.sign = "-";
		ticks = -ticks;
	}
	hundredths = (long long)(ticks / 100000); /* truncated, like LiveSplit shows it */
	p.cs = hundredths % 100;
	p.s = (hundredths / 100) % 60;
	p.m = (hundredths / 6000) % 60;
	p.h = hundredths / 360000;
	return p;
}

void lstime_format_filename(int64_t ticks, char *buf, size_t size)
{
	struct parts p = split_ticks(ticks);

	if (p.h > 0)
		snprintf(buf, size, "%s%lldh%02lldm%02lld.%02llds", p.sign, p.h, p.m, p.s, p.cs);
	else if (p.m > 0)
		snprintf(buf, size, "%s%lldm%02lld.%02llds", p.sign, p.m, p.s, p.cs);
	else
		snprintf(buf, size, "%s%lld.%02llds", p.sign, p.s, p.cs);
}

void lstime_format_display(int64_t ticks, char *buf, size_t size)
{
	struct parts p = split_ticks(ticks);

	if (p.h > 0)
		snprintf(buf, size, "%s%lld:%02lld:%02lld.%02lld", p.sign, p.h, p.m, p.s, p.cs);
	else if (p.m > 0)
		snprintf(buf, size, "%s%lld:%02lld.%02lld", p.sign, p.m, p.s, p.cs);
	else
		snprintf(buf, size, "%s%lld.%02lld", p.sign, p.s, p.cs);
}
