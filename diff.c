/*
 * Copyright (C) 2005 Junio C Hamano
 */
#include "cache.h"
#include "quote.h"
#include "diff.h"
#include "diffcore.h"
#include "delta.h"
#include "xdiff-interface.h"
#include "color.h"
#include "attr.h"
#include "run-command.h"
#include "utf8.h"
#include "userdiff.h"
#include "sigchain.h"

#ifdef NO_FAST_WORKING_DIRECTORY
#define FAST_WORKING_DIRECTORY 0
#else
#define FAST_WORKING_DIRECTORY 1
#endif

static int diff_detect_rename_default;
static int diff_rename_limit_default = 200;
static int diff_suppress_blank_empty;
int diff_use_color_default = -1;
static const char *diff_word_regex_cfg;
static const char *external_diff_cmd_cfg;
int diff_auto_refresh_index = 1;
static int diff_mnemonic_prefix;

static char diff_colors[][COLOR_MAXLEN] = {
	"\033[m",	/* reset */
	"",		/* PLAIN (normal) */
	"\033[1m",	/* METAINFO (bold) */
	"\033[36m",	/* FRAGINFO (cyan) */
	"\033[31m",	/* OLD (red) */
	"\033[32m",	/* NEW (green) */
	"\033[33m",	/* COMMIT (yellow) */
	"\033[41m",	/* WHITESPACE (red background) */
};

static void diff_filespec_load_driver(struct diff_filespec *one);
static char *run_textconv(const char *, struct diff_filespec *, size_t *);

static int parse_diff_color_slot(const char *var, int ofs)
{
	if (!strcasecmp(var+ofs, "plain"))
		return DIFF_PLAIN;
	if (!strcasecmp(var+ofs, "meta"))
		return DIFF_METAINFO;
	if (!strcasecmp(var+ofs, "frag"))
		return DIFF_FRAGINFO;
	if (!strcasecmp(var+ofs, "old"))
		return DIFF_FILE_OLD;
	if (!strcasecmp(var+ofs, "new"))
		return DIFF_FILE_NEW;
	if (!strcasecmp(var+ofs, "commit"))
		return DIFF_COMMIT;
	if (!strcasecmp(var+ofs, "whitespace"))
		return DIFF_WHITESPACE;
	die("bad config variable '%s'", var);
}

/*
 * These are to give UI layer defaults.
 * The core-level commands such as git-diff-files should
 * never be affected by the setting of diff.renames
 * the user happens to have in the configuration file.
 */
int git_diff_ui_config(const char *var, const char *value, void *cb)
{
	if (!strcmp(var, "diff.color") || !strcmp(var, "color.diff")) {
		diff_use_color_default = git_config_colorbool(var, value, -1);
		return 0;
	}
	if (!strcmp(var, "diff.renames")) {
		if (!value)
			diff_detect_rename_default = DIFF_DETECT_RENAME;
		else if (!strcasecmp(value, "copies") ||
			 !strcasecmp(value, "copy"))
			diff_detect_rename_default = DIFF_DETECT_COPY;
		else if (git_config_bool(var,value))
			diff_detect_rename_default = DIFF_DETECT_RENAME;
		return 0;
	}
	if (!strcmp(var, "diff.autorefreshindex")) {
		diff_auto_refresh_index = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "diff.mnemonicprefix")) {
		diff_mnemonic_prefix = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "diff.external"))
		return git_config_string(&external_diff_cmd_cfg, var, value);
	if (!strcmp(var, "diff.wordregex"))
		return git_config_string(&diff_word_regex_cfg, var, value);

	return git_diff_basic_config(var, value, cb);
}

int git_diff_basic_config(const char *var, const char *value, void *cb)
{
	if (!strcmp(var, "diff.renamelimit")) {
		diff_rename_limit_default = git_config_int(var, value);
		return 0;
	}

	switch (userdiff_config(var, value)) {
		case 0: break;
		case -1: return -1;
		default: return 0;
	}

	if (!prefixcmp(var, "diff.color.") || !prefixcmp(var, "color.diff.")) {
		int slot = parse_diff_color_slot(var, 11);
		if (!value)
			return config_error_nonbool(var);
		color_parse(value, var, diff_colors[slot]);
		return 0;
	}

	/* like GNU diff's --suppress-blank-empty option  */
	if (!strcmp(var, "diff.suppressblankempty") ||
			/* for backwards compatibility */
			!strcmp(var, "diff.suppress-blank-empty")) {
		diff_suppress_blank_empty = git_config_bool(var, value);
		return 0;
	}

	return git_color_default_config(var, value, cb);
}

static char *quote_two(const char *one, const char *two)
{
	int need_one = quote_c_style(one, NULL, NULL, 1);
	int need_two = quote_c_style(two, NULL, NULL, 1);
	struct strbuf res = STRBUF_INIT;

	if (need_one + need_two) {
		strbuf_addch(&res, '"');
		quote_c_style(one, &res, NULL, 1);
		quote_c_style(two, &res, NULL, 1);
		strbuf_addch(&res, '"');
	} else {
		strbuf_addstr(&res, one);
		strbuf_addstr(&res, two);
	}
	return strbuf_detach(&res, NULL);
}

static const char *external_diff(void)
{
	static const char *external_diff_cmd = NULL;
	static int done_preparing = 0;

	if (done_preparing)
		return external_diff_cmd;
	external_diff_cmd = getenv("GIT_EXTERNAL_DIFF");
	if (!external_diff_cmd)
		external_diff_cmd = external_diff_cmd_cfg;
	done_preparing = 1;
	return external_diff_cmd;
}

static struct diff_tempfile {
	const char *name; /* filename external diff should read from */
	char hex[41];
	char mode[10];
	char tmp_path[PATH_MAX];
} diff_temp[2];

static struct diff_tempfile *claim_diff_tempfile(void) {
	int i;
	for (i = 0; i < ARRAY_SIZE(diff_temp); i++)
		if (!diff_temp[i].name)
			return diff_temp + i;
	die("BUG: diff is failing to clean up its tempfiles");
}

static int remove_tempfile_installed;

static void remove_tempfile(void)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(diff_temp); i++)
		if (diff_temp[i].name == diff_temp[i].tmp_path) {
			unlink(diff_temp[i].name);
			diff_temp[i].name = NULL;
		}
}

static void remove_tempfile_on_signal(int signo)
{
	remove_tempfile();
	sigchain_pop(signo);
	raise(signo);
}

static int count_lines(const char *data, int size)
{
	int count, ch, completely_empty = 1, nl_just_seen = 0;
	count = 0;
	while (0 < size--) {
		ch = *data++;
		if (ch == '\n') {
			count++;
			nl_just_seen = 1;
			completely_empty = 0;
		}
		else {
			nl_just_seen = 0;
			completely_empty = 0;
		}
	}
	if (completely_empty)
		return 0;
	if (!nl_just_seen)
		count++; /* no trailing newline */
	return count;
}

static void print_line_count(FILE *file, int count)
{
	switch (count) {
	case 0:
		fprintf(file, "0,0");
		break;
	case 1:
		fprintf(file, "1");
		break;
	default:
		fprintf(file, "1,%d", count);
		break;
	}
}

static void copy_file_with_prefix(FILE *file,
				  int prefix, const char *data, int size,
				  const char *set, const char *reset)
{
	int ch, nl_just_seen = 1;
	while (0 < size--) {
		ch = *data++;
		if (nl_just_seen) {
			fputs(set, file);
			putc(prefix, file);
		}
		if (ch == '\n') {
			nl_just_seen = 1;
			fputs(reset, file);
		} else
			nl_just_seen = 0;
		putc(ch, file);
	}
	if (!nl_just_seen)
		fprintf(file, "%s\n\\ No newline at end of file\n", reset);
}

static void emit_rewrite_diff(const char *name_a,
			      const char *name_b,
			      struct diff_filespec *one,
			      struct diff_filespec *two,
			      const char *textconv_one,
			      const char *textconv_two,
			      struct diff_options *o)
{
	int lc_a, lc_b;
	int color_diff = DIFF_OPT_TST(o, COLOR_DIFF);
	const char *name_a_tab, *name_b_tab;
	const char *metainfo = diff_get_color(color_diff, DIFF_METAINFO);
	const char *fraginfo = diff_get_color(color_diff, DIFF_FRAGINFO);
	const char *old = diff_get_color(color_diff, DIFF_FILE_OLD);
	const char *new = diff_get_color(color_diff, DIFF_FILE_NEW);
	const char *reset = diff_get_color(color_diff, DIFF_RESET);
	static struct strbuf a_name = STRBUF_INIT, b_name = STRBUF_INIT;
	const char *a_prefix, *b_prefix;
	const char *data_one, *data_two;
	size_t size_one, size_two;

	if (diff_mnemonic_prefix && DIFF_OPT_TST(o, REVERSE_DIFF)) {
		a_prefix = o->b_prefix;
		b_prefix = o->a_prefix;
	} else {
		a_prefix = o->a_prefix;
		b_prefix = o->b_prefix;
	}

	name_a += (*name_a == '/');
	name_b += (*name_b == '/');
	name_a_tab = strchr(name_a, ' ') ? "\t" : "";
	name_b_tab = strchr(name_b, ' ') ? "\t" : "";

	strbuf_reset(&a_name);
	strbuf_reset(&b_name);
	quote_two_c_style(&a_name, a_prefix, name_a, 0);
	quote_two_c_style(&b_name, b_prefix, name_b, 0);

	diff_populate_filespec(one, 0);
	diff_populate_filespec(two, 0);
	if (textconv_one) {
		data_one = run_textconv(textconv_one, one, &size_one);
		if (!data_one)
			die("unable to read files to diff");
	}
	else {
		data_one = one->data;
		size_one = one->size;
	}
	if (textconv_two) {
		data_two = run_textconv(textconv_two, two, &size_two);
		if (!data_two)
			die("unable to read files to diff");
	}
	else {
		data_two = two->data;
		size_two = two->size;
	}

	lc_a = count_lines(data_one, size_one);
	lc_b = count_lines(data_two, size_two);
	fprintf(o->file,
		"%s--- %s%s%s\n%s+++ %s%s%s\n%s@@ -",
		metainfo, a_name.buf, name_a_tab, reset,
		metainfo, b_name.buf, name_b_tab, reset, fraginfo);
	print_line_count(o->file, lc_a);
	fprintf(o->file, " +");
	print_line_count(o->file, lc_b);
	fprintf(o->file, " @@%s\n", reset);
	if (lc_a)
		copy_file_with_prefix(o->file, '-', data_one, size_one, old, reset);
	if (lc_b)
		copy_file_with_prefix(o->file, '+', data_two, size_two, new, reset);
}

static int fill_mmfile(mmfile_t *mf, struct diff_filespec *one)
{
	if (!DIFF_FILE_VALID(one)) {
		mf->ptr = (char *)""; /* does not matter */
		mf->size = 0;
		return 0;
	}
	else if (diff_populate_filespec(one, 0))
		return -1;

	mf->ptr = one->data;
	mf->size = one->size;
	return 0;
}

struct diff_words_buffer {
	mmfile_t text;
	long alloc;
	struct diff_words_orig {
		const char *begin, *end;
	} *orig;
	int orig_nr, orig_alloc;
};

static void diff_words_append(char *line, unsigned long len,
		struct diff_words_buffer *buffer)
{
	ALLOC_GROW(buffer->text.ptr, buffer->text.size + len, buffer->alloc);
	line++;
	len--;
	memcpy(buffer->text.ptr + buffer->text.size, line, len);
	buffer->text.size += len;
	buffer->text.ptr[buffer->text.size] = '\0';
}

struct diff_words_data {
	struct diff_words_buffer minus, plus;
	const char *current_plus;
	FILE *file;
	regex_t *word_regex;
};

static void fn_out_diff_words_aux(void *priv, char *line, unsigned long len)
{
	struct diff_words_data *diff_words = priv;
	int minus_first, minus_len, plus_first, plus_len;
	const char *minus_begin, *minus_end, *plus_begin, *plus_end;

	if (line[0] != '@' || parse_hunk_header(line, len,
			&minus_first, &minus_len, &plus_first, &plus_len))
		return;

	/* POSIX requires that first be decremented by one if len == 0... */
	if (minus_len) {
		minus_begin = diff_words->minus.orig[minus_first].begin;
		minus_end =
			diff_words->minus.orig[minus_first + minus_len - 1].end;
	} else
		minus_begin = minus_end =
			diff_words->minus.orig[minus_first].end;

	if (plus_len) {
		plus_begin = diff_words->plus.orig[plus_first].begin;
		plus_end = diff_words->plus.orig[plus_first + plus_len - 1].end;
	} else
		plus_begin = plus_end = diff_words->plus.orig[plus_first].end;

	if (diff_words->current_plus != plus_begin)
		fwrite(diff_words->current_plus,
				plus_begin - diff_words->current_plus, 1,
				diff_words->file);
	if (minus_begin != minus_end)
		color_fwrite_lines(diff_words->file,
				diff_get_color(1, DIFF_FILE_OLD),
				minus_end - minus_begin, minus_begin);
	if (plus_begin != plus_end)
		color_fwrite_lines(diff_words->file,
				diff_get_color(1, DIFF_FILE_NEW),
				plus_end - plus_begin, plus_begin);

	diff_words->current_plus = plus_end;
}

/* This function starts looking at *begin, and returns 0 iff a word was found. */
static int find_word_boundaries(mmfile_t *buffer, regex_t *word_regex,
		int *begin, int *end)
{
	if (word_regex && *begin < buffer->size) {
		regmatch_t match[1];
		if (!regexec(word_regex, buffer->ptr + *begin, 1, match, 0)) {
			char *p = memchr(buffer->ptr + *begin + match[0].rm_so,
					'\n', match[0].rm_eo - match[0].rm_so);
			*end = p ? p - buffer->ptr : match[0].rm_eo + *begin;
			*begin += match[0].rm_so;
			return *begin >= *end;
		}
		return -1;
	}

	/* find the next word */
	while (*begin < buffer->size && isspace(buffer->ptr[*begin]))
		(*begin)++;
	if (*begin >= buffer->size)
		return -1;

	/* find the end of the word */
	*end = *begin + 1;
	while (*end < buffer->size && !isspace(buffer->ptr[*end]))
		(*end)++;

	return 0;
}

/*
 * This function splits the words in buffer->text, stores the list with
 * newline separator into out, and saves the offsets of the original words
 * in buffer->orig.
 */
static void diff_words_fill(struct diff_words_buffer *buffer, mmfile_t *out,
		regex_t *word_regex)
{
	int i, j;
	long alloc = 0;

	out->size = 0;
	out->ptr = NULL;

	/* fake an empty "0th" word */
	ALLOC_GROW(buffer->orig, 1, buffer->orig_alloc);
	buffer->orig[0].begin = buffer->orig[0].end = buffer->text.ptr;
	buffer->orig_nr = 1;

	for (i = 0; i < buffer->text.size; i++) {
		if (find_word_boundaries(&buffer->text, word_regex, &i, &j))
			return;

		/* store original boundaries */
		ALLOC_GROW(buffer->orig, buffer->orig_nr + 1,
				buffer->orig_alloc);
		buffer->orig[buffer->orig_nr].begin = buffer->text.ptr + i;
		buffer->orig[buffer->orig_nr].end = buffer->text.ptr + j;
		buffer->orig_nr++;

		/* store one word */
		ALLOC_GROW(out->ptr, out->size + j - i + 1, alloc);
		memcpy(out->ptr + out->size, buffer->text.ptr + i, j - i);
		out->ptr[out->size + j - i] = '\n';
		out->size += j - i + 1;

		i = j - 1;
	}
}

/* this executes the word diff on the accumulated buffers */
static void diff_words_show(struct diff_words_data *diff_words)
{
	xpparam_t xpp;
	xdemitconf_t xecfg;
	xdemitcb_t ecb;
	mmfile_t minus, plus;

	/* special case: only removal */
	if (!diff_words->plus.text.size) {
		color_fwrite_lines(diff_words->file,
			diff_get_color(1, DIFF_FILE_OLD),
			diff_words->minus.text.size, diff_words->minus.text.ptr);
		diff_words->minus.text.size = 0;
		return;
	}

	diff_words->current_plus = diff_words->plus.text.ptr;

	memset(&xpp, 0, sizeof(xpp));
	memset(&xecfg, 0, sizeof(xecfg));
	diff_words_fill(&diff_words->minus, &minus, diff_words->word_regex);
	diff_words_fill(&diff_words->plus, &plus, diff_words->word_regex);
	xpp.flags = XDF_NEED_MINIMAL;
	/* as only the hunk header will be parsed, we need a 0-context */
	xecfg.ctxlen = 0;
	xdi_diff_outf(&minus, &plus, fn_out_diff_words_aux, diff_words,
		      &xpp, &xecfg, &ecb);
	free(minus.ptr);
	free(plus.ptr);
	if (diff_words->current_plus != diff_words->plus.text.ptr +
			diff_words->plus.text.size)
		fwrite(diff_words->current_plus,
			diff_words->plus.text.ptr + diff_words->plus.text.size
			- diff_words->current_plus, 1,
			diff_words->file);
	diff_words->minus.text.size = diff_words->plus.text.size = 0;
}

typedef unsigned long (*sane_truncate_fn)(char *line, unsigned long len);

struct emit_callback {
	int nparents, color_diff;
	unsigned ws_rule;
	sane_truncate_fn truncate;
	const char **label_path;
	struct diff_words_data *diff_words;
	int *found_changesp;
	FILE *file;
};

static void free_diff_words_data(struct emit_callback *ecbdata)
{
	if (ecbdata->diff_words) {
		/* flush buffers */
		if (ecbdata->diff_words->minus.text.size ||
				ecbdata->diff_words->plus.text.size)
			diff_words_show(ecbdata->diff_words);

		free (ecbdata->diff_words->minus.text.ptr);
		free (ecbdata->diff_words->minus.orig);
		free (ecbdata->diff_words->plus.text.ptr);
		free (ecbdata->diff_words->plus.orig);
		free(ecbdata->diff_words->word_regex);
		free(ecbdata->diff_words);
		ecbdata->diff_words = NULL;
	}
}

const char *diff_get_color(int diff_use_color, enum color_diff ix)
{
	if (diff_use_color)
		return diff_colors[ix];
	return "";
}

static void emit_line(FILE *file, const char *set, const char *reset, const char *line, int len)
{
	int has_trailing_newline, has_trailing_carriage_return;

	has_trailing_newline = (len > 0 && line[len-1] == '\n');
	if (has_trailing_newline)
		len--;
	has_trailing_carriage_return = (len > 0 && line[len-1] == '\r');
	if (has_trailing_carriage_return)
		len--;

	fputs(set, file);
	fwrite(line, len, 1, file);
	fputs(reset, file);
	if (has_trailing_carriage_return)
		fputc('\r', file);
	if (has_trailing_newline)
		fputc('\n', file);
}

static void emit_add_line(const char *reset, struct emit_callback *ecbdata, const char *line, int len)
{
	const char *ws = diff_get_color(ecbdata->color_diff, DIFF_WHITESPACE);
	const char *set = diff_get_color(ecbdata->color_diff, DIFF_FILE_NEW);

	if (!*ws)
		emit_line(ecbdata->file, set, reset, line, len);
	else {
		/* Emit just the prefix, then the rest. */
		emit_line(ecbdata->file, set, reset, line, ecbdata->nparents);
		ws_check_emit(line + ecbdata->nparents,
			      len - ecbdata->nparents, ecbdata->ws_rule,
			      ecbdata->file, set, reset, ws);
	}
}

static unsigned long sane_truncate_line(struct emit_callback *ecb, char *line, unsigned long len)
{
	const char *cp;
	unsigned long allot;
	size_t l = len;

	if (ecb->truncate)
		return ecb->truncate(line, len);
	cp = line;
	allot = l;
	while (0 < l) {
		(void) utf8_width(&cp, &l);
		if (!cp)
			break; /* truncated in the middle? */
	}
	return allot - l;
}

static void fn_out_consume(void *priv, char *line, unsigned long len)
{
	int i;
	int color;
	struct emit_callback *ecbdata = priv;
	const char *meta = diff_get_color(ecbdata->color_diff, DIFF_METAINFO);
	const char *plain = diff_get_color(ecbdata->color_diff, DIFF_PLAIN);
	const char *reset = diff_get_color(ecbdata->color_diff, DIFF_RESET);

	*(ecbdata->found_changesp) = 1;

	if (ecbdata->label_path[0]) {
		const char *name_a_tab, *name_b_tab;

		name_a_tab = strchr(ecbdata->label_path[0], ' ') ? "\t" : "";
		name_b_tab = strchr(ecbdata->label_path[1], ' ') ? "\t" : "";

		fprintf(ecbdata->file, "%s--- %s%s%s\n",
			meta, ecbdata->label_path[0], reset, name_a_tab);
		fprintf(ecbdata->file, "%s+++ %s%s%s\n",
			meta, ecbdata->label_path[1], reset, name_b_tab);
		ecbdata->label_path[0] = ecbdata->label_path[1] = NULL;
	}

	if (diff_suppress_blank_empty
	    && len == 2 && line[0] == ' ' && line[1] == '\n') {
		line[0] = '\n';
		len = 1;
	}

	/* This is not really necessary for now because
	 * this codepath only deals with two-way diffs.
	 */
	for (i = 0; i < len && line[i] == '@'; i++)
		;
	if (2 <= i && i < len && line[i] == ' ') {
		ecbdata->nparents = i - 1;
		len = sane_truncate_line(ecbdata, line, len);
		emit_line(ecbdata->file,
			  diff_get_color(ecbdata->color_diff, DIFF_FRAGINFO),
			  reset, line, len);
		if (line[len-1] != '\n')
			putc('\n', ecbdata->file);
		return;
	}

	if (len < ecbdata->nparents) {
		emit_line(ecbdata->file, reset, reset, line, len);
		return;
	}

	color = DIFF_PLAIN;
	if (ecbdata->diff_words && ecbdata->nparents != 1)
		/* fall back to normal diff */
		free_diff_words_data(ecbdata);
	if (ecbdata->diff_words) {
		if (line[0] == '-') {
			diff_words_append(line, len,
					  &ecbdata->diff_words->minus);
			return;
		} else if (line[0] == '+') {
			diff_words_append(line, len,
					  &ecbdata->diff_words->plus);
			return;
		}
		if (ecbdata->diff_words->minus.text.size ||
		    ecbdata->diff_words->plus.text.size)
			diff_words_show(ecbdata->diff_words);
		line++;
		len--;
		emit_line(ecbdata->file, plain, reset, line, len);
		return;
	}
	for (i = 0; i < ecbdata->nparents && len; i++) {
		if (line[i] == '-')
			color = DIFF_FILE_OLD;
		else if (line[i] == '+')
			color = DIFF_FILE_NEW;
	}

	if (color != DIFF_FILE_NEW) {
		emit_line(ecbdata->file,
			  diff_get_color(ecbdata->color_diff, color),
			  reset, line, len);
		return;
	}
	emit_add_line(reset, ecbdata, line, len);
}

static char *pprint_rename(const char *a, const char *b)
{
	const char *old = a;
	const char *new = b;
	struct strbuf name = STRBUF_INIT;
	int pfx_length, sfx_length;
	int len_a = strlen(a);
	int len_b = strlen(b);
	int a_midlen, b_midlen;
	int qlen_a = quote_c_style(a, NULL, NULL, 0);
	int qlen_b = quote_c_style(b, NULL, NULL, 0);

	if (qlen_a || qlen_b) {
		quote_c_style(a, &name, NULL, 0);
		strbuf_addstr(&name, " => ");
		quote_c_style(b, &name, NULL, 0);
		return strbuf_detach(&name, NULL);
	}

	/* Find common prefix */
	pfx_length = 0;
	while (*old && *new && *old == *new) {
		if (*old == '/')
			pfx_length = old - a + 1;
		old++;
		new++;
	}

	/* Find common suffix */
	old = a + len_a;
	new = b + len_b;
	sfx_length = 0;
	while (a <= old && b <= new && *old == *new) {
		if (*old == '/')
			sfx_length = len_a - (old - a);
		old--;
		new--;
	}

	/*
	 * pfx{mid-a => mid-b}sfx
	 * {pfx-a => pfx-b}sfx
	 * pfx{sfx-a => sfx-b}
	 * name-a => name-b
	 */
	a_midlen = len_a - pfx_length - sfx_length;
	b_midlen = len_b - pfx_length - sfx_length;
	if (a_midlen < 0)
		a_midlen = 0;
	if (b_midlen < 0)
		b_midlen = 0;

	strbuf_grow(&name, pfx_length + a_midlen + b_midlen + sfx_length + 7);
	if (pfx_length + sfx_length) {
		strbuf_add(&name, a, pfx_length);
		strbuf_addch(&name, '{');
	}
	strbuf_add(&name, a + pfx_length, a_midlen);
	strbuf_addstr(&name, " => ");
	strbuf_add(&name, b + pfx_length, b_midlen);
	if (pfx_length + sfx_length) {
		strbuf_addch(&name, '}');
		strbuf_add(&name, a + len_a - sfx_length, sfx_length);
	}
	return strbuf_detach(&name, NULL);
}

struct diffstat_t {
	int nr;
	int alloc;
	struct diffstat_file {
		char *from_name;
		char *name;
		char *print_name;
		unsigned is_unmerged:1;
		unsigned is_binary:1;
		unsigned is_renamed:1;
		unsigned int added, deleted;
	} **files;
};

static struct diffstat_file *diffstat_add(struct diffstat_t *diffstat,
					  const char *name_a,
					  const char *name_b)
{
	struct diffstat_file *x;
	x = xcalloc(sizeof (*x), 1);
	if (diffstat->nr == diffstat->alloc) {
		diffstat->alloc = alloc_nr(diffstat->alloc);
		diffstat->files = xrealloc(diffstat->files,
				diffstat->alloc * sizeof(x));
	}
	diffstat->files[diffstat->nr++] = x;
	if (name_b) {
		x->from_name = xstrdup(name_a);
		x->name = xstrdup(name_b);
		x->is_renamed = 1;
	}
	else {
		x->from_name = NULL;
		x->name = xstrdup(name_a);
	}
	return x;
}

static void diffstat_consume(void *priv, char *line, unsigned long len)
{
	struct diffstat_t *diffstat = priv;
	struct diffstat_file *x = diffstat->files[diffstat->nr - 1];

	if (line[0] == '+')
		x->added++;
	else if (line[0] == '-')
		x->deleted++;
}

const char mime_boundary_leader[] = "------------";

static int scale_linear(int it, int width, int max_change)
{
	/*
	 * make sure that at least one '-' is printed if there were deletions,
	 * and likewise for '+'.
	 */
	if (max_change < 2)
		return it;
	return ((it - 1) * (width - 1) + max_change - 1) / (max_change - 1);
}

static void show_name(FILE *file,
		      const char *prefix, const char *name, int len,
		      const char *reset, const char *set)
{
	fprintf(file, " %s%s%-*s%s |", set, prefix, len, name, reset);
}

static void show_graph(FILE *file, char ch, int cnt, const char *set, const char *reset)
{
	if (cnt <= 0)
		return;
	fprintf(file, "%s", set);
	while (cnt--)
		putc(ch, file);
	fprintf(file, "%s", reset);
}

static void fill_print_name(struct diffstat_file *file)
{
	char *pname;

	if (file->print_name)
		return;

	if (!file->is_renamed) {
		struct strbuf buf = STRBUF_INIT;
		if (quote_c_style(file->name, &buf, NULL, 0)) {
			pname = strbuf_detach(&buf, NULL);
		} else {
			pname = file->name;
			strbuf_release(&buf);
		}
	} else {
		pname = pprint_rename(file->from_name, file->name);
	}
	file->print_name = pname;
}

static void show_stats(struct diffstat_t* data, struct diff_options *options)
{
	int i, len, add, del, total, adds = 0, dels = 0;
	int max_change = 0, max_len = 0;
	int total_files = data->nr;
	int width, name_width;
	const char *reset, *set, *add_c, *del_c;

	if (data->nr == 0)
		return;

	width = options->stat_width ? options->stat_width : 80;
	name_width = options->stat_name_width ? options->stat_name_width : 50;

	/* Sanity: give at least 5 columns to the graph,
	 * but leave at least 10 columns for the name.
	 */
	if (width < 25)
		width = 25;
	if (name_width < 10)
		name_width = 10;
	else if (width < name_width + 15)
		name_width = width - 15;

	/* Find the longest filename and max number of changes */
	reset = diff_get_color_opt(options, DIFF_RESET);
	set   = diff_get_color_opt(options, DIFF_PLAIN);
	add_c = diff_get_color_opt(options, DIFF_FILE_NEW);
	del_c = diff_get_color_opt(options, DIFF_FILE_OLD);

	for (i = 0; i < data->nr; i++) {
		struct diffstat_file *file = data->files[i];
		int change = file->added + file->deleted;
		fill_print_name(file);
		len = strlen(file->print_name);
		if (max_len < len)
			max_len = len;

		if (file->is_binary || file->is_unmerged)
			continue;
		if (max_change < change)
			max_change = change;
	}

	/* Compute the width of the graph part;
	 * 10 is for one blank at the beginning of the line plus
	 * " | count " between the name and the graph.
	 *
	 * From here on, name_width is the width of the name area,
	 * and width is the width of the graph area.
	 */
	name_width = (name_width < max_len) ? name_width : max_len;
	if (width < (name_width + 10) + max_change)
		width = width - (name_width + 10);
	else
		width = max_change;

	for (i = 0; i < data->nr; i++) {
		const char *prefix = "";
		char *name = data->files[i]->print_name;
		int added = data->files[i]->added;
		int deleted = data->files[i]->deleted;
		int name_len;

		/*
		 * "scale" the filename
		 */
		len = name_width;
		name_len = strlen(name);
		if (name_width < name_len) {
			char *slash;
			prefix = "...";
			len -= 3;
			name += name_len - len;
			slash = strchr(name, '/');
			if (slash)
				name = slash;
		}

		if (data->files[i]->is_binary) {
			show_name(options->file, prefix, name, len, reset, set);
			fprintf(options->file, "  Bin ");
			fprintf(options->file, "%s%d%s", del_c, deleted, reset);
			fprintf(options->file, " -> ");
			fprintf(options->file, "%s%d%s", add_c, added, reset);
			fprintf(options->file, " bytes");
			fprintf(options->file, "\n");
			continue;
		}
		else if (data->files[i]->is_unmerged) {
			show_name(options->file, prefix, name, len, reset, set);
			fprintf(options->file, "  Unmerged\n");
			continue;
		}
		else if (!data->files[i]->is_renamed &&
			 (added + deleted == 0)) {
			total_files--;
			continue;
		}

		/*
		 * scale the add/delete
		 */
		add = added;
		del = deleted;
		total = add + del;
		adds += add;
		dels += del;

		if (width <= max_change) {
			add = scale_linear(add, width, max_change);
			del = scale_linear(del, width, max_change);
			total = add + del;
		}
		show_name(options->file, prefix, name, len, reset, set);
		fprintf(options->file, "%5d%s", added + deleted,
				added + deleted ? " " : "");
		show_graph(options->file, '+', add, add_c, reset);
		show_graph(options->file, '-', del, del_c, reset);
		fprintf(options->file, "\n");
	}
	fprintf(options->file,
	       "%s %d files changed, %d insertions(+), %d deletions(-)%s\n",
	       set, total_files, adds, dels, reset);
}

static void show_shortstats(struct diffstat_t* data, struct diff_options *options)
{
	int i, adds = 0, dels = 0, total_files = data->nr;

	if (data->nr == 0)
		return;

	for (i = 0; i < data->nr; i++) {
		if (!data->files[i]->is_binary &&
		    !data->files[i]->is_unmerged) {
			int added = data->files[i]->added;
			int deleted= data->files[i]->deleted;
			if (!data->files[i]->is_renamed &&
			    (added + deleted == 0)) {
				total_files--;
			} else {
				adds += added;
				dels += deleted;
			}
		}
	}
	fprintf(options->file, " %d files changed, %d insertions(+), %d deletions(-)\n",
	       total_files, adds, dels);
}

static void show_numstat(struct diffstat_t* data, struct diff_options *options)
{
	int i;

	if (data->nr == 0)
		return;

	for (i = 0; i < data->nr; i++) {
		struct diffstat_file *file = data->files[i];

		if (file->is_binary)
			fprintf(options->file, "-\t-\t");
		else
			fprintf(options->file,
				"%d\t%d\t", file->added, file->deleted);
		if (options->line_termination) {
			fill_print_name(file);
			if (!file->is_renamed)
				write_name_quoted(file->name, options->file,
						  options->line_termination);
			else {
				fputs(file->print_name, options->file);
				putc(options->line_termination, options->file);
			}
		} else {
			if (file->is_renamed) {
				putc('\0', options->file);
				write_name_quoted(file->from_name, options->file, '\0');
			}
			write_name_quoted(file->name, options->file, '\0');
		}
	}
}

struct dirstat_file {
	const char *name;
	unsigned long changed;
};

struct dirstat_dir {
	struct dirstat_file *files;
	int alloc, nr, percent, cumulative;
};

static long gather_dirstat(FILE *file, struct dirstat_dir *dir, unsigned long changed, const char *base, int baselen)
{
	unsigned long this_dir = 0;
	unsigned int sources = 0;

	while (dir->nr) {
		struct dirstat_file *f = dir->files;
		int namelen = strlen(f->name);
		unsigned long this;
		char *slash;

		if (namelen < baselen)
			break;
		if (memcmp(f->name, base, baselen))
			break;
		slash = strchr(f->name + baselen, '/');
		if (slash) {
			int newbaselen = slash + 1 - f->name;
			this = gather_dirstat(file, dir, changed, f->name, newbaselen);
			sources++;
		} else {
			this = f->changed;
			dir->files++;
			dir->nr--;
			sources += 2;
		}
		this_dir += this;
	}

	/*
	 * We don't report dirstat's for
	 *  - the top level
	 *  - or cases where everything came from a single directory
	 *    under this directory (sources == 1).
	 */
	if (baselen && sources != 1) {
		int permille = this_dir * 1000 / changed;
		if (permille) {
			int percent = permille / 10;
			if (percent >= dir->percent) {
				fprintf(file, "%4d.%01d%% %.*s\n", percent, permille % 10, baselen, base);
				if (!dir->cumulative)
					return 0;
			}
		}
	}
	return this_dir;
}

static int dirstat_compare(const void *_a, const void *_b)
{
	const struct dirstat_file *a = _a;
	const struct dirstat_file *b = _b;
	return strcmp(a->name, b->name);
}

static void show_dirstat(struct diff_options *options)
{
	int i;
	unsigned long changed;
	struct dirstat_dir dir;
	struct diff_queue_struct *q = &diff_queued_diff;

	dir.files = NULL;
	dir.alloc = 0;
	dir.nr = 0;
	dir.percent = options->dirstat_percent;
	dir.cumulative = DIFF_OPT_TST(options, DIRSTAT_CUMULATIVE);

	changed = 0;
	for (i = 0; i < q->nr; i++) {
		struct diff_filepair *p = q->queue[i];
		const char *name;
		unsigned long copied, added, damage;

		name = p->one->path ? p->one->path : p->two->path;

		if (DIFF_FILE_VALID(p->one) && DIFF_FILE_VALID(p->two)) {
			diff_populate_filespec(p->one, 0);
			diff_populate_filespec(p->two, 0);
			diffcore_count_changes(p->one, p->two, NULL, NULL, 0,
					       &copied, &added);
			diff_free_filespec_data(p->one);
			diff_free_filespec_data(p->two);
		} else if (DIFF_FILE_VALID(p->one)) {
			diff_populate_filespec(p->one, 1);
			copied = added = 0;
			diff_free_filespec_data(p->one);
		} else if (DIFF_FILE_VALID(p->two)) {
			diff_populate_filespec(p->two, 1);
			copied = 0;
			added = p->two->size;
			diff_free_filespec_data(p->two);
		} else
			continue;

		/*
		 * Original minus copied is the removed material,
		 * added is the new material.  They are both damages
		 * made to the preimage. In --dirstat-by-file mode, count
		 * damaged files, not damaged lines. This is done by
		 * counting only a single damaged line per file.
		 */
		damage = (p->one->size - copied) + added;
		if (DIFF_OPT_TST(options, DIRSTAT_BY_FILE) && damage > 0)
			damage = 1;

		ALLOC_GROW(dir.files, dir.nr + 1, dir.alloc);
		dir.files[dir.nr].name = name;
		dir.files[dir.nr].changed = damage;
		changed += damage;
		dir.nr++;
	}

	/* This can happen even with many files, if everything was renames */
	if (!changed)
		return;

	/* Show all directories with more than x% of the changes */
	qsort(dir.files, dir.nr, sizeof(dir.files[0]), dirstat_compare);
	gather_dirstat(options->file, &dir, changed, "", 0);
}

static void free_diffstat_info(struct diffstat_t *diffstat)
{
	int i;
	for (i = 0; i < diffstat->nr; i++) {
		struct diffstat_file *f = diffstat->files[i];
		if (f->name != f->print_name)
			free(f->print_name);
		free(f->name);
		free(f->from_name);
		free(f);
	}
	free(diffstat->files);
}

struct checkdiff_t {
	const char *filename;
	int lineno;
	struct diff_options *o;
	unsigned ws_rule;
	unsigned status;
	int trailing_blanks_start;
};

static int is_conflict_marker(const char *line, unsigned long len)
{
	char firstchar;
	int cnt;

	if (len < 8)
		return 0;
	firstchar = line[0];
	switch (firstchar) {
	case '=': case '>': case '<':
		break;
	default:
		return 0;
	}
	for (cnt = 1; cnt < 7; cnt++)
		if (line[cnt] != firstchar)
			return 0;
	/* line[0] thru line[6] are same as firstchar */
	if (firstchar == '=') {
		/* divider between ours and theirs? */
		if (len != 8 || line[7] != '\n')
			return 0;
	} else if (len < 8 || !isspace(line[7])) {
		/* not divider before ours nor after theirs */
		return 0;
	}
	return 1;
}

static void checkdiff_consume(void *priv, char *line, unsigned long len)
{
	struct checkdiff_t *data = priv;
	int color_diff = DIFF_OPT_TST(data->o, COLOR_DIFF);
	const char *ws = diff_get_color(color_diff, DIFF_WHITESPACE);
	const char *reset = diff_get_color(color_diff, DIFF_RESET);
	const char *set = diff_get_color(color_diff, DIFF_FILE_NEW);
	char *err;

	if (line[0] == '+') {
		unsigned bad;
		data->lineno++;
		if (!ws_blank_line(line + 1, len - 1, data->ws_rule))
			data->trailing_blanks_start = 0;
		else if (!data->trailing_blanks_start)
			data->trailing_blanks_start = data->lineno;
		if (is_conflict_marker(line + 1, len - 1)) {
			data->status |= 1;
			fprintf(data->o->file,
				"%s:%d: leftover conflict marker\n",
				data->filename, data->lineno);
		}
		bad = ws_check(line + 1, len - 1, data->ws_rule);
		if (!bad)
			return;
		data->status |= bad;
		err = whitespace_error_string(bad);
		fprintf(data->o->file, "%s:%d: %s.\n",
			data->filename, data->lineno, err);
		free(err);
		emit_line(data->o->file, set, reset, line, 1);
		ws_check_emit(line + 1, len - 1, data->ws_rule,
			      data->o->file, set, reset, ws);
	} else if (line[0] == ' ') {
		data->lineno++;
		data->trailing_blanks_start = 0;
	} else if (line[0] == '@') {
		char *plus = strchr(line, '+');
		if (plus)
			data->lineno = strtol(plus, NULL, 10) - 1;
		else
			die("invalid diff");
		data->trailing_blanks_start = 0;
	}
}

static unsigned char *deflate_it(char *data,
				 unsigned long size,
				 unsigned long *result_size)
{
	int bound;
	unsigned char *deflated;
	z_stream stream;

	memset(&stream, 0, sizeof(stream));
	deflateInit(&stream, zlib_compression_level);
	bound = deflateBound(&stream, size);
	deflated = xmalloc(bound);
	stream.next_out = deflated;
	stream.avail_out = bound;

	stream.next_in = (unsigned char *)data;
	stream.avail_in = size;
	while (deflate(&stream, Z_FINISH) == Z_OK)
		; /* nothing */
	deflateEnd(&stream);
	*result_size = stream.total_out;
	return deflated;
}

static void emit_binary_diff_body(FILE *file, mmfile_t *one, mmfile_t *two)
{
	void *cp;
	void *delta;
	void *deflated;
	void *data;
	unsigned long orig_size;
	unsigned long delta_size;
	unsigned long deflate_size;
	unsigned long data_size;

	/* We could do deflated delta, or we could do just deflated two,
	 * whichever is smaller.
	 */
	delta = NULL;
	deflated = deflate_it(two->ptr, two->size, &deflate_size);
	if (one->size && two->size) {
		delta = diff_delta(one->ptr, one->size,
				   two->ptr, two->size,
				   &delta_size, deflate_size);
		if (delta) {
			void *to_free = delta;
			orig_size = delta_size;
			delta = deflate_it(delta, delta_size, &delta_size);
			free(to_free);
		}
	}

	if (delta && delta_size < deflate_size) {
		fprintf(file, "delta %lu\n", orig_size);
		free(deflated);
		data = delta;
		data_size = delta_size;
	}
	else {
		fprintf(file, "literal %lu\n", two->size);
		free(delta);
		data = deflated;
		data_size = deflate_size;
	}

	/* emit data encoded in base85 */
	cp = data;
	while (data_size) {
		int bytes = (52 < data_size) ? 52 : data_size;
		char line[70];
		data_size -= bytes;
		if (bytes <= 26)
			line[0] = bytes + 'A' - 1;
		else
			line[0] = bytes - 26 + 'a' - 1;
		encode_85(line + 1, cp, bytes);
		cp = (char *) cp + bytes;
		fputs(line, file);
		fputc('\n', file);
	}
	fprintf(file, "\n");
	free(data);
}

static void emit_binary_diff(FILE *file, mmfile_t *one, mmfile_t *two)
{
	fprintf(file, "GIT binary patch\n");
	emit_binary_diff_body(file, one, two);
	emit_binary_diff_body(file, two, one);
}

static void diff_filespec_load_driver(struct diff_filespec *one)
{
	if (!one->driver)
		one->driver = userdiff_find_by_path(one->path);
	if (!one->driver)
		one->driver = userdiff_find_by_name("default");
}

int diff_filespec_is_binary(struct diff_filespec *one)
{
	if (one->is_binary == -1) {
		diff_filespec_load_driver(one);
		if (one->driver->binary != -1)
			one->is_binary = one->driver->binary;
		else {
			if (!one->data && DIFF_FILE_VALID(one))
				diff_populate_filespec(one, 0);
			if (one->data)
				one->is_binary = buffer_is_binary(one->data,
						one->size);
			if (one->is_binary == -1)
				one->is_binary = 0;
		}
	}
	return one->is_binary;
}

static const struct userdiff_funcname *diff_funcname_pattern(struct diff_filespec *one)
{
	diff_filespec_load_driver(one);
	return one->driver->funcname.pattern ? &one->driver->funcname : NULL;
}

static const char *userdiff_word_regex(struct diff_filespec *one)
{
	diff_filespec_load_driver(one);
	return one->driver->word_regex;
}

void diff_set_mnemonic_prefix(struct diff_options *options, const char *a, const char *b)
{
	if (!options->a_prefix)
		options->a_prefix = a;
	if (!options->b_prefix)
		options->b_prefix = b;
}

static const char *get_textconv(struct diff_filespec *one)
{
	if (!DIFF_FILE_VALID(one))
		return NULL;
	if (!S_ISREG(one->mode))
		return NULL;
	diff_filespec_load_driver(one);
	return one->driver->textconv;
}

static void builtin_diff(const char *name_a,
			 const char *name_b,
			 struct diff_filespec *one,
			 struct diff_filespec *two,
			 const char *xfrm_msg,
			 struct diff_options *o,
			 int complete_rewrite)
{
	mmfile_t mf1, mf2;
	const char *lbl[2];
	char *a_one, *b_two;
	const char *set = diff_get_color_opt(o, DIFF_METAINFO);
	const char *reset = diff_get_color_opt(o, DIFF_RESET);
	const char *a_prefix, *b_prefix;
	const char *textconv_one = NULL, *textconv_two = NULL;

	if (DIFF_OPT_TST(o, ALLOW_TEXTCONV)) {
		textconv_one = get_textconv(one);
		textconv_two = get_textconv(two);
	}

	diff_set_mnemonic_prefix(o, "a/", "b/");
	if (DIFF_OPT_TST(o, REVERSE_DIFF)) {
		a_prefix = o->b_prefix;
		b_prefix = o->a_prefix;
	} else {
		a_prefix = o->a_prefix;
		b_prefix = o->b_prefix;
	}

	/* Never use a non-valid filename anywhere if at all possible */
	name_a = DIFF_FILE_VALID(one) ? name_a : name_b;
	name_b = DIFF_FILE_VALID(two) ? name_b : name_a;

	a_one = quote_two(a_prefix, name_a + (*name_a == '/'));
	b_two = quote_two(b_prefix, name_b + (*name_b == '/'));
	lbl[0] = DIFF_FILE_VALID(one) ? a_one : "/dev/null";
	lbl[1] = DIFF_FILE_VALID(two) ? b_two : "/dev/null";
	fprintf(o->file, "%sdiff --git %s %s%s\n", set, a_one, b_two, reset);
	if (lbl[0][0] == '/') {
		/* /dev/null */
		fprintf(o->file, "%snew file mode %06o%s\n", set, two->mode, reset);
		if (xfrm_msg && xfrm_msg[0])
			fprintf(o->file, "%s%s%s\n", set, xfrm_msg, reset);
	}
	else if (lbl[1][0] == '/') {
		fprintf(o->file, "%sdeleted file mode %06o%s\n", set, one->mode, reset);
		if (xfrm_msg && xfrm_msg[0])
			fprintf(o->file, "%s%s%s\n", set, xfrm_msg, reset);
	}
	else {
		if (one->mode != two->mode) {
			fprintf(o->file, "%sold mode %06o%s\n", set, one->mode, reset);
			fprintf(o->file, "%snew mode %06o%s\n", set, two->mode, reset);
		}
		if (xfrm_msg && xfrm_msg[0])
			fprintf(o->file, "%s%s%s\n", set, xfrm_msg, reset);
		/*
		 * we do not run diff between different kind
		 * of objects.
		 */
		if ((one->mode ^ two->mode) & S_IFMT)
			goto free_ab_and_return;
		if (complete_rewrite &&
		    (textconv_one || !diff_filespec_is_binary(one)) &&
		    (textconv_two || !diff_filespec_is_binary(two))) {
			emit_rewrite_diff(name_a, name_b, one, two,
						textconv_one, textconv_two, o);
			o->found_changes = 1;
			goto free_ab_and_return;
		}
	}

	if (fill_mmfile(&mf1, one) < 0 || fill_mmfile(&mf2, two) < 0)
		die("unable to read files to diff");

	if (!DIFF_OPT_TST(o, TEXT) &&
	    ( (diff_filespec_is_binary(one) && !textconv_one) ||
	      (diff_filespec_is_binary(two) && !textconv_two) )) {
		/* Quite common confusing case */
		if (mf1.size == mf2.size &&
		    !memcmp(mf1.ptr, mf2.ptr, mf1.size))
			goto free_ab_and_return;
		if (DIFF_OPT_TST(o, BINARY))
			emit_binary_diff(o->file, &mf1, &mf2);
		else
			fprintf(o->file, "Binary files %s and %s differ\n",
				lbl[0], lbl[1]);
		o->found_changes = 1;
	}
	else {
		/* Crazy xdl interfaces.. */
		const char *diffopts = getenv("GIT_DIFF_OPTS");
		xpparam_t xpp;
		xdemitconf_t xecfg;
		xdemitcb_t ecb;
		struct emit_callback ecbdata;
		const struct userdiff_funcname *pe;

		if (textconv_one) {
			size_t size;
			mf1.ptr = run_textconv(textconv_one, one, &size);
			if (!mf1.ptr)
				die("unable to read files to diff");
			mf1.size = size;
		}
		if (textconv_two) {
			size_t size;
			mf2.ptr = run_textconv(textconv_two, two, &size);
			if (!mf2.ptr)
				die("unable to read files to diff");
			mf2.size = size;
		}

		pe = diff_funcname_pattern(one);
		if (!pe)
			pe = diff_funcname_pattern(two);

		memset(&xpp, 0, sizeof(xpp));
		memset(&xecfg, 0, sizeof(xecfg));
		memset(&ecbdata, 0, sizeof(ecbdata));
		ecbdata.label_path = lbl;
		ecbdata.color_diff = DIFF_OPT_TST(o, COLOR_DIFF);
		ecbdata.found_changesp = &o->found_changes;
		ecbdata.ws_rule = whitespace_rule(name_b ? name_b : name_a);
		ecbdata.file = o->file;
		xpp.flags = XDF_NEED_MINIMAL | o->xdl_opts;
		xecfg.ctxlen = o->context;
		xecfg.interhunkctxlen = o->interhunkcontext;
		xecfg.flags = XDL_EMIT_FUNCNAMES;
		if (pe)
			xdiff_set_find_func(&xecfg, pe->pattern, pe->cflags);
		if (!diffopts)
			;
		else if (!prefixcmp(diffopts, "--unified="))
			xecfg.ctxlen = strtoul(diffopts + 10, NULL, 10);
		else if (!prefixcmp(diffopts, "-u"))
			xecfg.ctxlen = strtoul(diffopts + 2, NULL, 10);
		if (DIFF_OPT_TST(o, COLOR_DIFF_WORDS)) {
			ecbdata.diff_words =
				xcalloc(1, sizeof(struct diff_words_data));
			ecbdata.diff_words->file = o->file;
			if (!o->word_regex)
				o->word_regex = userdiff_word_regex(one);
			if (!o->word_regex)
				o->word_regex = userdiff_word_regex(two);
			if (!o->word_regex)
				o->word_regex = diff_word_regex_cfg;
			if (o->word_regex) {
				ecbdata.diff_words->word_regex = (regex_t *)
					xmalloc(sizeof(regex_t));
				if (regcomp(ecbdata.diff_words->word_regex,
						o->word_regex,
						REG_EXTENDED | REG_NEWLINE))
					die ("Invalid regular expression: %s",
							o->word_regex);
			}
		}
		xdi_diff_outf(&mf1, &mf2, fn_out_consume, &ecbdata,
			      &xpp, &xecfg, &ecb);
		if (DIFF_OPT_TST(o, COLOR_DIFF_WORDS))
			free_diff_words_data(&ecbdata);
		if (textconv_one)
			free(mf1.ptr);
		if (textconv_two)
			free(mf2.ptr);
	}

 free_ab_and_return:
	diff_free_filespec_data(one);
	diff_free_filespec_data(two);
	free(a_one);
	free(b_two);
	return;
}

static void builtin_diffstat(const char *name_a, const char *name_b,
			     struct diff_filespec *one,
			     struct diff_filespec *two,
			     struct diffstat_t *diffstat,
			     struct diff_options *o,
			     int complete_rewrite)
{
	mmfile_t mf1, mf2;
	struct diffstat_file *data;

	data = diffstat_add(diffstat, name_a, name_b);

	if (!one || !two) {
		data->is_unmerged = 1;
		return;
	}
	if (complete_rewrite) {
		diff_populate_filespec(one, 0);
		diff_populate_filespec(two, 0);
		data->deleted = count_lines(one->data, one->size);
		data->added = count_lines(two->data, two->size);
		goto free_and_return;
	}
	if (fill_mmfile(&mf1, one) < 0 || fill_mmfile(&mf2, two) < 0)
		die("unable to read files to diff");

	if (diff_filespec_is_binary(one) || diff_filespec_is_binary(two)) {
		data->is_binary = 1;
		data->added = mf2.size;
		data->deleted = mf1.size;
	} else {
		/* Crazy xdl interfaces.. */
		xpparam_t xpp;
		xdemitconf_t xecfg;
		xdemitcb_t ecb;

		memset(&xpp, 0, sizeof(xpp));
		memset(&xecfg, 0, sizeof(xecfg));
		xpp.flags = XDF_NEED_MINIMAL | o->xdl_opts;
		xdi_diff_outf(&mf1, &mf2, diffstat_consume, diffstat,
			      &xpp, &xecfg, &ecb);
	}

 free_and_return:
	diff_free_filespec_data(one);
	diff_free_filespec_data(two);
}

static void builtin_checkdiff(const char *name_a, const char *name_b,
			      const char *attr_path,
			      struct diff_filespec *one,
			      struct diff_filespec *two,
			      struct diff_options *o)
{
	mmfile_t mf1, mf2;
	struct checkdiff_t data;

	if (!two)
		return;

	memset(&data, 0, sizeof(data));
	data.filename = name_b ? name_b : name_a;
	data.lineno = 0;
	data.o = o;
	data.ws_rule = whitespace_rule(attr_path);

	if (fill_mmfile(&mf1, one) < 0 || fill_mmfile(&mf2, two) < 0)
		die("unable to read files to diff");

	/*
	 * All the other codepaths check both sides, but not checking
	 * the "old" side here is deliberate.  We are checking the newly
	 * introduced changes, and as long as the "new" side is text, we
	 * can and should check what it introduces.
	 */
	if (diff_filespec_is_binary(two))
		goto free_and_return;
	else {
		/* Crazy xdl interfaces.. */
		xpparam_t xpp;
		xdemitconf_t xecfg;
		xdemitcb_t ecb;

		memset(&xpp, 0, sizeof(xpp));
		memset(&xecfg, 0, sizeof(xecfg));
		xecfg.ctxlen = 1; /* at least one context line */
		xpp.flags = XDF_NEED_MINIMAL;
		xdi_diff_outf(&mf1, &mf2, checkdiff_consume, &data,
			      &xpp, &xecfg, &ecb);

		if ((data.ws_rule & WS_TRAILING_SPACE) &&
		    data.trailing_blanks_start) {
			fprintf(o->file, "%s:%d: ends with blank lines.\n",
				data.filename, data.trailing_blanks_start);
			data.status = 1; /* report errors */
		}
	}
 free_and_return:
	diff_free_filespec_data(one);
	diff_free_filespec_data(two);
	if (data.status)
		DIFF_OPT_SET(o, CHECK_FAILED);
}

struct diff_filespec *alloc_filespec(const char *path)
{
	int namelen = strlen(path);
	struct diff_filespec *spec = xmalloc(sizeof(*spec) + namelen + 1);

	memset(spec, 0, sizeof(*spec));
	spec->path = (char *)(spec + 1);
	memcpy(spec->path, path, namelen+1);
	spec->count = 1;
	spec->is_binary = -1;
	return spec;
}

void free_filespec(struct diff_filespec *spec)
{
	if (!--spec->count) {
		diff_free_filespec_data(spec);
		free(spec);
	}
}

void fill_filespec(struct diff_filespec *spec, const unsigned char *sha1,
		   unsigned short mode)
{
	if (mode) {
		spec->mode = canon_mode(mode);
		hashcpy(spec->sha1, sha1);
		spec->sha1_valid = !is_null_sha1(sha1);
	}
}

/*
 * Given a name and sha1 pair, if the index tells us the file in
 * the work tree has that object contents, return true, so that
 * prepare_temp_file() does not have to inflate and extract.
 */
static int reuse_worktree_file(const char *name, const unsigned char *sha1, int want_file)
{
	struct cache_entry *ce;
	struct stat st;
	int pos, len;

	/* We do not read the cache ourselves here, because the
	 * benchmark with my previous version that always reads cache
	 * shows that it makes things worse for diff-tree comparing
	 * two linux-2.6 kernel trees in an already checked out work
	 * tree.  This is because most diff-tree comparisons deal with
	 * only a small number of files, while reading the cache is
	 * expensive for a large project, and its cost outweighs the
	 * savings we get by not inflating the object to a temporary
	 * file.  Practically, this code only helps when we are used
	 * by diff-cache --cached, which does read the cache before
	 * calling us.
	 */
	if (!active_cache)
		return 0;

	/* We want to avoid the working directory if our caller
	 * doesn't need the data in a normal file, this system
	 * is rather slow with its stat/open/mmap/close syscalls,
	 * and the object is contained in a pack file.  The pack
	 * is probably already open and will be faster to obtain
	 * the data through than the working directory.  Loose
	 * objects however would tend to be slower as they need
	 * to be individually opened and inflated.
	 */
	if (!FAST_WORKING_DIRECTORY && !want_file && has_sha1_pack(sha1, NULL))
		return 0;

	len = strlen(name);
	pos = cache_name_pos(name, len);
	if (pos < 0)
		return 0;
	ce = active_cache[pos];

	/*
	 * This is not the sha1 we are looking for, or
	 * unreusable because it is not a regular file.
	 */
	if (hashcmp(sha1, ce->sha1) || !S_ISREG(ce->ce_mode))
		return 0;

	/*
	 * If ce matches the file in the work tree, we can reuse it.
	 */
	if (ce_uptodate(ce) ||
	    (!lstat(name, &st) && !ce_match_stat(ce, &st, 0)))
		return 1;

	return 0;
}

static int populate_from_stdin(struct diff_filespec *s)
{
	struct strbuf buf = STRBUF_INIT;
	size_t size = 0;

	if (strbuf_read(&buf, 0, 0) < 0)
		return error("error while reading from stdin %s",
				     strerror(errno));

	s->should_munmap = 0;
	s->data = strbuf_detach(&buf, &size);
	s->size = size;
	s->should_free = 1;
	return 0;
}

static int diff_populate_gitlink(struct diff_filespec *s, int size_only)
{
	int len;
	char *data = xmalloc(100);
	len = snprintf(data, 100,
		"Subproject commit %s\n", sha1_to_hex(s->sha1));
	s->data = data;
	s->size = len;
	s->should_free = 1;
	if (size_only) {
		s->data = NULL;
		free(data);
	}
	return 0;
}

/*
 * While doing rename detection and pickaxe operation, we may need to
 * grab the data for the blob (or file) for our own in-core comparison.
 * diff_filespec has data and size fields for this purpose.
 */
int diff_populate_filespec(struct diff_filespec *s, int size_only)
{
	int err = 0;
	if (!DIFF_FILE_VALID(s))
		die("internal error: asking to populate invalid file.");
	if (S_ISDIR(s->mode))
		return -1;

	if (s->data)
		return 0;

	if (size_only && 0 < s->size)
		return 0;

	if (S_ISGITLINK(s->mode))
		return diff_populate_gitlink(s, size_only);

	if (!s->sha1_valid ||
	    reuse_worktree_file(s->path, s->sha1, 0)) {
		struct strbuf buf = STRBUF_INIT;
		struct stat st;
		int fd;

		if (!strcmp(s->path, "-"))
			return populate_from_stdin(s);

		if (lstat(s->path, &st) < 0) {
			if (errno == ENOENT) {
			err_empty:
				err = -1;
			empty:
				s->data = (char *)"";
				s->size = 0;
				return err;
			}
		}
		s->size = xsize_t(st.st_size);
		if (!s->size)
			goto empty;
		if (S_ISLNK(st.st_mode)) {
			struct strbuf sb = STRBUF_INIT;

			if (strbuf_readlink(&sb, s->path, s->size))
				goto err_empty;
			s->size = sb.len;
			s->data = strbuf_detach(&sb, NULL);
			s->should_free = 1;
			return 0;
		}
		if (size_only)
			return 0;
		fd = open(s->path, O_RDONLY);
		if (fd < 0)
			goto err_empty;
		s->data = xmmap(NULL, s->size, PROT_READ, MAP_PRIVATE, fd, 0);
		close(fd);
		s->should_munmap = 1;

		/*
		 * Convert from working tree format to canonical git format
		 */
		if (convert_to_git(s->path, s->data, s->size, &buf, safe_crlf)) {
			size_t size = 0;
			munmap(s->data, s->size);
			s->should_munmap = 0;
			s->data = strbuf_detach(&buf, &size);
			s->size = size;
			s->should_free = 1;
		}
	}
	else {
		enum object_type type;
		if (size_only)
			type = sha1_object_info(s->sha1, &s->size);
		else {
			s->data = read_sha1_file(s->sha1, &type, &s->size);
			s->should_free = 1;
		}
	}
	return 0;
}

void diff_free_filespec_blob(struct diff_filespec *s)
{
	if (s->should_free)
		free(s->data);
	else if (s->should_munmap)
		munmap(s->data, s->size);

	if (s->should_free || s->should_munmap) {
		s->should_free = s->should_munmap = 0;
		s->data = NULL;
	}
}

void diff_free_filespec_data(struct diff_filespec *s)
{
	diff_free_filespec_blob(s);
	free(s->cnt_data);
	s->cnt_data = NULL;
}

static void prep_temp_blob(struct diff_tempfile *temp,
			   void *blob,
			   unsigned long size,
			   const unsigned char *sha1,
			   int mode)
{
	int fd;

	fd = git_mkstemp(temp->tmp_path, PATH_MAX, ".diff_XXXXXX");
	if (fd < 0)
		die("unable to create temp-file: %s", strerror(errno));
	if (write_in_full(fd, blob, size) != size)
		die("unable to write temp-file");
	close(fd);
	temp->name = temp->tmp_path;
	strcpy(temp->hex, sha1_to_hex(sha1));
	temp->hex[40] = 0;
	sprintf(temp->mode, "%06o", mode);
}

static struct diff_tempfile *prepare_temp_file(const char *name,
		struct diff_filespec *one)
{
	struct diff_tempfile *temp = claim_diff_tempfile();

	if (!DIFF_FILE_VALID(one)) {
	not_a_valid_file:
		/* A '-' entry produces this for file-2, and
		 * a '+' entry produces this for file-1.
		 */
		temp->name = "/dev/null";
		strcpy(temp->hex, ".");
		strcpy(temp->mode, ".");
		return temp;
	}

	if (!remove_tempfile_installed) {
		atexit(remove_tempfile);
		sigchain_push_common(remove_tempfile_on_signal);
		remove_tempfile_installed = 1;
	}

	if (!one->sha1_valid ||
	    reuse_worktree_file(name, one->sha1, 1)) {
		struct stat st;
		if (lstat(name, &st) < 0) {
			if (errno == ENOENT)
				goto not_a_valid_file;
			die("stat(%s): %s", name, strerror(errno));
		}
		if (S_ISLNK(st.st_mode)) {
			int ret;
			char buf[PATH_MAX + 1]; /* ought to be SYMLINK_MAX */
			ret = readlink(name, buf, sizeof(buf));
			if (ret < 0)
				die("readlink(%s)", name);
			if (ret == sizeof(buf))
				die("symlink too long: %s", name);
			prep_temp_blob(temp, buf, ret,
				       (one->sha1_valid ?
					one->sha1 : null_sha1),
				       (one->sha1_valid ?
					one->mode : S_IFLNK));
		}
		else {
			/* we can borrow from the file in the work tree */
			temp->name = name;
			if (!one->sha1_valid)
				strcpy(temp->hex, sha1_to_hex(null_sha1));
			else
				strcpy(temp->hex, sha1_to_hex(one->sha1));
			/* Even though we may sometimes borrow the
			 * contents from the work tree, we always want
			 * one->mode.  mode is trustworthy even when
			 * !(one->sha1_valid), as long as
			 * DIFF_FILE_VALID(one).
			 */
			sprintf(temp->mode, "%06o", one->mode);
		}
		return temp;
	}
	else {
		if (diff_populate_filespec(one, 0))
			die("cannot read data blob for %s", one->path);
		prep_temp_blob(temp, one->data, one->size,
			       one->sha1, one->mode);
	}
	return temp;
}

/* An external diff command takes:
 *
 * diff-cmd name infile1 infile1-sha1 infile1-mode \
 *               infile2 infile2-sha1 infile2-mode [ rename-to ]
 *
 */
static void run_external_diff(const char *pgm,
			      const char *name,
			      const char *other,
			      struct diff_filespec *one,
			      struct diff_filespec *two,
			      const char *xfrm_msg,
			      int complete_rewrite)
{
	const char *spawn_arg[10];
	int retval;
	const char **arg = &spawn_arg[0];

	if (one && two) {
		struct diff_tempfile *temp_one, *temp_two;
		const char *othername = (other ? other : name);
		temp_one = prepare_temp_file(name, one);
		temp_two = prepare_temp_file(othername, two);
		*arg++ = pgm;
		*arg++ = name;
		*arg++ = temp_one->name;
		*arg++ = temp_one->hex;
		*arg++ = temp_one->mode;
		*arg++ = temp_two->name;
		*arg++ = temp_two->hex;
		*arg++ = temp_two->mode;
		if (other) {
			*arg++ = other;
			*arg++ = xfrm_msg;
		}
	} else {
		*arg++ = pgm;
		*arg++ = name;
	}
	*arg = NULL;
	fflush(NULL);
	retval = run_command_v_opt(spawn_arg, 0);
	remove_tempfile();
	if (retval) {
		fprintf(stderr, "external diff died, stopping at %s.\n", name);
		exit(1);
	}
}

static int similarity_index(struct diff_filepair *p)
{
	return p->score * 100 / MAX_SCORE;
}

static void fill_metainfo(struct strbuf *msg,
			  const char *name,
			  const char *other,
			  struct diff_filespec *one,
			  struct diff_filespec *two,
			  struct diff_options *o,
			  struct diff_filepair *p)
{
	strbuf_init(msg, PATH_MAX * 2 + 300);
	switch (p->status) {
	case DIFF_STATUS_COPIED:
		strbuf_addf(msg, "similarity index %d%%", similarity_index(p));
		strbuf_addstr(msg, "\ncopy from ");
		quote_c_style(name, msg, NULL, 0);
		strbuf_addstr(msg, "\ncopy to ");
		quote_c_style(other, msg, NULL, 0);
		strbuf_addch(msg, '\n');
		break;
	case DIFF_STATUS_RENAMED:
		strbuf_addf(msg, "similarity index %d%%", similarity_index(p));
		strbuf_addstr(msg, "\nrename from ");
		quote_c_style(name, msg, NULL, 0);
		strbuf_addstr(msg, "\nrename to ");
		quote_c_style(other, msg, NULL, 0);
		strbuf_addch(msg, '\n');
		break;
	case DIFF_STATUS_MODIFIED:
		if (p->score) {
			strbuf_addf(msg, "dissimilarity index %d%%\n",
				    similarity_index(p));
			break;
		}
		/* fallthru */
	default:
		/* nothing */
		;
	}
	if (one && two && hashcmp(one->sha1, two->sha1)) {
		int abbrev = DIFF_OPT_TST(o, FULL_INDEX) ? 40 : DEFAULT_ABBREV;

		if (DIFF_OPT_TST(o, BINARY)) {
			mmfile_t mf;
			if ((!fill_mmfile(&mf, one) && diff_filespec_is_binary(one)) ||
			    (!fill_mmfile(&mf, two) && diff_filespec_is_binary(two)))
				abbrev = 40;
		}
		strbuf_addf(msg, "index %.*s..%.*s",
			    abbrev, sha1_to_hex(one->sha1),
			    abbrev, sha1_to_hex(two->sha1));
		if (one->mode == two->mode)
			strbuf_addf(msg, " %06o", one->mode);
		strbuf_addch(msg, '\n');
	}
	if (msg->len)
		strbuf_setlen(msg, msg->len - 1);
}

static void run_diff_cmd(const char *pgm,
			 const char *name,
			 const char *other,
			 const char *attr_path,
			 struct diff_filespec *one,
			 struct diff_filespec *two,
			 struct strbuf *msg,
			 struct diff_options *o,
			 struct diff_filepair *p)
{
	const char *xfrm_msg = NULL;
	int complete_rewrite = (p->status == DIFF_STATUS_MODIFIED) && p->score;

	if (msg) {
		fill_metainfo(msg, name, other, one, two, o, p);
		xfrm_msg = msg->len ? msg->buf : NULL;
	}

	if (!DIFF_OPT_TST(o, ALLOW_EXTERNAL))
		pgm = NULL;
	else {
		struct userdiff_driver *drv = userdiff_find_by_path(attr_path);
		if (drv && drv->external)
			pgm = drv->external;
	}

	if (pgm) {
		run_external_diff(pgm, name, other, one, two, xfrm_msg,
				  complete_rewrite);
		return;
	}
	if (one && two)
		builtin_diff(name, other ? other : name,
			     one, two, xfrm_msg, o, complete_rewrite);
	else
		fprintf(o->file, "* Unmerged path %s\n", name);
}

static void diff_fill_sha1_info(struct diff_filespec *one)
{
	if (DIFF_FILE_VALID(one)) {
		if (!one->sha1_valid) {
			struct stat st;
			if (!strcmp(one->path, "-")) {
				hashcpy(one->sha1, null_sha1);
				return;
			}
			if (lstat(one->path, &st) < 0)
				die("stat %s", one->path);
			if (index_path(one->sha1, one->path, &st, 0))
				die("cannot hash %s", one->path);
		}
	}
	else
		hashclr(one->sha1);
}

static void strip_prefix(int prefix_length, const char **namep, const char **otherp)
{
	/* Strip the prefix but do not molest /dev/null and absolute paths */
	if (*namep && **namep != '/')
		*namep += prefix_length;
	if (*otherp && **otherp != '/')
		*otherp += prefix_length;
}

static void run_diff(struct diff_filepair *p, struct diff_options *o)
{
	const char *pgm = external_diff();
	struct strbuf msg;
	struct diff_filespec *one = p->one;
	struct diff_filespec *two = p->two;
	const char *name;
	const char *other;
	const char *attr_path;

	name  = p->one->path;
	other = (strcmp(name, p->two->path) ? p->two->path : NULL);
	attr_path = name;
	if (o->prefix_length)
		strip_prefix(o->prefix_length, &name, &other);

	if (DIFF_PAIR_UNMERGED(p)) {
		run_diff_cmd(pgm, name, NULL, attr_path,
			     NULL, NULL, NULL, o, p);
		return;
	}

	diff_fill_sha1_info(one);
	diff_fill_sha1_info(two);

	if (!pgm &&
	    DIFF_FILE_VALID(one) && DIFF_FILE_VALID(two) &&
	    (S_IFMT & one->mode) != (S_IFMT & two->mode)) {
		/*
		 * a filepair that changes between file and symlink
		 * needs to be split into deletion and creation.
		 */
		struct diff_filespec *null = alloc_filespec(two->path);
		run_diff_cmd(NULL, name, other, attr_path,
			     one, null, &msg, o, p);
		free(null);
		strbuf_release(&msg);

		null = alloc_filespec(one->path);
		run_diff_cmd(NULL, name, other, attr_path,
			     null, two, &msg, o, p);
		free(null);
	}
	else
		run_diff_cmd(pgm, name, other, attr_path,
			     one, two, &msg, o, p);

	strbuf_release(&msg);
}

static void run_diffstat(struct diff_filepair *p, struct diff_options *o,
			 struct diffstat_t *diffstat)
{
	const char *name;
	const char *other;
	int complete_rewrite = 0;

	if (DIFF_PAIR_UNMERGED(p)) {
		/* unmerged */
		builtin_diffstat(p->one->path, NULL, NULL, NULL, diffstat, o, 0);
		return;
	}

	name = p->one->path;
	other = (strcmp(name, p->two->path) ? p->two->path : NULL);

	if (o->prefix_length)
		strip_prefix(o->prefix_length, &name, &other);

	diff_fill_sha1_info(p->one);
	diff_fill_sha1_info(p->two);

	if (p->status == DIFF_STATUS_MODIFIED && p->score)
		complete_rewrite = 1;
	builtin_diffstat(name, other, p->one, p->two, diffstat, o, complete_rewrite);
}

static void run_checkdiff(struct diff_filepair *p, struct diff_options *o)
{
	const char *name;
	const char *other;
	const char *attr_path;

	if (DIFF_PAIR_UNMERGED(p)) {
		/* unmerged */
		return;
	}

	name = p->one->path;
	other = (strcmp(name, p->two->path) ? p->two->path : NULL);
	attr_path = other ? other : name;

	if (o->prefix_length)
		strip_prefix(o->prefix_length, &name, &other);

	diff_fill_sha1_info(p->one);
	diff_fill_sha1_info(p->two);

	builtin_checkdiff(name, other, attr_path, p->one, p->two, o);
}

void diff_setup(struct diff_options *options)
{
	memset(options, 0, sizeof(*options));

	options->file = stdout;

	options->line_termination = '\n';
	options->break_opt = -1;
	options->rename_limit = -1;
	options->dirstat_percent = 3;
	DIFF_OPT_CLR(options, DIRSTAT_CUMULATIVE);
	options->context = 3;

	options->change = diff_change;
	options->add_remove = diff_addremove;
	if (diff_use_color_default > 0)
		DIFF_OPT_SET(options, COLOR_DIFF);
	else
		DIFF_OPT_CLR(options, COLOR_DIFF);
	options->detect_rename = diff_detect_rename_default;

	if (!diff_mnemonic_prefix) {
		options->a_prefix = "a/";
		options->b_prefix = "b/";
	}
}

int diff_setup_done(struct diff_options *options)
{
	int count = 0;

	if (options->output_format & DIFF_FORMAT_NAME)
		count++;
	if (options->output_format & DIFF_FORMAT_NAME_STATUS)
		count++;
	if (options->output_format & DIFF_FORMAT_CHECKDIFF)
		count++;
	if (options->output_format & DIFF_FORMAT_NO_OUTPUT)
		count++;
	if (count > 1)
		die("--name-only, --name-status, --check and -s are mutually exclusive");

	if (DIFF_OPT_TST(options, FIND_COPIES_HARDER))
		options->detect_rename = DIFF_DETECT_COPY;

	if (!DIFF_OPT_TST(options, RELATIVE_NAME))
		options->prefix = NULL;
	if (options->prefix)
		options->prefix_length = strlen(options->prefix);
	else
		options->prefix_length = 0;

	if (options->output_format & (DIFF_FORMAT_NAME |
				      DIFF_FORMAT_NAME_STATUS |
				      DIFF_FORMAT_CHECKDIFF |
				      DIFF_FORMAT_NO_OUTPUT))
		options->output_format &= ~(DIFF_FORMAT_RAW |
					    DIFF_FORMAT_NUMSTAT |
					    DIFF_FORMAT_DIFFSTAT |
					    DIFF_FORMAT_SHORTSTAT |
					    DIFF_FORMAT_DIRSTAT |
					    DIFF_FORMAT_SUMMARY |
					    DIFF_FORMAT_PATCH);

	/*
	 * These cases always need recursive; we do not drop caller-supplied
	 * recursive bits for other formats here.
	 */
	if (options->output_format & (DIFF_FORMAT_PATCH |
				      DIFF_FORMAT_NUMSTAT |
				      DIFF_FORMAT_DIFFSTAT |
				      DIFF_FORMAT_SHORTSTAT |
				      DIFF_FORMAT_DIRSTAT |
				      DIFF_FORMAT_SUMMARY |
				      DIFF_FORMAT_CHECKDIFF))
		DIFF_OPT_SET(options, RECURSIVE);
	/*
	 * Also pickaxe would not work very well if you do not say recursive
	 */
	if (options->pickaxe)
		DIFF_OPT_SET(options, RECURSIVE);

	if (options->detect_rename && options->rename_limit < 0)
		options->rename_limit = diff_rename_limit_default;
	if (options->setup & DIFF_SETUP_USE_CACHE) {
		if (!active_cache)
			/* read-cache does not die even when it fails
			 * so it is safe for us to do this here.  Also
			 * it does not smudge active_cache or active_nr
			 * when it fails, so we do not have to worry about
			 * cleaning it up ourselves either.
			 */
			read_cache();
	}
	if (options->abbrev <= 0 || 40 < options->abbrev)
		options->abbrev = 40; /* full */

	/*
	 * It does not make sense to show the first hit we happened
	 * to have found.  It does not make sense not to return with
	 * exit code in such a case either.
	 */
	if (DIFF_OPT_TST(options, QUIET)) {
		options->output_format = DIFF_FORMAT_NO_OUTPUT;
		DIFF_OPT_SET(options, EXIT_WITH_STATUS);
	}

	return 0;
}

static int opt_arg(const char *arg, int arg_short, const char *arg_long, int *val)
{
	char c, *eq;
	int len;

	if (*arg != '-')
		return 0;
	c = *++arg;
	if (!c)
		return 0;
	if (c == arg_short) {
		c = *++arg;
		if (!c)
			return 1;
		if (val && isdigit(c)) {
			char *end;
			int n = strtoul(arg, &end, 10);
			if (*end)
				return 0;
			*val = n;
			return 1;
		}
		return 0;
	}
	if (c != '-')
		return 0;
	arg++;
	eq = strchr(arg, '=');
	if (eq)
		len = eq - arg;
	else
		len = strlen(arg);
	if (!len || strncmp(arg, arg_long, len))
		return 0;
	if (eq) {
		int n;
		char *end;
		if (!isdigit(*++eq))
			return 0;
		n = strtoul(eq, &end, 10);
		if (*end)
			return 0;
		*val = n;
	}
	return 1;
}

static int diff_scoreopt_parse(const char *opt);

int diff_opt_parse(struct diff_options *options, const char **av, int ac)
{
	const char *arg = av[0];

	/* Output format options */
	if (!strcmp(arg, "-p") || !strcmp(arg, "-u"))
		options->output_format |= DIFF_FORMAT_PATCH;
	else if (opt_arg(arg, 'U', "unified", &options->context))
		options->output_format |= DIFF_FORMAT_PATCH;
	else if (!strcmp(arg, "--raw"))
		options->output_format |= DIFF_FORMAT_RAW;
	else if (!strcmp(arg, "--patch-with-raw"))
		options->output_format |= DIFF_FORMAT_PATCH | DIFF_FORMAT_RAW;
	else if (!strcmp(arg, "--numstat"))
		options->output_format |= DIFF_FORMAT_NUMSTAT;
	else if (!strcmp(arg, "--shortstat"))
		options->output_format |= DIFF_FORMAT_SHORTSTAT;
	else if (opt_arg(arg, 'X', "dirstat", &options->dirstat_percent))
		options->output_format |= DIFF_FORMAT_DIRSTAT;
	else if (!strcmp(arg, "--cumulative")) {
		options->output_format |= DIFF_FORMAT_DIRSTAT;
		DIFF_OPT_SET(options, DIRSTAT_CUMULATIVE);
	} else if (opt_arg(arg, 0, "dirstat-by-file",
			   &options->dirstat_percent)) {
		options->output_format |= DIFF_FORMAT_DIRSTAT;
		DIFF_OPT_SET(options, DIRSTAT_BY_FILE);
	}
	else if (!strcmp(arg, "--check"))
		options->output_format |= DIFF_FORMAT_CHECKDIFF;
	else if (!strcmp(arg, "--summary"))
		options->output_format |= DIFF_FORMAT_SUMMARY;
	else if (!strcmp(arg, "--patch-with-stat"))
		options->output_format |= DIFF_FORMAT_PATCH | DIFF_FORMAT_DIFFSTAT;
	else if (!strcmp(arg, "--name-only"))
		options->output_format |= DIFF_FORMAT_NAME;
	else if (!strcmp(arg, "--name-status"))
		options->output_format |= DIFF_FORMAT_NAME_STATUS;
	else if (!strcmp(arg, "-s"))
		options->output_format |= DIFF_FORMAT_NO_OUTPUT;
	else if (!prefixcmp(arg, "--stat")) {
		char *end;
		int width = options->stat_width;
		int name_width = options->stat_name_width;
		arg += 6;
		end = (char *)arg;

		switch (*arg) {
		case '-':
			if (!prefixcmp(arg, "-width="))
				width = strtoul(arg + 7, &end, 10);
			else if (!prefixcmp(arg, "-name-width="))
				name_width = strtoul(arg + 12, &end, 10);
			break;
		case '=':
			width = strtoul(arg+1, &end, 10);
			if (*end == ',')
				name_width = strtoul(end+1, &end, 10);
		}

		/* Important! This checks all the error cases! */
		if (*end)
			return 0;
		options->output_format |= DIFF_FORMAT_DIFFSTAT;
		options->stat_name_width = name_width;
		options->stat_width = width;
	}

	/* renames options */
	else if (!prefixcmp(arg, "-B")) {
		if ((options->break_opt = diff_scoreopt_parse(arg)) == -1)
			return -1;
	}
	else if (!prefixcmp(arg, "-M")) {
		if ((options->rename_score = diff_scoreopt_parse(arg)) == -1)
			return -1;
		options->detect_rename = DIFF_DETECT_RENAME;
	}
	else if (!prefixcmp(arg, "-C")) {
		if (options->detect_rename == DIFF_DETECT_COPY)
			DIFF_OPT_SET(options, FIND_COPIES_HARDER);
		if ((options->rename_score = diff_scoreopt_parse(arg)) == -1)
			return -1;
		options->detect_rename = DIFF_DETECT_COPY;
	}
	else if (!strcmp(arg, "--no-renames"))
		options->detect_rename = 0;
	else if (!strcmp(arg, "--relative"))
		DIFF_OPT_SET(options, RELATIVE_NAME);
	else if (!prefixcmp(arg, "--relative=")) {
		DIFF_OPT_SET(options, RELATIVE_NAME);
		options->prefix = arg + 11;
	}

	/* xdiff options */
	else if (!strcmp(arg, "-w") || !strcmp(arg, "--ignore-all-space"))
		options->xdl_opts |= XDF_IGNORE_WHITESPACE;
	else if (!strcmp(arg, "-b") || !strcmp(arg, "--ignore-space-change"))
		options->xdl_opts |= XDF_IGNORE_WHITESPACE_CHANGE;
	else if (!strcmp(arg, "--ignore-space-at-eol"))
		options->xdl_opts |= XDF_IGNORE_WHITESPACE_AT_EOL;
	else if (!strcmp(arg, "--patience"))
		options->xdl_opts |= XDF_PATIENCE_DIFF;

	/* flags options */
	else if (!strcmp(arg, "--binary")) {
		options->output_format |= DIFF_FORMAT_PATCH;
		DIFF_OPT_SET(options, BINARY);
	}
	else if (!strcmp(arg, "--full-index"))
		DIFF_OPT_SET(options, FULL_INDEX);
	else if (!strcmp(arg, "-a") || !strcmp(arg, "--text"))
		DIFF_OPT_SET(options, TEXT);
	else if (!strcmp(arg, "-R"))
		DIFF_OPT_SET(options, REVERSE_DIFF);
	else if (!strcmp(arg, "--find-copies-harder"))
		DIFF_OPT_SET(options, FIND_COPIES_HARDER);
	else if (!strcmp(arg, "--follow"))
		DIFF_OPT_SET(options, FOLLOW_RENAMES);
	else if (!strcmp(arg, "--color"))
		DIFF_OPT_SET(options, COLOR_DIFF);
	else if (!strcmp(arg, "--no-color"))
		DIFF_OPT_CLR(options, COLOR_DIFF);
	else if (!strcmp(arg, "--color-words"))
		options->flags |= DIFF_OPT_COLOR_DIFF | DIFF_OPT_COLOR_DIFF_WORDS;
	else if (!prefixcmp(arg, "--color-words=")) {
		options->flags |= DIFF_OPT_COLOR_DIFF | DIFF_OPT_COLOR_DIFF_WORDS;
		options->word_regex = arg + 14;
	}
	else if (!strcmp(arg, "--exit-code"))
		DIFF_OPT_SET(options, EXIT_WITH_STATUS);
	else if (!strcmp(arg, "--quiet"))
		DIFF_OPT_SET(options, QUIET);
	else if (!strcmp(arg, "--ext-diff"))
		DIFF_OPT_SET(options, ALLOW_EXTERNAL);
	else if (!strcmp(arg, "--no-ext-diff"))
		DIFF_OPT_CLR(options, ALLOW_EXTERNAL);
	else if (!strcmp(arg, "--textconv"))
		DIFF_OPT_SET(options, ALLOW_TEXTCONV);
	else if (!strcmp(arg, "--no-textconv"))
		DIFF_OPT_CLR(options, ALLOW_TEXTCONV);
	else if (!strcmp(arg, "--ignore-submodules"))
		DIFF_OPT_SET(options, IGNORE_SUBMODULES);

	/* misc options */
	else if (!strcmp(arg, "-z"))
		options->line_termination = 0;
	else if (!prefixcmp(arg, "-l"))
		options->rename_limit = strtoul(arg+2, NULL, 10);
	else if (!prefixcmp(arg, "-S"))
		options->pickaxe = arg + 2;
	else if (!strcmp(arg, "--pickaxe-all"))
		options->pickaxe_opts = DIFF_PICKAXE_ALL;
	else if (!strcmp(arg, "--pickaxe-regex"))
		options->pickaxe_opts = DIFF_PICKAXE_REGEX;
	else if (!prefixcmp(arg, "-O"))
		options->orderfile = arg + 2;
	else if (!prefixcmp(arg, "--diff-filter="))
		options->filter = arg + 14;
	else if (!strcmp(arg, "--abbrev"))
		options->abbrev = DEFAULT_ABBREV;
	else if (!prefixcmp(arg, "--abbrev=")) {
		options->abbrev = strtoul(arg + 9, NULL, 10);
		if (options->abbrev < MINIMUM_ABBREV)
			options->abbrev = MINIMUM_ABBREV;
		else if (40 < options->abbrev)
			options->abbrev = 40;
	}
	else if (!prefixcmp(arg, "--src-prefix="))
		options->a_prefix = arg + 13;
	else if (!prefixcmp(arg, "--dst-prefix="))
		options->b_prefix = arg + 13;
	else if (!strcmp(arg, "--no-prefix"))
		options->a_prefix = options->b_prefix = "";
	else if (opt_arg(arg, '\0', "inter-hunk-context",
			 &options->interhunkcontext))
		;
	else if (!prefixcmp(arg, "--output=")) {
		options->file = fopen(arg + strlen("--output="), "w");
		options->close_file = 1;
	} else
		return 0;
	return 1;
}

static int parse_num(const char **cp_p)
{
	unsigned long num, scale;
	int ch, dot;
	const char *cp = *cp_p;

	num = 0;
	scale = 1;
	dot = 0;
	for(;;) {
		ch = *cp;
		if ( !dot && ch == '.' ) {
			scale = 1;
			dot = 1;
		} else if ( ch == '%' ) {
			scale = dot ? scale*100 : 100;
			cp++;	/* % is always at the end */
			break;
		} else if ( ch >= '0' && ch <= '9' ) {
			if ( scale < 100000 ) {
				scale *= 10;
				num = (num*10) + (ch-'0');
			}
		} else {
			break;
		}
		cp++;
	}
	*cp_p = cp;

	/* user says num divided by scale and we say internally that
	 * is MAX_SCORE * num / scale.
	 */
	return (int)((num >= scale) ? MAX_SCORE : (MAX_SCORE * num / scale));
}

static int diff_scoreopt_parse(const char *opt)
{
	int opt1, opt2, cmd;

	if (*opt++ != '-')
		return -1;
	cmd = *opt++;
	if (cmd != 'M' && cmd != 'C' && cmd != 'B')
		return -1; /* that is not a -M, -C nor -B option */

	opt1 = parse_num(&opt);
	if (cmd != 'B')
		opt2 = 0;
	else {
		if (*opt == 0)
			opt2 = 0;
		else if (*opt != '/')
			return -1; /* we expect -B80/99 or -B80 */
		else {
			opt++;
			opt2 = parse_num(&opt);
		}
	}
	if (*opt != 0)
		return -1;
	return opt1 | (opt2 << 16);
}

struct diff_queue_struct diff_queued_diff;

void diff_q(struct diff_queue_struct *queue, struct diff_filepair *dp)
{
	if (queue->alloc <= queue->nr) {
		queue->alloc = alloc_nr(queue->alloc);
		queue->queue = xrealloc(queue->queue,
					sizeof(dp) * queue->alloc);
	}
	queue->queue[queue->nr++] = dp;
}

struct diff_filepair *diff_queue(struct diff_queue_struct *queue,
				 struct diff_filespec *one,
				 struct diff_filespec *two)
{
	struct diff_filepair *dp = xcalloc(1, sizeof(*dp));
	dp->one = one;
	dp->two = two;
	if (queue)
		diff_q(queue, dp);
	return dp;
}

void diff_free_filepair(struct diff_filepair *p)
{
	free_filespec(p->one);
	free_filespec(p->two);
	free(p);
}

/* This is different from find_unique_abbrev() in that
 * it stuffs the result with dots for alignment.
 */
const char *diff_unique_abbrev(const unsigned char *sha1, int len)
{
	int abblen;
	const char *abbrev;
	if (len == 40)
		return sha1_to_hex(sha1);

	abbrev = find_unique_abbrev(sha1, len);
	abblen = strlen(abbrev);
	if (abblen < 37) {
		static char hex[41];
		if (len < abblen && abblen <= len + 2)
			sprintf(hex, "%s%.*s", abbrev, len+3-abblen, "..");
		else
			sprintf(hex, "%s...", abbrev);
		return hex;
	}
	return sha1_to_hex(sha1);
}

static void diff_flush_raw(struct diff_filepair *p, struct diff_options *opt)
{
	int line_termination = opt->line_termination;
	int inter_name_termination = line_termination ? '\t' : '\0';

	if (!(opt->output_format & DIFF_FORMAT_NAME_STATUS)) {
		fprintf(opt->file, ":%06o %06o %s ", p->one->mode, p->two->mode,
			diff_unique_abbrev(p->one->sha1, opt->abbrev));
		fprintf(opt->file, "%s ", diff_unique_abbrev(p->two->sha1, opt->abbrev));
	}
	if (p->score) {
		fprintf(opt->file, "%c%03d%c", p->status, similarity_index(p),
			inter_name_termination);
	} else {
		fprintf(opt->file, "%c%c", p->status, inter_name_termination);
	}

	if (p->status == DIFF_STATUS_COPIED ||
	    p->status == DIFF_STATUS_RENAMED) {
		const char *name_a, *name_b;
		name_a = p->one->path;
		name_b = p->two->path;
		strip_prefix(opt->prefix_length, &name_a, &name_b);
		write_name_quoted(name_a, opt->file, inter_name_termination);
		write_name_quoted(name_b, opt->file, line_termination);
	} else {
		const char *name_a, *name_b;
		name_a = p->one->mode ? p->one->path : p->two->path;
		name_b = NULL;
		strip_prefix(opt->prefix_length, &name_a, &name_b);
		write_name_quoted(name_a, opt->file, line_termination);
	}
}

int diff_unmodified_pair(struct diff_filepair *p)
{
	/* This function is written stricter than necessary to support
	 * the currently implemented transformers, but the idea is to
	 * let transformers to produce diff_filepairs any way they want,
	 * and filter and clean them up here before producing the output.
	 */
	struct diff_filespec *one = p->one, *two = p->two;

	if (DIFF_PAIR_UNMERGED(p))
		return 0; /* unmerged is interesting */

	/* deletion, addition, mode or type change
	 * and rename are all interesting.
	 */
	if (DIFF_FILE_VALID(one) != DIFF_FILE_VALID(two) ||
	    DIFF_PAIR_MODE_CHANGED(p) ||
	    strcmp(one->path, two->path))
		return 0;

	/* both are valid and point at the same path.  that is, we are
	 * dealing with a change.
	 */
	if (one->sha1_valid && two->sha1_valid &&
	    !hashcmp(one->sha1, two->sha1))
		return 1; /* no change */
	if (!one->sha1_valid && !two->sha1_valid)
		return 1; /* both look at the same file on the filesystem. */
	return 0;
}

static void diff_flush_patch(struct diff_filepair *p, struct diff_options *o)
{
	if (diff_unmodified_pair(p))
		return;

	if ((DIFF_FILE_VALID(p->one) && S_ISDIR(p->one->mode)) ||
	    (DIFF_FILE_VALID(p->two) && S_ISDIR(p->two->mode)))
		return; /* no tree diffs in patch format */

	run_diff(p, o);
}

static void diff_flush_stat(struct diff_filepair *p, struct diff_options *o,
			    struct diffstat_t *diffstat)
{
	if (diff_unmodified_pair(p))
		return;

	if ((DIFF_FILE_VALID(p->one) && S_ISDIR(p->one->mode)) ||
	    (DIFF_FILE_VALID(p->two) && S_ISDIR(p->two->mode)))
		return; /* no tree diffs in patch format */

	run_diffstat(p, o, diffstat);
}

static void diff_flush_checkdiff(struct diff_filepair *p,
		struct diff_options *o)
{
	if (diff_unmodified_pair(p))
		return;

	if ((DIFF_FILE_VALID(p->one) && S_ISDIR(p->one->mode)) ||
	    (DIFF_FILE_VALID(p->two) && S_ISDIR(p->two->mode)))
		return; /* no tree diffs in patch format */

	run_checkdiff(p, o);
}

int diff_queue_is_empty(void)
{
	struct diff_queue_struct *q = &diff_queued_diff;
	int i;
	for (i = 0; i < q->nr; i++)
		if (!diff_unmodified_pair(q->queue[i]))
			return 0;
	return 1;
}

#if DIFF_DEBUG
void diff_debug_filespec(struct diff_filespec *s, int x, const char *one)
{
	fprintf(stderr, "queue[%d] %s (%s) %s %06o %s\n",
		x, one ? one : "",
		s->path,
		DIFF_FILE_VALID(s) ? "valid" : "invalid",
		s->mode,
		s->sha1_valid ? sha1_to_hex(s->sha1) : "");
	fprintf(stderr, "queue[%d] %s size %lu flags %d\n",
		x, one ? one : "",
		s->size, s->xfrm_flags);
}

void diff_debug_filepair(const struct diff_filepair *p, int i)
{
	diff_debug_filespec(p->one, i, "one");
	diff_debug_filespec(p->two, i, "two");
	fprintf(stderr, "score %d, status %c rename_used %d broken %d\n",
		p->score, p->status ? p->status : '?',
		p->one->rename_used, p->broken_pair);
}

void diff_debug_queue(const char *msg, struct diff_queue_struct *q)
{
	int i;
	if (msg)
		fprintf(stderr, "%s\n", msg);
	fprintf(stderr, "q->nr = %d\n", q->nr);
	for (i = 0; i < q->nr; i++) {
		struct diff_filepair *p = q->queue[i];
		diff_debug_filepair(p, i);
	}
}
#endif

static void diff_resolve_rename_copy(void)
{
	int i;
	struct diff_filepair *p;
	struct diff_queue_struct *q = &diff_queued_diff;

	diff_debug_queue("resolve-rename-copy", q);

	for (i = 0; i < q->nr; i++) {
		p = q->queue[i];
		p->status = 0; /* undecided */
		if (DIFF_PAIR_UNMERGED(p))
			p->status = DIFF_STATUS_UNMERGED;
		else if (!DIFF_FILE_VALID(p->one))
			p->status = DIFF_STATUS_ADDED;
		else if (!DIFF_FILE_VALID(p->two))
			p->status = DIFF_STATUS_DELETED;
		else if (DIFF_PAIR_TYPE_CHANGED(p))
			p->status = DIFF_STATUS_TYPE_CHANGED;

		/* from this point on, we are dealing with a pair
		 * whose both sides are valid and of the same type, i.e.
		 * either in-place edit or rename/copy edit.
		 */
		else if (DIFF_PAIR_RENAME(p)) {
			/*
			 * A rename might have re-connected a broken
			 * pair up, causing the pathnames to be the
			 * same again. If so, that's not a rename at
			 * all, just a modification..
			 *
			 * Otherwise, see if this source was used for
			 * multiple renames, in which case we decrement
			 * the count, and call it a copy.
			 */
			if (!strcmp(p->one->path, p->two->path))
				p->status = DIFF_STATUS_MODIFIED;
			else if (--p->one->rename_used > 0)
				p->status = DIFF_STATUS_COPIED;
			else
				p->status = DIFF_STATUS_RENAMED;
		}
		else if (hashcmp(p->one->sha1, p->two->sha1) ||
			 p->one->mode != p->two->mode ||
			 is_null_sha1(p->one->sha1))
			p->status = DIFF_STATUS_MODIFIED;
		else {
			/* This is a "no-change" entry and should not
			 * happen anymore, but prepare for broken callers.
			 */
			error("feeding unmodified %s to diffcore",
			      p->one->path);
			p->status = DIFF_STATUS_UNKNOWN;
		}
	}
	diff_debug_queue("resolve-rename-copy done", q);
}

static int check_pair_status(struct diff_filepair *p)
{
	switch (p->status) {
	case DIFF_STATUS_UNKNOWN:
		return 0;
	case 0:
		die("internal error in diff-resolve-rename-copy");
	default:
		return 1;
	}
}

static void flush_one_pair(struct diff_filepair *p, struct diff_options *opt)
{
	int fmt = opt->output_format;

	if (fmt & DIFF_FORMAT_CHECKDIFF)
		diff_flush_checkdiff(p, opt);
	else if (fmt & (DIFF_FORMAT_RAW | DIFF_FORMAT_NAME_STATUS))
		diff_flush_raw(p, opt);
	else if (fmt & DIFF_FORMAT_NAME) {
		const char *name_a, *name_b;
		name_a = p->two->path;
		name_b = NULL;
		strip_prefix(opt->prefix_length, &name_a, &name_b);
		write_name_quoted(name_a, opt->file, opt->line_termination);
	}
}

static void show_file_mode_name(FILE *file, const char *newdelete, struct diff_filespec *fs)
{
	if (fs->mode)
		fprintf(file, " %s mode %06o ", newdelete, fs->mode);
	else
		fprintf(file, " %s ", newdelete);
	write_name_quoted(fs->path, file, '\n');
}


static void show_mode_change(FILE *file, struct diff_filepair *p, int show_name)
{
	if (p->one->mode && p->two->mode && p->one->mode != p->two->mode) {
		fprintf(file, " mode change %06o => %06o%c", p->one->mode, p->two->mode,
			show_name ? ' ' : '\n');
		if (show_name) {
			write_name_quoted(p->two->path, file, '\n');
		}
	}
}

static void show_rename_copy(FILE *file, const char *renamecopy, struct diff_filepair *p)
{
	char *names = pprint_rename(p->one->path, p->two->path);

	fprintf(file, " %s %s (%d%%)\n", renamecopy, names, similarity_index(p));
	free(names);
	show_mode_change(file, p, 0);
}

static void diff_summary(FILE *file, struct diff_filepair *p)
{
	switch(p->status) {
	case DIFF_STATUS_DELETED:
		show_file_mode_name(file, "delete", p->one);
		break;
	case DIFF_STATUS_ADDED:
		show_file_mode_name(file, "create", p->two);
		break;
	case DIFF_STATUS_COPIED:
		show_rename_copy(file, "copy", p);
		break;
	case DIFF_STATUS_RENAMED:
		show_rename_copy(file, "rename", p);
		break;
	default:
		if (p->score) {
			fputs(" rewrite ", file);
			write_name_quoted(p->two->path, file, ' ');
			fprintf(file, "(%d%%)\n", similarity_index(p));
		}
		show_mode_change(file, p, !p->score);
		break;
	}
}

struct patch_id_t {
	git_SHA_CTX *ctx;
	int patchlen;
};

static int remove_space(char *line, int len)
{
	int i;
	char *dst = line;
	unsigned char c;

	for (i = 0; i < len; i++)
		if (!isspace((c = line[i])))
			*dst++ = c;

	return dst - line;
}

static void patch_id_consume(void *priv, char *line, unsigned long len)
{
	struct patch_id_t *data = priv;
	int new_len;

	/* Ignore line numbers when computing the SHA1 of the patch */
	if (!prefixcmp(line, "@@ -"))
		return;

	new_len = remove_space(line, len);

	git_SHA1_Update(data->ctx, line, new_len);
	data->patchlen += new_len;
}

/* returns 0 upon success, and writes result into sha1 */
static int diff_get_patch_id(struct diff_options *options, unsigned char *sha1)
{
	struct diff_queue_struct *q = &diff_queued_diff;
	int i;
	git_SHA_CTX ctx;
	struct patch_id_t data;
	char buffer[PATH_MAX * 4 + 20];

	git_SHA1_Init(&ctx);
	memset(&data, 0, sizeof(struct patch_id_t));
	data.ctx = &ctx;

	for (i = 0; i < q->nr; i++) {
		xpparam_t xpp;
		xdemitconf_t xecfg;
		xdemitcb_t ecb;
		mmfile_t mf1, mf2;
		struct diff_filepair *p = q->queue[i];
		int len1, len2;

		memset(&xpp, 0, sizeof(xpp));
		memset(&xecfg, 0, sizeof(xecfg));
		if (p->status == 0)
			return error("internal diff status error");
		if (p->status == DIFF_STATUS_UNKNOWN)
			continue;
		if (diff_unmodified_pair(p))
			continue;
		if ((DIFF_FILE_VALID(p->one) && S_ISDIR(p->one->mode)) ||
		    (DIFF_FILE_VALID(p->two) && S_ISDIR(p->two->mode)))
			continue;
		if (DIFF_PAIR_UNMERGED(p))
			continue;

		diff_fill_sha1_info(p->one);
		diff_fill_sha1_info(p->two);
		if (fill_mmfile(&mf1, p->one) < 0 ||
				fill_mmfile(&mf2, p->two) < 0)
			return error("unable to read files to diff");

		len1 = remove_space(p->one->path, strlen(p->one->path));
		len2 = remove_space(p->two->path, strlen(p->two->path));
		if (p->one->mode == 0)
			len1 = snprintf(buffer, sizeof(buffer),
					"diff--gita/%.*sb/%.*s"
					"newfilemode%06o"
					"---/dev/null"
					"+++b/%.*s",
					len1, p->one->path,
					len2, p->two->path,
					p->two->mode,
					len2, p->two->path);
		else if (p->two->mode == 0)
			len1 = snprintf(buffer, sizeof(buffer),
					"diff--gita/%.*sb/%.*s"
					"deletedfilemode%06o"
					"---a/%.*s"
					"+++/dev/null",
					len1, p->one->path,
					len2, p->two->path,
					p->one->mode,
					len1, p->one->path);
		else
			len1 = snprintf(buffer, sizeof(buffer),
					"diff--gita/%.*sb/%.*s"
					"---a/%.*s"
					"+++b/%.*s",
					len1, p->one->path,
					len2, p->two->path,
					len1, p->one->path,
					len2, p->two->path);
		git_SHA1_Update(&ctx, buffer, len1);

		xpp.flags = XDF_NEED_MINIMAL;
		xecfg.ctxlen = 3;
		xecfg.flags = XDL_EMIT_FUNCNAMES;
		xdi_diff_outf(&mf1, &mf2, patch_id_consume, &data,
			      &xpp, &xecfg, &ecb);
	}

	git_SHA1_Final(sha1, &ctx);
	return 0;
}

int diff_flush_patch_id(struct diff_options *options, unsigned char *sha1)
{
	struct diff_queue_struct *q = &diff_queued_diff;
	int i;
	int result = diff_get_patch_id(options, sha1);

	for (i = 0; i < q->nr; i++)
		diff_free_filepair(q->queue[i]);

	free(q->queue);
	q->queue = NULL;
	q->nr = q->alloc = 0;

	return result;
}

static int is_summary_empty(const struct diff_queue_struct *q)
{
	int i;

	for (i = 0; i < q->nr; i++) {
		const struct diff_filepair *p = q->queue[i];

		switch (p->status) {
		case DIFF_STATUS_DELETED:
		case DIFF_STATUS_ADDED:
		case DIFF_STATUS_COPIED:
		case DIFF_STATUS_RENAMED:
			return 0;
		default:
			if (p->score)
				return 0;
			if (p->one->mode && p->two->mode &&
			    p->one->mode != p->two->mode)
				return 0;
			break;
		}
	}
	return 1;
}

void diff_flush(struct diff_options *options)
{
	struct diff_queue_struct *q = &diff_queued_diff;
	int i, output_format = options->output_format;
	int separator = 0;

	/*
	 * Order: raw, stat, summary, patch
	 * or:    name/name-status/checkdiff (other bits clear)
	 */
	if (!q->nr)
		goto free_queue;

	if (output_format & (DIFF_FORMAT_RAW |
			     DIFF_FORMAT_NAME |
			     DIFF_FORMAT_NAME_STATUS |
			     DIFF_FORMAT_CHECKDIFF)) {
		for (i = 0; i < q->nr; i++) {
			struct diff_filepair *p = q->queue[i];
			if (check_pair_status(p))
				flush_one_pair(p, options);
		}
		separator++;
	}

	if (output_format & (DIFF_FORMAT_DIFFSTAT|DIFF_FORMAT_SHORTSTAT|DIFF_FORMAT_NUMSTAT)) {
		struct diffstat_t diffstat;

		memset(&diffstat, 0, sizeof(struct diffstat_t));
		for (i = 0; i < q->nr; i++) {
			struct diff_filepair *p = q->queue[i];
			if (check_pair_status(p))
				diff_flush_stat(p, options, &diffstat);
		}
		if (output_format & DIFF_FORMAT_NUMSTAT)
			show_numstat(&diffstat, options);
		if (output_format & DIFF_FORMAT_DIFFSTAT)
			show_stats(&diffstat, options);
		if (output_format & DIFF_FORMAT_SHORTSTAT)
			show_shortstats(&diffstat, options);
		free_diffstat_info(&diffstat);
		separator++;
	}
	if (output_format & DIFF_FORMAT_DIRSTAT)
		show_dirstat(options);

	if (output_format & DIFF_FORMAT_SUMMARY && !is_summary_empty(q)) {
		for (i = 0; i < q->nr; i++)
			diff_summary(options->file, q->queue[i]);
		separator++;
	}

	if (output_format & DIFF_FORMAT_PATCH) {
		if (separator) {
			putc(options->line_termination, options->file);
			if (options->stat_sep) {
				/* attach patch instead of inline */
				fputs(options->stat_sep, options->file);
			}
		}

		for (i = 0; i < q->nr; i++) {
			struct diff_filepair *p = q->queue[i];
			if (check_pair_status(p))
				diff_flush_patch(p, options);
		}
	}

	if (output_format & DIFF_FORMAT_CALLBACK)
		options->format_callback(q, options, options->format_callback_data);

	for (i = 0; i < q->nr; i++)
		diff_free_filepair(q->queue[i]);
free_queue:
	free(q->queue);
	q->queue = NULL;
	q->nr = q->alloc = 0;
	if (options->close_file)
		fclose(options->file);
}

static void diffcore_apply_filter(const char *filter)
{
	int i;
	struct diff_queue_struct *q = &diff_queued_diff;
	struct diff_queue_struct outq;
	outq.queue = NULL;
	outq.nr = outq.alloc = 0;

	if (!filter)
		return;

	if (strchr(filter, DIFF_STATUS_FILTER_AON)) {
		int found;
		for (i = found = 0; !found && i < q->nr; i++) {
			struct diff_filepair *p = q->queue[i];
			if (((p->status == DIFF_STATUS_MODIFIED) &&
			     ((p->score &&
			       strchr(filter, DIFF_STATUS_FILTER_BROKEN)) ||
			      (!p->score &&
			       strchr(filter, DIFF_STATUS_MODIFIED)))) ||
			    ((p->status != DIFF_STATUS_MODIFIED) &&
			     strchr(filter, p->status)))
				found++;
		}
		if (found)
			return;

		/* otherwise we will clear the whole queue
		 * by copying the empty outq at the end of this
		 * function, but first clear the current entries
		 * in the queue.
		 */
		for (i = 0; i < q->nr; i++)
			diff_free_filepair(q->queue[i]);
	}
	else {
		/* Only the matching ones */
		for (i = 0; i < q->nr; i++) {
			struct diff_filepair *p = q->queue[i];

			if (((p->status == DIFF_STATUS_MODIFIED) &&
			     ((p->score &&
			       strchr(filter, DIFF_STATUS_FILTER_BROKEN)) ||
			      (!p->score &&
			       strchr(filter, DIFF_STATUS_MODIFIED)))) ||
			    ((p->status != DIFF_STATUS_MODIFIED) &&
			     strchr(filter, p->status)))
				diff_q(&outq, p);
			else
				diff_free_filepair(p);
		}
	}
	free(q->queue);
	*q = outq;
}

/* Check whether two filespecs with the same mode and size are identical */
static int diff_filespec_is_identical(struct diff_filespec *one,
				      struct diff_filespec *two)
{
	if (S_ISGITLINK(one->mode))
		return 0;
	if (diff_populate_filespec(one, 0))
		return 0;
	if (diff_populate_filespec(two, 0))
		return 0;
	return !memcmp(one->data, two->data, one->size);
}

static void diffcore_skip_stat_unmatch(struct diff_options *diffopt)
{
	int i;
	struct diff_queue_struct *q = &diff_queued_diff;
	struct diff_queue_struct outq;
	outq.queue = NULL;
	outq.nr = outq.alloc = 0;

	for (i = 0; i < q->nr; i++) {
		struct diff_filepair *p = q->queue[i];

		/*
		 * 1. Entries that come from stat info dirtyness
		 *    always have both sides (iow, not create/delete),
		 *    one side of the object name is unknown, with
		 *    the same mode and size.  Keep the ones that
		 *    do not match these criteria.  They have real
		 *    differences.
		 *
		 * 2. At this point, the file is known to be modified,
		 *    with the same mode and size, and the object
		 *    name of one side is unknown.  Need to inspect
		 *    the identical contents.
		 */
		if (!DIFF_FILE_VALID(p->one) || /* (1) */
		    !DIFF_FILE_VALID(p->two) ||
		    (p->one->sha1_valid && p->two->sha1_valid) ||
		    (p->one->mode != p->two->mode) ||
		    diff_populate_filespec(p->one, 1) ||
		    diff_populate_filespec(p->two, 1) ||
		    (p->one->size != p->two->size) ||
		    !diff_filespec_is_identical(p->one, p->two)) /* (2) */
			diff_q(&outq, p);
		else {
			/*
			 * The caller can subtract 1 from skip_stat_unmatch
			 * to determine how many paths were dirty only
			 * due to stat info mismatch.
			 */
			if (!DIFF_OPT_TST(diffopt, NO_INDEX))
				diffopt->skip_stat_unmatch++;
			diff_free_filepair(p);
		}
	}
	free(q->queue);
	*q = outq;
}

void diffcore_std(struct diff_options *options)
{
	if (options->skip_stat_unmatch)
		diffcore_skip_stat_unmatch(options);
	if (options->break_opt != -1)
		diffcore_break(options->break_opt);
	if (options->detect_rename)
		diffcore_rename(options);
	if (options->break_opt != -1)
		diffcore_merge_broken();
	if (options->pickaxe)
		diffcore_pickaxe(options->pickaxe, options->pickaxe_opts);
	if (options->orderfile)
		diffcore_order(options->orderfile);
	diff_resolve_rename_copy();
	diffcore_apply_filter(options->filter);

	if (diff_queued_diff.nr)
		DIFF_OPT_SET(options, HAS_CHANGES);
	else
		DIFF_OPT_CLR(options, HAS_CHANGES);
}

int diff_result_code(struct diff_options *opt, int status)
{
	int result = 0;
	if (!DIFF_OPT_TST(opt, EXIT_WITH_STATUS) &&
	    !(opt->output_format & DIFF_FORMAT_CHECKDIFF))
		return status;
	if (DIFF_OPT_TST(opt, EXIT_WITH_STATUS) &&
	    DIFF_OPT_TST(opt, HAS_CHANGES))
		result |= 01;
	if ((opt->output_format & DIFF_FORMAT_CHECKDIFF) &&
	    DIFF_OPT_TST(opt, CHECK_FAILED))
		result |= 02;
	return result;
}

void diff_addremove(struct diff_options *options,
		    int addremove, unsigned mode,
		    const unsigned char *sha1,
		    const char *concatpath)
{
	struct diff_filespec *one, *two;

	if (DIFF_OPT_TST(options, IGNORE_SUBMODULES) && S_ISGITLINK(mode))
		return;

	/* This may look odd, but it is a preparation for
	 * feeding "there are unchanged files which should
	 * not produce diffs, but when you are doing copy
	 * detection you would need them, so here they are"
	 * entries to the diff-core.  They will be prefixed
	 * with something like '=' or '*' (I haven't decided
	 * which but should not make any difference).
	 * Feeding the same new and old to diff_change()
	 * also has the same effect.
	 * Before the final output happens, they are pruned after
	 * merged into rename/copy pairs as appropriate.
	 */
	if (DIFF_OPT_TST(options, REVERSE_DIFF))
		addremove = (addremove == '+' ? '-' :
			     addremove == '-' ? '+' : addremove);

	if (options->prefix &&
	    strncmp(concatpath, options->prefix, options->prefix_length))
		return;

	one = alloc_filespec(concatpath);
	two = alloc_filespec(concatpath);

	if (addremove != '+')
		fill_filespec(one, sha1, mode);
	if (addremove != '-')
		fill_filespec(two, sha1, mode);

	diff_queue(&diff_queued_diff, one, two);
	DIFF_OPT_SET(options, HAS_CHANGES);
}

void diff_change(struct diff_options *options,
		 unsigned old_mode, unsigned new_mode,
		 const unsigned char *old_sha1,
		 const unsigned char *new_sha1,
		 const char *concatpath)
{
	struct diff_filespec *one, *two;

	if (DIFF_OPT_TST(options, IGNORE_SUBMODULES) && S_ISGITLINK(old_mode)
			&& S_ISGITLINK(new_mode))
		return;

	if (DIFF_OPT_TST(options, REVERSE_DIFF)) {
		unsigned tmp;
		const unsigned char *tmp_c;
		tmp = old_mode; old_mode = new_mode; new_mode = tmp;
		tmp_c = old_sha1; old_sha1 = new_sha1; new_sha1 = tmp_c;
	}

	if (options->prefix &&
	    strncmp(concatpath, options->prefix, options->prefix_length))
		return;

	one = alloc_filespec(concatpath);
	two = alloc_filespec(concatpath);
	fill_filespec(one, old_sha1, old_mode);
	fill_filespec(two, new_sha1, new_mode);

	diff_queue(&diff_queued_diff, one, two);
	DIFF_OPT_SET(options, HAS_CHANGES);
}

void diff_unmerge(struct diff_options *options,
		  const char *path,
		  unsigned mode, const unsigned char *sha1)
{
	struct diff_filespec *one, *two;

	if (options->prefix &&
	    strncmp(path, options->prefix, options->prefix_length))
		return;

	one = alloc_filespec(path);
	two = alloc_filespec(path);
	fill_filespec(one, sha1, mode);
	diff_queue(&diff_queued_diff, one, two)->is_unmerged = 1;
}

static char *run_textconv(const char *pgm, struct diff_filespec *spec,
		size_t *outsize)
{
	struct diff_tempfile *temp;
	const char *argv[3];
	const char **arg = argv;
	struct child_process child;
	struct strbuf buf = STRBUF_INIT;

	temp = prepare_temp_file(spec->path, spec);
	*arg++ = pgm;
	*arg++ = temp->name;
	*arg = NULL;

	memset(&child, 0, sizeof(child));
	child.argv = argv;
	child.out = -1;
	if (start_command(&child) != 0 ||
	    strbuf_read(&buf, child.out, 0) < 0 ||
	    finish_command(&child) != 0) {
		remove_tempfile();
		error("error running textconv command '%s'", pgm);
		return NULL;
	}
	remove_tempfile();

	return strbuf_detach(&buf, outsize);
}
