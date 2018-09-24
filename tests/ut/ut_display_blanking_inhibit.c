#include <check.h>
#include <glib.h>

#include "common.h"

/* Tested module */
#include "../../modules/display.c"

/* ------------------------------------------------------------------------- *
 * STUBS
 * ------------------------------------------------------------------------- */

EXTERN_DUMMY_STUB (
submode_t, mce_get_submode_int32, (void));

EXTERN_DUMMY_STUB (
gconstpointer, datapipe_exec_full, (datapipe_t *const datapipe,
				    gpointer indata,
				    const datapipe_use_t use_cache,
				    const datapipe_cache_t cache_indata));

LOCAL_STUB (
void, cancel_blank_prevent, (void))
{
}

LOCAL_STUB (
void, setup_dim_timeout, (void))
{
}

LOCAL_STUB (
void, setup_lpm_timeout, (void))
{
}

LOCAL_STUB (
void, setup_blank_timeout, (void))
{
}

/* Stub init/cleanup */

static void stub_setup(void)
{
}

static void stub_teardown(void)
{
}

/* ------------------------------------------------------------------------- *
 * TESTS
 * ------------------------------------------------------------------------- */

#define DATA(sys, dsp, alrm, call, charger, mode, timed, ex_dim, ex_blank) { \
        "{ "#sys", "#dsp", "#alrm", "#call", "#charger", "#mode ", "#timed   \
        ", "#ex_dim", "#ex_blank" }",                                        \
        MCE_STATE_##sys, MCE_DISPLAY_##dsp, MCE_ALARM_UI_##alrm##_INT32,     \
        call, charger, INHIBIT_##mode, timed, ex_dim, ex_blank }
static struct ut_check_blanking_inhibit_data
{
	const gchar 	 *tag;

	/* Global state */
        system_state_t   system_state;           /* ACTDEAD, BOOT, REBOOT,
						    SHUTDOWN, USER, UNDEF    */
        display_state_t  display_state_curr;          /* {LPM_,}{OFF,ON}, DIM,    */
        alarm_ui_state_t alarm_ui_state;         /* OFF, RINGING, VISIBLE    */
        gboolean         call_ringing;
        gboolean         charger_connected;
        inhibit_t        blanking_inhibit_mode;  /* INVALID, OFF,
						    STAY_DIM{,_WITH_CHARGER},
						    STAY_ON{,_WITH_CHARGER}  */
	/* Input arguments */
	gboolean         timed_inhibit;
	/* Expected result */
	gboolean         expected_dimming_inhibited;
	gboolean         expected_blanking_inhibited;

} ut_check_blanking_inhibit_data[] = {
	/* PRIO 1: When in acting dead && no alarm ui is visible && charger is
	 * connected, never inhibit blanking */
	DATA( ACTDEAD, ON, OFF    , 0, 1, INVALID              , 0, 0, 0 ),
	DATA( ACTDEAD, ON, INVALID, 0, 1, INVALID              , 0, 0, 0 ),
	DATA( ACTDEAD, ON, INVALID, 0, 1, INVALID              , 1, 0, 0 ),
	DATA( ACTDEAD, ON, OFF    , 0, 1, OFF                  , 0, 0, 0 ),
	DATA( ACTDEAD, ON, INVALID, 0, 1, OFF                  , 0, 0, 0 ),
	DATA( ACTDEAD, ON, INVALID, 0, 1, OFF                  , 1, 0, 0 ),
	DATA( ACTDEAD, ON, OFF    , 0, 1, STAY_DIM             , 0, 0, 0 ),
	DATA( ACTDEAD, ON, OFF    , 0, 1, STAY_DIM_WITH_CHARGER, 0, 0, 0 ),
	DATA( ACTDEAD, ON, INVALID, 0, 1, STAY_DIM_WITH_CHARGER, 1, 0, 0 ),
	DATA( ACTDEAD, ON, INVALID, 0, 1, STAY_ON              , 0, 0, 0 ),
	DATA( ACTDEAD, ON, INVALID, 0, 1, STAY_ON_WITH_CHARGER , 0, 0, 0 ),
	DATA( ACTDEAD, ON, OFF    , 0, 1, STAY_ON_WITH_CHARGER , 1, 0, 0 ),
	/* PRIO 2: If ringing, always inhibit both blanking and dimming */
	DATA( USER   , ON, OFF    , 1, 1, INVALID              , 0, 1, 1 ),
	DATA( USER   , ON, OFF    , 1, 0, INVALID              , 1, 1, 1 ),
	DATA( USER   , ON, OFF    , 1, 1, OFF                  , 0, 1, 1 ),
	DATA( USER   , ON, OFF    , 1, 0, OFF                  , 1, 1, 1 ),
	/* PRIO 2: If alarm is ringing, always inh. both blanking and dimming */
	DATA( USER   , ON, RINGING, 0, 1, INVALID              , 0, 1, 1 ),
	DATA( USER   , ON, RINGING, 0, 0, INVALID              , 1, 1, 1 ),
	DATA( USER   , ON, RINGING, 0, 1, OFF                  , 0, 1, 1 ),
	DATA( USER   , ON, RINGING, 0, 0, OFF                  , 1, 1, 1 ),
	/* PRIO 2: Do what blanking_inhibit_mode says but do not inhibit dimming
	 * in acting dead */
	DATA( USER   , ON, OFF    , 0, 0, STAY_ON              , 0, 1, 1 ),
	DATA( ACTDEAD, ON, OFF    , 0, 0, STAY_ON              , 0, 0, 1 ),
	DATA( USER   , ON, OFF    , 0, 0, STAY_DIM             , 0, 0, 1 ),
	DATA( USER   , ON, OFF    , 0, 1, STAY_ON_WITH_CHARGER , 0, 1, 1 ),
	/* TODO: this case is actually caught as PRIO 1 - condition could be
	 * simplified */
	DATA( ACTDEAD, ON, OFF    , 0, 1, STAY_ON_WITH_CHARGER , 0, 0, 0 ),
	DATA( USER   , ON, OFF    , 0, 1, STAY_DIM_WITH_CHARGER, 0, 0, 1 ),
	/* PRIO 2: If 'timed' request is issued, always inhibit both */
	DATA( USER   , ON, OFF    , 0, 0, OFF                  , 1, 1, 1 ),
	/* PRIO 3: If 'timed' request expired and there is no other reason to
	 * inhibit, clear both inhibit flags (blank_prevent_timeout_cb_id is
	 * left initialized to 0 during testing) */
	DATA( USER   , ON, OFF    , 0, 0, OFF                  , 0, 0, 0 ),
	/* vim: AlignCtrl =<<<<<<<<<><p00000000010P11111111100 [(,)] */
};
#undef DATA

static const int ut_check_blanking_inhibit_data_count =
	sizeof(ut_check_blanking_inhibit_data)
	/ sizeof(struct ut_check_blanking_inhibit_data);

START_TEST (ut_check_blanking_inhibit)
{
	ck_assert_int_lt(_i, ut_check_blanking_inhibit_data_count);

	struct ut_check_blanking_inhibit_data *const data =
		ut_check_blanking_inhibit_data;

	printf("data: %s\n", data[_i].tag);

	/* Setup global state */
	system_state_pipe.cached_data =
		GINT_TO_POINTER(data[_i].system_state);
	display_state_curr_pipe.cached_data =
		GINT_TO_POINTER(data[_i].display_state_curr);
	alarm_ui_state_pipe.cached_data =
		GINT_TO_POINTER(data[_i].alarm_ui_state);
	call_state_pipe.cached_data = data[_i].call_ringing
		? GINT_TO_POINTER(CALL_STATE_RINGING)
		: GINT_TO_POINTER(CALL_STATE_NONE);
	charger_connected = data[_i].charger_connected;
	blanking_inhibit_mode = data[_i].blanking_inhibit_mode;

	dimming_inhibited = TRUE;
	blanking_inhibited = TRUE;

	/* Call tested function */
	update_blanking_inhibit(data[_i].timed_inhibit);

	/* Evaluate results */
	ck_assert_int_eq(dimming_inhibited,
			 data[_i].expected_dimming_inhibited);
	ck_assert_int_eq(blanking_inhibited,
			 data[_i].expected_blanking_inhibited);

	/* TODO: Verify cancel_blank_prevent() was called (only) if expected */
}
END_TEST

static Suite *ut_display_filter_suite (void)
{
	Suite *s = suite_create ("ut_display_filter");

	TCase *tc_core = tcase_create ("core");
	tcase_add_unchecked_fixture(tc_core, stub_setup, stub_teardown);

	tcase_add_loop_test (tc_core, ut_check_blanking_inhibit,
			     0, ut_check_blanking_inhibit_data_count);

	suite_add_tcase (s, tc_core);

	return s;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	int number_failed;
	Suite *s = ut_display_filter_suite ();
	SRunner *sr = srunner_create (s);
	srunner_run_all (sr, CK_NORMAL);
	number_failed = srunner_ntests_failed (sr);
	srunner_free (sr);
	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
