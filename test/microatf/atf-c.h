#ifndef MICROATF_ATF_C_H_
#define MICROATF_ATF_C_H_

#include <sys/queue.h>

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

/**/

struct atf_tc_s;
typedef struct atf_tc_s atf_tc_t;
struct atf_tc_s {
	char const *name;
	void (*test_func)(atf_tc_t const *);
	char const *variables[128];
	size_t variables_size;
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
	MICROATF_ERROR_TOO_MANY_VARIABLES,
};

/**/

typedef struct {
	FILE *result_file;
	bool do_close_result_file;
	atf_tc_t *test_case;
} microatf_context_t;

static microatf_context_t microatf_context;

static inline void
microatf_context_write_result(microatf_context_t *context, char const *result,
    char const *reason, ...)
{
	fprintf(context->result_file, "%s", result);

	if (reason) {
		fprintf(context->result_file, ": ");

		va_list args;
		va_start(args, reason);
		vfprintf(context->result_file, reason, args);
		va_end(args);
	}

	fprintf(context->result_file, "\n");

	if (context->do_close_result_file) {
		fclose(context->result_file);
	}
}

/**/

#define ATF_REQUIRE(expression)                                               \
	do {                                                                  \
		if (!(expression)) {                                          \
			microatf_context_write_result(&microatf_context,      \
			    "failed", "%s:%d: %s", __FILE__, __LINE__,        \
			    #expression " not met");                          \
			exit(EXIT_FAILURE);                                   \
		}                                                             \
	} while (0)

#define ATF_REQUIRE_EQ(expected, actual)                                      \
	do {                                                                  \
		if ((expected) != (actual)) {                                 \
			microatf_context_write_result(&microatf_context,      \
			    "failed", "%s:%d: %s != %s", __FILE__, __LINE__,  \
			    #expected, #actual);                              \
			exit(EXIT_FAILURE);                                   \
		}                                                             \
	} while (0)

#define ATF_REQUIRE_ERRNO(exp_errno, bool_expr)                               \
	do {                                                                  \
		if (!(bool_expr)) {                                           \
			microatf_context_write_result(&microatf_context,      \
			    "failed", "%s:%d: Expected true value in %s\n",   \
			    __FILE__, __LINE__, #bool_expr);                  \
			exit(EXIT_FAILURE);                                   \
		}                                                             \
		int ec = errno;                                               \
		if (ec != (exp_errno)) {                                      \
			microatf_context_write_result(&microatf_context,      \
			    "failed",                                         \
			    "%s:%d: Expected errno %d, got %d, in %s\n",      \
			    __FILE__, __LINE__, exp_errno, ec, #bool_expr);   \
			exit(EXIT_FAILURE);                                   \
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
	char const *result_file_path = NULL;
	char const *srcdir_path = NULL;
	char const *variables[128];
	size_t variables_size = 0;

	int ch;
	while ((ch = getopt(argc, argv, "lr:s:v:")) != -1) {
		switch (ch) {
		case 'l':
			list_tests = true;
			break;
		case 'r':
			result_file_path = optarg;
			break;
		case 's':
			srcdir_path = optarg;
			break;
		case 'v':
			if (variables_size == 128) {
				ec = MICROATF_ERROR_TOO_MANY_VARIABLES;
				goto out;
			}
			variables[variables_size] = optarg;
			++variables_size;
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

	if (!result_file_path ||
	    strcmp(result_file_path, "/dev/stdout") == 0) {
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

	microatf_context = (microatf_context_t){
	    .result_file = result_file,
	    .do_close_result_file = do_close_result_file,
	    .test_case = matching_tc,
	};

	for (size_t i = 0; i < variables_size; ++i) {
		matching_tc->variables[i] = variables[i];
	}
	matching_tc->variables_size = variables_size;

	matching_tc->test_func(matching_tc);

	/**/

	microatf_context_write_result(&microatf_context, "passed", NULL);

out:
	return ec ? 1 : 0;
}

static inline atf_error_t
atf_no_error(void)
{
	return 0;
}

#endif
