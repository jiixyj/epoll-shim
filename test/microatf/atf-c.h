#ifndef MICROATF_ATF_C_H_
#define MICROATF_ATF_C_H_

#include <sys/queue.h>

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

/**/

#define MICROATF_ATTRIBUTE_UNUSED __attribute__((__unused__))

/**/

typedef int atf_error_t;

enum microatf_error_code {
	MICROATF_SUCCESS,
	MICROATF_ERROR_ARGUMENT_PARSING,
	MICROATF_ERROR_NO_MATCHING_TEST_CASE,
	MICROATF_ERROR_RESULT_FILE,
	MICROATF_ERROR_TOO_MANY_VARIABLES,
};

enum microatf_expect_type {
	MICROATF_EXPECT_PASS,
	MICROATF_EXPECT_FAIL,
	MICROATF_EXPECT_EXIT,
	MICROATF_EXPECT_SIGNAL,
	MICROATF_EXPECT_DEATH,
	MICROATF_EXPECT_TIMEOUT,
};

/**/

struct atf_tc_s;
typedef struct atf_tc_s atf_tc_t;
struct atf_tc_s {
	char const *name;
	void (*head)(atf_tc_t *);
	void (*body)(atf_tc_t const *);
	char const *variables_key[128];
	char const *variables_value[128];
	size_t variables_size;
	char const *config_variables_key[128];
	char const *config_variables_value[128];
	size_t config_variables_size;
	STAILQ_ENTRY(atf_tc_s) entries;
};

static inline atf_error_t
atf_tc_set_md_var(atf_tc_t *tc, char const *key, char const *value, ...)
{
	size_t key_length = strlen(key);

	for (size_t i = 0; i < tc->variables_size; ++i) {
		if (strncmp(tc->variables_key[i], key, key_length) == 0) {
			tc->variables_value[i] = value;
			return MICROATF_SUCCESS;
		}
	}

	if (tc->variables_size == 128) {
		return MICROATF_ERROR_TOO_MANY_VARIABLES;
	}

	tc->variables_key[tc->variables_size] = key;
	tc->variables_value[tc->variables_size] = value;

	++tc->variables_size;

	return MICROATF_SUCCESS;
}

static inline const char *
atf_tc_get_md_var(atf_tc_t const *tc, const char *key)
{
	size_t key_length = strlen(key);

	for (size_t i = 0; i < tc->variables_size; ++i) {
		if (strncmp(tc->variables_key[i], key, key_length) == 0) {
			return tc->variables_value[i];
		}
	}

	return NULL;
}

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

typedef struct {
	char const *result_file_path;
	FILE *result_file;
	bool do_close_result_file;
	atf_tc_t *test_case;

	enum microatf_expect_type expect;

	int fail_count;
	int expect_fail_count;
	int expect_previous_fail_count;
} microatf_context_t;

static microatf_context_t microatf_context;

static inline void
microatf_context_write_result_pack(microatf_context_t *context,
    char const *result, const int arg, char const *reason, va_list args)
{
	if (!context->result_file) {
		return;
	}

#ifdef __NetBSD__
	fclose(context->result_file);
	context->result_file = fopen(context->result_file_path, "w");
#else
	context->result_file = freopen(NULL, "w", context->result_file);
#endif
	if (!context->result_file) {
		return;
	}

	fprintf(context->result_file, "%s", result);

	if (arg != -1) {
		fprintf(context->result_file, "(%d)", arg);
	}

	if (reason) {
		fprintf(context->result_file, ": ");
		vfprintf(context->result_file, reason, args);
	}

	fprintf(context->result_file, "\n");
	fflush(context->result_file);
}

static inline void
microatf_context_write_result(microatf_context_t *context, char const *result,
    const int arg, char const *reason, ...)
{
	va_list args;
	va_start(args, reason);
	microatf_context_write_result_pack(context, result, arg, reason, args);
	va_end(args);
}

static inline void
microatf_context_exit(microatf_context_t *context, int exit_code)
{
	if (context->do_close_result_file && context->result_file) {
		fclose(context->result_file);
		context->result_file = NULL;
	}

	exit(exit_code);
}

static inline void
microatf_context_validate_expect(microatf_context_t *context)
{
	if (context->expect == MICROATF_EXPECT_FAIL) {
		if (context->expect_fail_count ==
		    context->expect_previous_fail_count) {
			microatf_context_write_result(context, "failed", -1,
			    "Test case was expecting a failure but none were "
			    "raised");
			microatf_context_exit(context, EXIT_FAILURE);
		}
	} else if (context->expect) {
		microatf_context_write_result(context, "failed", -1,
		    "Test case continued");
		microatf_context_exit(context, EXIT_FAILURE);
	}
}

static inline void
microatf_context_pass(microatf_context_t *context)
{
	if (context->expect == MICROATF_EXPECT_FAIL) {
		microatf_context_write_result(context, "failed", -1,
		    "Test case was expecting a failure but got a pass "
		    "instead");
		microatf_context_exit(context, EXIT_FAILURE);
	} else if (context->expect == MICROATF_EXPECT_PASS) {
		microatf_context_write_result(&microatf_context, "passed", -1,
		    NULL);
		microatf_context_exit(&microatf_context, EXIT_SUCCESS);
	} else {
		microatf_context_write_result(context, "failed", -1,
		    "Test case asked to explicitly pass but was not expecting "
		    "such condition");
		microatf_context_exit(context, EXIT_FAILURE);
	}
}

static inline void
microatf_context_fail_check(microatf_context_t *context, /**/
    char const *msg, ...)
{
	if (context->expect == MICROATF_EXPECT_FAIL) {
		fprintf(stderr, "*** Expected check failure: ");

		va_list args;
		va_start(args, msg);
		vfprintf(stderr, msg, args);
		va_end(args);

		fprintf(stderr, "\n");

		context->expect_fail_count++;
	} else if (context->expect == MICROATF_EXPECT_PASS) {
		fprintf(stderr, "*** Check failed: ");

		va_list args;
		va_start(args, msg);
		vfprintf(stderr, msg, args);
		va_end(args);

		fprintf(stderr, "\n");

		context->fail_count++;
	} else {
		va_list args;
		va_start(args, msg);
		char const *file = va_arg(args, char const *);
		int line = va_arg(args, int);
		microatf_context_write_result(context, "failed", -1,
		    "%s:%d: %s", file, line,
		    "Test case raised a failure but was not expecting one");
		va_end(args);

		microatf_context_exit(context, EXIT_FAILURE);
	}
}

static inline void
microatf_context_fail_require(microatf_context_t *context, /**/
    char const *msg, ...)
{
	if (context->expect == MICROATF_EXPECT_FAIL) {
		va_list args;
		va_start(args, msg);
		microatf_context_write_result_pack(context, /**/
		    "expected_failure", -1, msg, args);
		va_end(args);

		microatf_context_exit(context, EXIT_SUCCESS);
	} else if (context->expect == MICROATF_EXPECT_PASS) {
		va_list args;
		va_start(args, msg);
		microatf_context_write_result_pack(context, /**/
		    "failed", -1, msg, args);
		va_end(args);

		microatf_context_exit(context, EXIT_FAILURE);
	} else {
		va_list args;
		va_start(args, msg);
		char const *file = va_arg(args, char const *);
		int line = va_arg(args, int);
		microatf_context_write_result(context, "failed", -1,
		    "%s:%d: %s", file, line,
		    "Test case raised a failure but was not expecting one");
		va_end(args);

		microatf_context_exit(context, EXIT_FAILURE);
	}
}

/**/

static inline void
atf_tc_expect_timeout(const char *msg, ...)
{
	microatf_context_validate_expect(&microatf_context);
	microatf_context.expect = MICROATF_EXPECT_TIMEOUT;

	va_list args;
	va_start(args, msg);
	microatf_context_write_result_pack(&microatf_context,
	    "expected_timeout", -1, msg, args);
	va_end(args);
}

static inline void
atf_tc_expect_exit(const int exitcode, const char *msg, ...)
{
	microatf_context_validate_expect(&microatf_context);
	microatf_context.expect = MICROATF_EXPECT_EXIT;

	va_list args;
	va_start(args, msg);
	microatf_context_write_result_pack(&microatf_context, /**/
	    "expected_exit", exitcode, msg, args);
	va_end(args);
}

static inline void
atf_tc_expect_signal(const int signal, const char *msg, ...)
{
	microatf_context_validate_expect(&microatf_context);
	microatf_context.expect = MICROATF_EXPECT_SIGNAL;

	va_list args;
	va_start(args, msg);
	microatf_context_write_result_pack(&microatf_context, /**/
	    "expected_signal", signal, msg, args);
	va_end(args);
}

static inline void
atf_tc_expect_fail(const char *msg, ...)
{
	(void)msg;

	microatf_context_validate_expect(&microatf_context);
	microatf_context.expect = MICROATF_EXPECT_FAIL;

	microatf_context.expect_previous_fail_count =
	    microatf_context.expect_fail_count;
}

/**/

static inline void
atf_tc_skip(const char *reason, ...)
{
	microatf_context_t *context = &microatf_context;

	if (context->expect == MICROATF_EXPECT_PASS) {
		va_list args;
		va_start(args, reason);
		microatf_context_write_result_pack(context, /**/
		    "skipped", -1, reason, args);
		va_end(args);

		microatf_context_exit(context, EXIT_SUCCESS);
	} else {
		microatf_context_write_result(context, "failed", -1,
		    "Can only skip a test case when running in "
		    "expect pass mode");
		microatf_context_exit(context, EXIT_FAILURE);
	}
}

/**/

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"

#define ATF_REQUIRE_MSG(expression, fmt, ...)                                 \
	do {                                                                  \
		if (!(expression)) {                                          \
			microatf_context_fail_require(&microatf_context,      \
			    "%s:%d: " fmt, __FILE__, __LINE__,                \
			    ##__VA_ARGS__);                                   \
		}                                                             \
	} while (0)

#define ATF_CHECK_MSG(expression, fmt, ...)                                   \
	do {                                                                  \
		if (!(expression)) {                                          \
			microatf_context_fail_require(&microatf_context,      \
			    "%s:%d: " fmt, __FILE__, __LINE__,                \
			    ##__VA_ARGS__);                                   \
		}                                                             \
	} while (0)

#pragma clang diagnostic pop

#define ATF_REQUIRE(expression)                                               \
	ATF_REQUIRE_MSG((expression), "%s not met", #expression)

#define ATF_CHECK(expression)                                                 \
	ATF_CHECK_MSG((expression), "%s not met", #expression)

#define ATF_REQUIRE_EQ(expected, actual)                                      \
	ATF_REQUIRE_MSG((expected) == (actual), "%s != %s", #expected, #actual)

#define ATF_CHECK_EQ(expected, actual)                                        \
	ATF_CHECK_MSG((expected) == (actual), "%s != %s", #expected, #actual)

#define ATF_REQUIRE_ERRNO(exp_errno, bool_expr)                               \
	do {                                                                  \
		ATF_REQUIRE_MSG((bool_expr),	 /**/                         \
		    "Expected true value in %s", /**/                         \
		    #bool_expr);                                              \
		int ec = errno;                                               \
		ATF_REQUIRE_MSG(ec == (exp_errno),	/**/                  \
		    "Expected errno %d, got %d, in %s", /**/                  \
		    (exp_errno), ec, #bool_expr);                             \
	} while (0)

#define ATF_TC_WITHOUT_HEAD(tc)                                               \
	static void microatf_tc_##tc##_body(atf_tc_t const *);                \
	static atf_tc_t microatf_tc_##tc = {                                  \
	    .name = #tc,                                                      \
	    .head = NULL,                                                     \
	    .body = microatf_tc_##tc##_body,                                  \
	}

#define ATF_TC(tc)                                                            \
	static void microatf_tc_##tc##_head(atf_tc_t *);                      \
	static void microatf_tc_##tc##_body(atf_tc_t const *);                \
	static atf_tc_t microatf_tc_##tc = {                                  \
	    .name = #tc,                                                      \
	    .head = microatf_tc_##tc##_head,                                  \
	    .body = microatf_tc_##tc##_body,                                  \
	}

#define ATF_TC_HEAD(tc, tcptr)                                                \
	static void microatf_tc_##tc##_head(                                  \
	    atf_tc_t *tcptr MICROATF_ATTRIBUTE_UNUSED)

#define ATF_TC_BODY(tc, tcptr)                                                \
	static void microatf_tc_##tc##_body(                                  \
	    atf_tc_t const *tcptr MICROATF_ATTRIBUTE_UNUSED)

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
		atf_tc_t *tst = &microatf_tc_##tc;                            \
		char const *ident = tst->name;                                \
		atf_tc_set_md_var(tst, "ident", ident);                       \
		if (tst->head != NULL) {                                      \
			tst->head(tst);                                       \
		}                                                             \
		if (strcmp(atf_tc_get_md_var(tst, "ident"), ident) != 0) {    \
			abort();                                              \
		}                                                             \
		STAILQ_INSERT_TAIL(&tp->tcs, tst, entries);                   \
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
	char const *config_variables[128];
	size_t config_variables_size = 0;

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
			if (config_variables_size == 128) {
				ec = MICROATF_ERROR_TOO_MANY_VARIABLES;
				goto out;
			}
			config_variables[config_variables_size] = optarg;
			++config_variables_size;
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
		printf("Content-Type: application/X-atf-tp; "
		       "version=\"1\"\n\n");

		bool print_newline = false;

		atf_tc_t *tc;
		STAILQ_FOREACH(tc, &tp.tcs, entries)
		{
			if (print_newline) {
				printf("\n");
			}
			print_newline = true;

			for (size_t i = 0; i < tc->variables_size; ++i) {
				char *key_end =
				    strchr(tc->variables_key[i], '=');
				ptrdiff_t key_length = key_end
				    ? (key_end - tc->variables_key[i])
				    : (ptrdiff_t)strlen(tc->variables_key[i]);
				printf("%.*s: %s\n", (int)key_length,
				    tc->variables_key[i],
				    tc->variables_value[i]);
			}
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

	if (!result_file_path) {
		result_file_path = "/dev/stdout";
	}

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

	microatf_context = (microatf_context_t){
	    .result_file_path = result_file_path,
	    .result_file = result_file,
	    .do_close_result_file = do_close_result_file,
	    .test_case = matching_tc,
	};

	for (size_t i = 0; i < config_variables_size; ++i) {
		matching_tc->config_variables_key[i] = config_variables[i];
		matching_tc->config_variables_value[i] =
		    strchr(config_variables[i], '=');
		if (!matching_tc->config_variables_value[i]) {
			ec = MICROATF_ERROR_ARGUMENT_PARSING;
			goto out;
		}
		++matching_tc->config_variables_value[i];
	}
	matching_tc->config_variables_size = config_variables_size;

	matching_tc->body(matching_tc);

	/**/

	microatf_context_validate_expect(&microatf_context);

	if (microatf_context.fail_count > 0) {
		microatf_context_write_result(&microatf_context, "failed", -1,
		    "Some checks failed");
		microatf_context_exit(&microatf_context, EXIT_FAILURE);
	} else if (microatf_context.expect_fail_count > 0) {
		microatf_context_write_result(&microatf_context,
		    "expected_failure", -1, "Some checks failed as expected");
		microatf_context_exit(&microatf_context, EXIT_SUCCESS);
	} else {
		microatf_context_pass(&microatf_context);
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
