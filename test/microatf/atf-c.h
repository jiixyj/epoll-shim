#ifndef MICROATF_ATF_C_H_
#define MICROATF_ATF_C_H_

#include <sys/queue.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

struct atf_tc_s;
typedef struct atf_tc_s atf_tc_t;
struct atf_tc_s {
	char const *name;
	void (*test_func)(atf_tc_t const *);
	STAILQ_ENTRY(atf_tc_s) entries;
};

/**/

STAILQ_HEAD(atf_tc_list_s, atf_tc_s);
typedef struct atf_tc_list_s atf_tc_list_t;

typedef struct atf_tp_s {
	atf_tc_list_t tcs;
} atf_tp_t;

static inline void
atf_tp_init(atf_tp_t *tp)
{
	*tp = (atf_tp_t){.tcs = STAILQ_HEAD_INITIALIZER(tp->tcs)};
}

/**/

typedef errno_t atf_error_t;

enum microatf_error_code {
	MICROATF_SUCCESS,
	MICROATF_ERROR_ARGUMENT_PARSING,
	MICROATF_ERROR_NO_MATCHING_TEST_CASE,
	MICROATF_ERROR_RESULT_FILE,
};

/**/

#define ATF_REQUIRE(expression)                                               \
	do {                                                                  \
		if (!(expression)) {                                          \
			fprintf(stderr, "%s\n", #expression "not met");       \
			abort();                                              \
		}                                                             \
	} while (0)

#define ATF_REQUIRE_ERRNO(exp_errno, bool_expr)                               \
	do {                                                                  \
		ATF_REQUIRE(bool_expr);                                       \
		int ec = errno;                                               \
		if (ec != (exp_errno)) {                                      \
			fprintf(stderr, "Expected errno %d, got %d, in %s\n", \
			    exp_errno, ec, #bool_expr);                       \
			abort();                                              \
		}                                                             \
	} while (0)

#define ATF_TC_WITHOUT_HEAD(tc)                                               \
	static void microatf_tc_##tc##_body(atf_tc_t const *);                \
	static atf_tc_t microatf_tc_##tc = {                                  \
	    .name = #tc,                                                      \
	    .test_func = microatf_tc_##tc##_body,                             \
	}

#define ATF_TC_BODY(tc, tcptr)                                                \
	static void microatf_tc_##tc##_body(atf_tc_t const *tcptr __unused)

#define ATF_TP_ADD_TCS(tps)                                                   \
	static atf_error_t microatf_tp_add_tcs(atf_tp_t *);                   \
	static inline int microatf_tp_main(int, char **,                      \
	    atf_error_t (*)(atf_tp_t *));                                     \
                                                                              \
	int main(int argc, char **argv)                                       \
	{                                                                     \
		return microatf_tp_main(argc, argv, microatf_tp_add_tcs);     \
	}                                                                     \
	static atf_error_t microatf_tp_add_tcs(atf_tp_t *tps)

#define ATF_TP_ADD_TC(tp, tc)                                                 \
	do {                                                                  \
		STAILQ_INSERT_TAIL(&tp->tcs, &microatf_tc_##tc, entries);     \
	} while (0)

static inline int
microatf_tp_main(int argc, char **argv,
    atf_error_t (*add_tcs_hook)(atf_tp_t *))
{
	atf_error_t ec;

	bool list_tests = false;
	char const *test_case_name = NULL;
	char const *result_file_path = "/dev/stdout";

	int ch;
	while ((ch = getopt(argc, argv, "lr:v:")) != -1) {
		switch (ch) {
		case 'l':
			list_tests = true;
			break;
		case 'r':
			result_file_path = optarg;
			break;
		case 'v':
			// Ignore all variables for now.
			break;
		case '?':
		default:
			ec = MICROATF_ERROR_ARGUMENT_PARSING;
			goto out;
		}
	}
	argc -= optind;
	argv += optind;

	if (list_tests) {
		if (argc != 0) {
			ec = MICROATF_ERROR_ARGUMENT_PARSING;
			goto out;
		}
	} else {
		if (argc != 1) {
			ec = MICROATF_ERROR_ARGUMENT_PARSING;
			goto out;
		}
		test_case_name = argv[0];
	}

	atf_tp_t tp;
	atf_tp_init(&tp);

	ec = add_tcs_hook(&tp);
	if (ec) {
		goto out;
	}

	if (list_tests) {
		printf(
		    "Content-Type: application/X-atf-tp; version=\"1\"\n\n");

		bool print_newline = false;

		atf_tc_t *tc;
		STAILQ_FOREACH(tc, &tp.tcs, entries)
		{
			if (print_newline) {
				printf("\n");
			}
			print_newline = true;

			printf("ident: %s\n", tc->name);
		}

		return 0;
	}

	atf_tc_t *matching_tc = NULL;

	atf_tc_t *tc;
	STAILQ_FOREACH(tc, &tp.tcs, entries)
	{
		if (strcmp(tc->name, test_case_name) == 0) {
			matching_tc = tc;
			break;
		}
	}

	if (!matching_tc) {
		ec = MICROATF_ERROR_NO_MATCHING_TEST_CASE;
		goto out;
	}

	bool do_close_result_file = false;
	FILE *result_file;

	if (strcmp(result_file_path, "/dev/stdout") == 0) {
		result_file = stdout;
	} else if (strcmp(result_file_path, "/dev/stderr") == 0) {
		result_file = stderr;
	} else {
		do_close_result_file = true;

		result_file = fopen(result_file_path, "w");
		if (!result_file) {
			ec = MICROATF_ERROR_RESULT_FILE;
			goto out;
		}
	}

	/* Run the test case. */

	matching_tc->test_func(matching_tc);

	/**/

	fprintf(result_file, "passed\n");

	if (do_close_result_file) {
		fclose(result_file);
	}

out:
	return ec ? 1 : 0;
}

static inline atf_error_t
atf_no_error(void)
{
	return 0;
}

#endif
