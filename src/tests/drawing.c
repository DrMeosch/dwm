/* Headless checks for UTF-8 decoding and drawing API guard clauses. */
#include <stdio.h>
#include <stdlib.h>

#include "../drw.c"

static int failures;

#define CHECK(condition, ...) do { \
	if (!(condition)) { \
		fprintf(stderr, "FAIL: "); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
		failures++; \
	} \
} while (0)

static void
check_utf8(const char *name, const char *text, int expected_len,
           long expected_codepoint, int expected_error)
{
	long codepoint = 0;
	int error = 0;
	int len = utf8decode(text, &codepoint, &error);

	CHECK(len == expected_len && codepoint == expected_codepoint
	      && error == expected_error,
	      "%s decoded as len=%d cp=%ld error=%d", name, len, codepoint, error);
}

static void
test_utf8(void)
{
	check_utf8("ASCII", "A", 1, 'A', 0);
	check_utf8("two-byte", "\302\242", 2, 0xA2, 0);
	check_utf8("three-byte", "\342\202\254", 3, 0x20AC, 0);
	check_utf8("four-byte", "\360\237\214\222", 4, 0x1F312, 0);
	check_utf8("continuation", "\200", 1, 0xFFFD, 1);
	check_utf8("truncated", "\342\202", 2, 0xFFFD, 1);
	check_utf8("bad continuation", "\342A", 1, 0xFFFD, 1);
	check_utf8("overlong", "\300\200", 2, 0xFFFD, 1);
	check_utf8("surrogate", "\355\240\200", 3, 0xFFFD, 1);
	check_utf8("out of range", "\364\220\200\200", 4, 0xFFFD, 1);
	check_utf8("five-byte", "\370", 1, 0xFFFD, 1);
}

static void
test_guard_clauses(void)
{
	Drw drw = {0};
	unsigned int width = 99, height = 88;
	Clr color = {0};
	char *fonts[] = {"monospace"};
	char *colors[] = {"#000", "#fff"};

	CHECK(drw_fontset_create(NULL, fonts, 1) == NULL,
	      "drw_fontset_create accepted a NULL context");
	CHECK(drw_fontset_create(&drw, NULL, 1) == NULL,
	      "drw_fontset_create accepted NULL font names");
	CHECK(drw_fontset_getwidth(NULL, "text") == 0
	      && drw_fontset_getwidth(&drw, "text") == 0,
	      "drw_fontset_getwidth did not guard missing fonts");
	CHECK(drw_fontset_getwidth_clamp(NULL, "text", 4) == 0
	      && drw_fontset_getwidth_clamp(&drw, "text", 4) == 0,
	      "drw_fontset_getwidth_clamp did not guard missing fonts");
	CHECK(drw_text(NULL, 0, 0, 0, 0, 0, "text", 0) == 0
	      && drw_text(&drw, 0, 0, 0, 0, 0, "text", 0) == 0,
	      "drw_text did not guard missing context or fonts");
	CHECK(drw_scm_create(NULL, colors, 2) == NULL
	      && drw_scm_create(&drw, colors, 1) == NULL,
	      "drw_scm_create did not reject invalid schemes");
	drw_font_getexts(NULL, "text", 4, &width, &height);
	CHECK(width == 99 && height == 88,
	      "drw_font_getexts changed outputs for a NULL font");
	drw_font_getexts(NULL, NULL, 0, &width, &height);
	drw_resize(NULL, 10, 10);
	drw_clr_create(NULL, &color, "#000");
	drw_clr_free(NULL, &color);
	drw_scm_free(NULL, &color, 1);
	drw_fontset_free(NULL);
	drw_cur_free(NULL, NULL);
	drw_setfontset(NULL, NULL);
	drw_setscheme(NULL, NULL);
	drw_rect(NULL, 0, 0, 1, 1, 1, 0);
	drw_map(NULL, 0, 0, 0, 1, 1);
}

int
main(void)
{
	test_utf8();
	test_guard_clauses();
	if (failures)
		return EXIT_FAILURE;
	puts("drawing regression checks passed");
	return EXIT_SUCCESS;
}
