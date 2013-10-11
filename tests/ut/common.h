#ifndef MCE_TESTS_UT_COMMON_H
#define MCE_TESTS_UT_COMMON_H

#include <check.h>
#include <glib/gprintf.h>
#include <sys/cdefs.h>
#include <sys/mman.h>
#include <stdint.h>
#include <unistd.h>

#if CHECK_MAJOR_VERSION < 0 || CHECK_MINOR_VERSION < 9 || CHECK_MICRO_VERSION < 9
#define _ck_assert_msg ck_assert_msg
#define ck_assert_int_lt(X, Y) ck_assert((X) < (Y))
#endif

/* ------------------------------------------------------------------------- *
 * UTILITIES FOR WRITING FUNCTION STUBS
 * ------------------------------------------------------------------------- */

/* Support for stubs for external functions */

#define EXTERN_STUB(ret, name, args)					     \
	ret __REDIRECT(name, args, stub__##name);			     \
	/*  extra prototype to silent -Wmissing-prototypes */		     \
	ret stub__##name args;						     \
	ret stub__##name args

#define EXTERN_DUMMY_STUB(ret, name, args)				     \
	ret __REDIRECT(name, args, stub__##name);			     \
	_Pragma("GCC diagnostic push")					     \
	_Pragma("GCC diagnostic ignored \"-Wunused-parameter\"")	     \
	/* extra prototype to silent -Wmissing-prototypes */		     \
	ret stub__##name args;						     \
	__attribute__ ((noreturn)) ret stub__##name args		     \
	{								     \
		printf("fatal: dummy stub called: "#ret" "#name#args"\n");   \
		abort();						     \
	}								     \
	_Pragma("GCC diagnostic pop")

/* Support for stubs for static functions */

#if defined(__i386__)

# define _LOCAL_STUB_DECL_JMP_ASM_FIELDS				     \
	unsigned char jmp;						     \
	uint32_t displacement;
# define _LOCAL_STUB_DEFN_JMP_ASM_FIELDS(name)				     \
	.jmp = '\xe9',							     \
	.displacement = (char *)stub__##name				     \
		      - ((char *)name + sizeof(struct jmp2stub_asm)),

#elif defined(__arm__)

# define _LOCAL_STUB_DECL_JMP_ASM_FIELDS				     \
	uint32_t displacement:24;					     \
	uint32_t jmp:8;
# define _LOCAL_STUB_DEFN_JMP_ASM_FIELDS(name)				     \
	.jmp = '\xea',							     \
	.displacement = ((char *)stub__##name - ((char *)name + 8)) >> 2,

#else
# error "Target architecture unsupported"
#endif

#define LOCAL_STUB(ret, name, args)					     \
	__attribute__ ((noinline)) ret name args;			     \
	static ret stub__##name args;					     \
	__attribute__ ((constructor)) static void _init_stub__##name(void)   \
	{								     \
		struct jmp2stub_asm					     \
		{							     \
			_LOCAL_STUB_DECL_JMP_ASM_FIELDS			     \
		} __attribute__ ((__packed__)) jmp2stub_asm = {		     \
			_LOCAL_STUB_DEFN_JMP_ASM_FIELDS(name)		     \
		};							     \
		void *name##__aligned =					     \
			(void *)((size_t)name & ~(sysconf(_SC_PAGESIZE)-1)); \
		if( mprotect(name##__aligned,				     \
			     ((char *)name - (char *)name##__aligned)	     \
			     + sizeof(struct jmp2stub_asm),		     \
			     PROT_READ | PROT_WRITE | PROT_EXEC) != 0 ) {    \
			perror("mprotect() failed");			     \
			abort();					     \
		}							     \
		memcpy(name, &jmp2stub_asm, sizeof(struct jmp2stub_asm));    \
	}								     \
	static ret stub__##name args

#define LOCAL_DUMMY_STUB(ret, name, args)				     \
	LOCAL_STUB(ret, name, args);					     \
	_Pragma("GCC diagnostic push")					     \
	_Pragma("GCC diagnostic ignored \"-Wunused-parameter\"")	     \
	__attribute__ ((noreturn)) static ret stub__##name args		     \
	{								     \
		printf("fatal: dummy stub called: "#ret" "#name#args"\n");   \
		abort();						     \
	}								     \
	_Pragma("GCC diagnostic pop")

/* ------------------------------------------------------------------------- *
 * MAKE mce_log() WORK DURING TEST EXECUTION
 * ------------------------------------------------------------------------- */

/* Redefine START_TEST macro (from check.h) so it marks the begin of test
 * execution in log */

#undef START_TEST
#define START_TEST(__testname)					     \
	static void __testname (int _i CK_ATTRIBUTE_UNUSED)	     \
	{							     \
		tcase_fn_start (""# __testname, __FILE__, __LINE__); \
		printf("--- " # __testname " [%d]:\n", _i);

/* Provide stub for mce_log_file so the log output is to stdout, instead of
 * syslog */

#include "../../mce-log.h"

EXTERN_STUB (
void, mce_log_file, (loglevel_t loglevel, const char *const file,
		     const char *const function, const char *const fmt, ...))
{
	(void)file;

	const char *tag(loglevel_t level)
	{
		const char *res = "?";
		switch( level ) {
		case LL_CRIT:	res = "C"; break;
		case LL_ERR:	res = "E"; break;
		case LL_WARN:	res = "W"; break;
		case LL_NOTICE: res = "N"; break;
		case LL_INFO:	res = "I"; break;
		case LL_DEBUG:	res = "D"; break;
		default: break;
		}
		return res;
	}

	char *msg = 0;
	va_list args;

	va_start(args, fmt);
	g_vasprintf(&msg, fmt, args);
	va_end(args);

	printf("%s %s: %s\n", tag(loglevel), function, msg);

	g_free(msg);
}

/* ------------------------------------------------------------------------- *
 * OTHER
 * ------------------------------------------------------------------------- */

typedef enum
{
	UT_TRISTATE_UNDEF,
	UT_TRISTATE_FALSE,
	UT_TRISTATE_TRUE,
} ut_tristate_t;

G_GNUC_UNUSED
static void ut_wait_seconds(gint seconds)
{
	GMainLoop *loop = g_main_loop_new(NULL, TRUE);

	gboolean quit_loop(gpointer data)
	{
		(void)data;
		g_main_loop_quit(loop);
		return G_SOURCE_REMOVE;
	}
	g_timeout_add_seconds(seconds, quit_loop, NULL);

	g_main_loop_run(loop);
}

/* ------------------------------------------------------------------------- *
 * ASSERTING A FUTURE STATE CHANGE
 * ------------------------------------------------------------------------- */

typedef gboolean (*ut_state_test_t)(gpointer user_data);
static GMainLoop *_ut_transition_wait_loop = NULL;
static const int UT_TRANSITION_WAIT_TIME = 10; /* seconds */
static const int UT_COMPARE_TIME_TRESHOLD = 2; /* +/- seconds */

G_GNUC_UNUSED
static gboolean ut_fuzzy_compare_time(gdouble seconds1, gdouble seconds2)
{
	return ABS(seconds1 - seconds2) <= UT_COMPARE_TIME_TRESHOLD;
}

#define ut_transition_recheck_schedule() \
	_ut_transition_recheck_schedule(__FILE__, __LINE__, __PRETTY_FUNCTION__)

G_GNUC_UNUSED
static void _ut_transition_recheck_schedule(const char *loc_file,
					    int loc_line,
					    const char *loc_function)
{
	static guint scheduled_id = 0;

	printf("transition RECHECK @ %s:%d:%s()\n", loc_file, loc_line, loc_function);

	/* Already scheduled */
	if( scheduled_id != 0 )
		return;

	/* Nothing to check */
	if( _ut_transition_wait_loop == NULL )
		return;

	gboolean real(gpointer data)
	{
		(void)data;

		scheduled_id = 0;

		if( _ut_transition_wait_loop != NULL )
			g_main_loop_quit(_ut_transition_wait_loop);

		return G_SOURCE_REMOVE;
	}
	scheduled_id = g_idle_add(real, NULL);
}

#define ut_assert_transition(target_state_test, user_data)                            \
        _ut_assert_transition(target_state_test, user_data, G_MINDOUBLE, G_MAXDOUBLE, \
                           __FILE__, __LINE__,                                        \
                           G_STRINGIFY(target_state_test) " [user_data: "             \
                           G_STRINGIFY(user_data) " ]")
#define ut_assert_transition_time_eq(target_state_test, user_data, seconds)           \
        _ut_assert_transition(target_state_test, user_data,                           \
                           seconds - UT_COMPARE_TIME_TRESHOLD,                        \
                           seconds + UT_COMPARE_TIME_TRESHOLD,                        \
                           __FILE__, __LINE__,                                        \
                           G_STRINGIFY(target_state_test) " [user_data: "             \
                           G_STRINGIFY(user_data) " ]")

G_GNUC_UNUSED
static void _ut_assert_transition(ut_state_test_t target_state_test,
				  gpointer user_data,
				  gdouble expect_seconds_min,
				  gdouble expect_seconds_max,
				  const char *loc_file, int loc_line,
				  const char *tag)
{
	ck_assert(_ut_transition_wait_loop == NULL); /* not reentrant */

	gdouble time_elapsed = 0;

	if( (*target_state_test)(user_data) ) {
		printf("transition DONE '%s' @ %s:%i\n", tag, loc_file, loc_line);
		goto check_time_elapsed;
	}

	GTimer *timer = NULL;
	if( expect_seconds_min != G_MINDOUBLE
	    || expect_seconds_max != G_MAXDOUBLE ) {
		timer = g_timer_new();
	}

	_ut_transition_wait_loop = g_main_loop_new(NULL, FALSE);

	guint timeout_id = 0;
	gboolean timeout_cb(gpointer user_data_)
	{
		(void)user_data_;
		timeout_id = 0;
		g_main_loop_quit(_ut_transition_wait_loop);
		return G_SOURCE_REMOVE;
	}
	guint timeout = expect_seconds_max != G_MAXDOUBLE
		? expect_seconds_max + UT_TRANSITION_WAIT_TIME
		: expect_seconds_min != G_MINDOUBLE
		? expect_seconds_min + UT_TRANSITION_WAIT_TIME
		: UT_TRANSITION_WAIT_TIME;
	timeout_id = g_timeout_add_seconds(timeout, timeout_cb, NULL);

	guint recheck_id = 0;
	gboolean recheck_cb(gpointer user_data_)
	{
		(void)user_data_;
		g_main_loop_quit(_ut_transition_wait_loop);
		return G_SOURCE_CONTINUE;
	}
	recheck_id = g_timeout_add_seconds(1, recheck_cb, NULL);

	printf("transition BEGIN wait for '%s' @ %s:%i\n",
	       tag, loc_file, loc_line);

	do {
		g_main_loop_run(_ut_transition_wait_loop);
	} while( timeout_id != 0 && !(*target_state_test)(user_data) );

	printf("transition END wait for '%s' @ %s:%i\n",
	       tag, loc_file, loc_line);

	_ck_assert_msg(timeout_id != 0, loc_file, loc_line, "Failed",
		       "Timeout waiting for transition to: '%s'", tag);

	if( expect_seconds_min != G_MINDOUBLE
	    || expect_seconds_max != G_MAXDOUBLE ) {
		time_elapsed = g_timer_elapsed(timer, NULL);
		g_timer_destroy(timer), timer = NULL;
	}


	g_source_remove(recheck_id), recheck_id = 0;
	g_source_remove(timeout_id), timeout_id = 0;
	g_main_loop_unref(_ut_transition_wait_loop), _ut_transition_wait_loop = 0;

check_time_elapsed:
	if( expect_seconds_min != G_MINDOUBLE ) {
		_ck_assert_msg(time_elapsed >= expect_seconds_min,
			       loc_file, loc_line, "Failed",
			       "Passed too soon: '%s'. "
			       "Took ~%d secs.",
			       tag, (gint)time_elapsed);
	}

	if( expect_seconds_max != G_MAXDOUBLE ) {
		_ck_assert_msg(time_elapsed <= expect_seconds_max,
			       loc_file, loc_line, "Failed",
			       "Passed too late: '%s'. "
			       "Took ~%d secs.",
			       tag, (gint)time_elapsed);
	}
}

#endif // MCE_TESTS_UT_COMMON_H
