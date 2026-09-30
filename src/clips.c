#include "clips.h"

#include "lstime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_PER_SECOND 1000000000LL
#define NS_PER_MS 1000000LL

static char *dup_str(const char *s)
{
	size_t len = strlen(s ? s : "");
	char *d = malloc(len + 1);
	if (d)
		memcpy(d, s ? s : "", len + 1);
	return d;
}

bool clip_copy(struct clip *dst, const struct clip *src)
{
	*dst = *src;
	dst->game = dup_str(src->game);
	dst->category = dup_str(src->category);
	dst->split_name = dup_str(src->split_name);
	if (!dst->game || !dst->category || !dst->split_name) {
		clip_free(dst);
		return false;
	}
	return true;
}

void clip_free(struct clip *c)
{
	free(c->game);
	free(c->category);
	free(c->split_name);
	c->game = c->category = c->split_name = NULL;
}

void clip_free_array(struct clip *clips, size_t num)
{
	for (size_t i = 0; i < num; i++)
		clip_free(&clips[i]);
	free(clips);
}

void clipq_init(struct clipq *q)
{
	memset(q, 0, sizeof(*q));
}

void clipq_free(struct clipq *q)
{
	clip_free_array(q->items, q->num);
	memset(q, 0, sizeof(*q));
}

bool clipq_add(struct clipq *q, const struct clip *c)
{
	if (q->num == q->cap) {
		size_t cap = q->cap ? q->cap * 2 : 8;
		struct clip *items = realloc(q->items, cap * sizeof(*items));
		if (!items)
			return false;
		q->items = items;
		q->cap = cap;
	}
	if (!clip_copy(&q->items[q->num], c))
		return false;
	q->num++;
	return true;
}

size_t clipq_cancel(struct clipq *q, uint64_t run_id, int from_split)
{
	size_t kept = 0, removed = 0;

	for (size_t i = 0; i < q->num; i++) {
		if (q->items[i].run_id == run_id && q->items[i].split_index >= from_split) {
			clip_free(&q->items[i]);
			removed++;
		} else {
			q->items[kept++] = q->items[i];
		}
	}
	q->num = kept;
	return removed;
}

bool clipq_latest_end(const struct clipq *q, int64_t *end_ns)
{
	if (!q->num)
		return false;
	*end_ns = q->items[0].end_ns;
	for (size_t i = 1; i < q->num; i++)
		if (q->items[i].end_ns > *end_ns)
			*end_ns = q->items[i].end_ns;
	return true;
}

size_t clipq_take_ended_by(struct clipq *q, int64_t t_ns, struct clip **out)
{
	size_t n = 0, kept = 0;
	struct clip *taken;

	*out = NULL;
	for (size_t i = 0; i < q->num; i++)
		if (q->items[i].end_ns <= t_ns)
			n++;
	if (!n)
		return 0;

	taken = malloc(n * sizeof(*taken));
	if (!taken)
		return 0;
	n = 0;
	for (size_t i = 0; i < q->num; i++) {
		if (q->items[i].end_ns <= t_ns)
			taken[n++] = q->items[i];
		else
			q->items[kept++] = q->items[i];
	}
	q->num = kept;
	*out = taken;
	return n;
}

static int64_t seconds_to_ns(double s)
{
	return (int64_t)(s * (double)NS_PER_SECOND + (s >= 0 ? 0.5 : -0.5));
}

void clip_window(int64_t seg_start_ns, int64_t seg_end_ns, double lead_seconds, double tail_seconds,
		 int64_t *start_ns, int64_t *end_ns)
{
	*start_ns = seg_start_ns - seconds_to_ns(lead_seconds);
	*end_ns = seg_end_ns + seconds_to_ns(tail_seconds);
}

static int64_t ns_to_ms_ceil(int64_t ns)
{
	return ns > 0 ? (ns + NS_PER_MS - 1) / NS_PER_MS : ns / NS_PER_MS;
}

bool clip_cut_params(int64_t t_req_ns, int64_t start_ns, int64_t end_ns, double buffer_seconds, int64_t *sseof_ms,
		     int64_t *dur_ms)
{
	int64_t x = t_req_ns - start_ns;
	int64_t d = end_ns - start_ns;
	bool clamped = false;

	if (buffer_seconds > 0) {
		int64_t buffer = seconds_to_ns(buffer_seconds);
		if (x > buffer) {
			d -= x - buffer;
			x = buffer;
			clamped = true;
		}
	}
	*sseof_ms = ns_to_ms_ceil(x);
	if (*sseof_ms < 1)
		*sseof_ms = 1;
	*dur_ms = d > 0 ? ns_to_ms_ceil(d) : 0;
	return clamped;
}

static size_t utf8_sequence_length(const unsigned char *s)
{
	size_t len;

	if (s[0] < 0x80)
		return 1;
	if (s[0] >= 0xC2 && s[0] <= 0xDF)
		len = 2;
	else if (s[0] >= 0xE0 && s[0] <= 0xEF)
		len = 3;
	else if (s[0] >= 0xF0 && s[0] <= 0xF4)
		len = 4;
	else
		return 0; /* invalid lead byte */
	for (size_t i = 1; i < len; i++)
		if ((s[i] & 0xC0) != 0x80)
			return 0;
	return len;
}

static bool is_reserved_name(const char *name)
{
	static const char *reserved[] = {"CON", "PRN", "AUX", "NUL"};
	char base[8];
	size_t len = 0;

	/* The device name rule applies to the part before the first dot, with
	 * trailing spaces ignored ("CON.txt" and "NUL .mkv" are reserved too). */
	while (name[len] && name[len] != '.')
		len++;
	while (len > 0 && name[len - 1] == ' ')
		len--;
	if (len < 3 || len > 4)
		return false;
	for (size_t i = 0; i < len; i++) {
		char c = name[i];
		base[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
	}
	base[len] = '\0';

	if (len == 3) {
		for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
			if (strcmp(base, reserved[i]) == 0)
				return true;
		return false;
	}
	return (strncmp(base, "COM", 3) == 0 || strncmp(base, "LPT", 3) == 0) && base[3] >= '0' && base[3] <= '9';
}

void path_sanitize_component(const char *in, char *out, size_t out_size)
{
	size_t max = out_size - 1;
	size_t n = 0, start = 0;
	const unsigned char *s = (const unsigned char *)(in ? in : "");

	if (out_size < 2) {
		if (out_size)
			out[0] = '\0';
		return;
	}
	if (max > GC_MAX_COMPONENT_BYTES)
		max = GC_MAX_COMPONENT_BYTES;

	while (*s) {
		size_t len = utf8_sequence_length(s);
		if (len <= 1) {
			unsigned char c = *s;
			if (n + 1 > max)
				break;
			if (len == 0 || c < 0x20 || c == 0x7F || strchr("\\/:*?\"<>|", c))
				out[n++] = '_';
			else
				out[n++] = (char)c;
			s++;
		} else {
			if (n + len > max)
				break;
			memcpy(out + n, s, len);
			n += len;
			s += len;
		}
	}
	out[n] = '\0';

	while (n > 0 && (out[n - 1] == '.' || out[n - 1] == ' '))
		out[--n] = '\0';
	while (start < n && out[start] == ' ')
		start++;
	if (start) {
		memmove(out, out + start, n - start + 1);
		n -= start;
	}

	if (n == 0) {
		strcpy(out, "_");
		return;
	}
	if (is_reserved_name(out)) {
		if (n + 1 > max)
			n = max - 1; /* reserved names are short; this cannot split UTF-8 */
		memmove(out + 1, out, n);
		out[0] = '_';
		out[n + 1] = '\0';
	}
}

const char *path_extension(const char *path)
{
	const char *slash1 = strrchr(path, '/');
	const char *slash2 = strrchr(path, '\\');
	const char *name = path;
	const char *dot;

	if (slash1 && slash1 + 1 > name)
		name = slash1 + 1;
	if (slash2 && slash2 + 1 > name)
		name = slash2 + 1;
	dot = strrchr(name, '.');
	return dot && dot != name ? dot : path + strlen(path);
}

static bool append(char *out, size_t size, size_t *len, const char *s)
{
	size_t n = strlen(s);
	if (*len + n + 1 > size)
		return false;
	memcpy(out + *len, s, n + 1);
	*len += n;
	return true;
}

static bool append_component(char *out, size_t size, size_t *len, const char *raw)
{
	char safe[GC_MAX_COMPONENT_BYTES + 8];
	path_sanitize_component(raw, safe, sizeof(safe));
	return append(out, size, len, "/") && append(out, size, len, safe);
}

bool clip_output_dir(const char *replay_path, const char *folder, const char *game, const char *category, char *out,
		     size_t out_size)
{
	const char *slash1 = strrchr(replay_path, '/');
	const char *slash2 = strrchr(replay_path, '\\');
	const char *sep = !slash1 ? slash2 : !slash2 ? slash1 : slash1 > slash2 ? slash1 : slash2;
	size_t len;

	if (sep) {
		len = (size_t)(sep - replay_path);
		if (len + 1 > out_size)
			return false;
		memcpy(out, replay_path, len);
		out[len] = '\0';
	} else {
		if (out_size < 2)
			return false;
		strcpy(out, ".");
		len = 1;
	}

	if (folder) {
		const char *p = folder;
		while (*p) {
			char part[512];
			size_t n = strcspn(p, "/\\");
			if (n > 0 && !(n == 1 && p[0] == '.')) {
				if (n >= sizeof(part))
					n = sizeof(part) - 1;
				memcpy(part, p, n);
				part[n] = '\0';
				if (!append_component(out, out_size, &len, part))
					return false;
			}
			p += strcspn(p, "/\\");
			if (*p)
				p++;
		}
	}

	return append_component(out, out_size, &len, game) && append_component(out, out_size, &len, category);
}

void clip_file_base(const char *split_name, int64_t seg_ticks, char *out, size_t out_size)
{
	char safe[GC_MAX_COMPONENT_BYTES + 8];
	char seg[32];

	path_sanitize_component(split_name, safe, sizeof(safe));
	lstime_format_filename(seg_ticks, seg, sizeof(seg));
	snprintf(out, out_size, "%s %s", safe, seg);
}

bool clip_unique_path(const char *dir, const char *base, const char *ext, path_exists_fn exists, void *ctx,
		      char *out, size_t out_size)
{
	int n = snprintf(out, out_size, "%s/%s%s", dir, base, ext);
	if (n < 0 || (size_t)n >= out_size)
		return false;
	if (!exists(ctx, out))
		return true;
	for (int i = 2; i < 10000; i++) {
		n = snprintf(out, out_size, "%s/%s (%d)%s", dir, base, i, ext);
		if (n < 0 || (size_t)n >= out_size)
			return false;
		if (!exists(ctx, out))
			return true;
	}
	return false;
}
