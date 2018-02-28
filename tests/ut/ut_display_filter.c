#include <check.h>
#include <glib.h>

#include "common.h"

/* Tested module */
#include "../../modules/display.c"

/* ------------------------------------------------------------------------- *
 * STUBS
 * ------------------------------------------------------------------------- */

static submode_t stub__submode = MCE_SUBMODE_NORMAL;

EXTERN_STUB (
submode_t, mce_get_submode_int32, (void))
{
	return stub__submode;
}

/* Stub init/cleanup */

static void stub_setup(void)
{
	call_state_pipe.cached_data = GINT_TO_POINTER(CALL_STATE_NONE);
}

static void stub_teardown(void)
{
}

/* ------------------------------------------------------------------------- *
 * TESTS
 * ------------------------------------------------------------------------- */

#define DATA(lpm, call, prox, tklock, malf, res) {                  \
        "{ "#lpm", "#call", "#prox", "#tklock", "#malf", "#res" }", \
        lpm, call, prox, tklock, malf, res }
static struct ut_check_is_dismiss_lpm_enabled_data
{
	const gchar  *tag;

	/* Global state */
	gboolean     use_low_power_mode;
	call_state_t call_state;
	gboolean     proximity_tklock_submode;
	gboolean     tklock_submode;
	gboolean     malf_submode;

	/* Expected result */
	gboolean     expected_result;

} ut_check_is_dismiss_lpm_enabled_data[] = {
	DATA( FALSE, CALL_STATE_NONE   , FALSE, FALSE, FALSE, FALSE ),
	DATA( TRUE , CALL_STATE_NONE   , FALSE, FALSE, FALSE, FALSE ),
	DATA( FALSE, CALL_STATE_RINGING, FALSE, FALSE, FALSE, FALSE ),
	DATA( TRUE , CALL_STATE_RINGING, FALSE, FALSE, FALSE, TRUE  ),
	DATA( FALSE, CALL_STATE_ACTIVE , FALSE, FALSE, FALSE, FALSE ),
	DATA( TRUE , CALL_STATE_ACTIVE , FALSE, FALSE, FALSE, TRUE  ),
	DATA( TRUE , CALL_STATE_SERVICE, FALSE, FALSE, FALSE, FALSE ),
	DATA( TRUE , CALL_STATE_RINGING, TRUE , FALSE, FALSE, TRUE  ),
	DATA( TRUE , CALL_STATE_RINGING, FALSE, TRUE , FALSE, FALSE ),
	DATA( TRUE , CALL_STATE_RINGING, TRUE , TRUE , FALSE, TRUE  ),
	DATA( TRUE , CALL_STATE_NONE   , TRUE , TRUE , TRUE , TRUE  ),
	/* vim: AlignCtrl =<<<<<<><p00000010P11111100 [(,)] */
};
#undef DATA

static const int ut_check_is_dismiss_lpm_enabled_data_count =
	sizeof(ut_check_is_dismiss_lpm_enabled_data)
	/ sizeof(struct ut_check_is_dismiss_lpm_enabled_data);

START_TEST (ut_check_is_dismiss_lpm_enabled)
{
	ck_assert_int_lt(_i, ut_check_is_dismiss_lpm_enabled_data_count);

	struct ut_check_is_dismiss_lpm_enabled_data *const data =
		ut_check_is_dismiss_lpm_enabled_data;

	printf("data: %s\n", data[_i].tag);

	use_low_power_mode = data[_i].use_low_power_mode;
	call_state_pipe.cached_data = GINT_TO_POINTER(data[_i].call_state);
	if( data[_i].proximity_tklock_submode )
		stub__submode |= MCE_SUBMODE_PROXIMITY_TKLOCK;
	if( data[_i].tklock_submode )
		stub__submode |= MCE_SUBMODE_TKLOCK;
	if( data[_i].malf_submode )
		stub__submode |= MCE_SUBMODE_MALF;

	ck_assert_int_eq(is_dismiss_low_power_mode_enabled(),
			 data[_i].expected_result);
}
END_TEST

#define DATA(sys, trans, alarm, call, lpm, in1, in2, out) {            \
        "{ "#sys", "#trans", "#alarm", "#call", "#lpm", "#in1          \
        ", "#in2", "#out" }",                                          \
        MCE_STATE_##sys, trans, MCE_ALARM_UI_##alarm##_INT32, call,    \
        lpm, MCE_DISPLAY_##in1, MCE_DISPLAY_##in2, MCE_DISPLAY_##out }
static struct ut_check_display_state_filter_data
{
	const gchar      *tag;

	/* Global state */
	system_state_t   system_state;
	gboolean         transition_submode;
	alarm_ui_state_t alarm_ui_state;
	gboolean         call_ringing;
	gboolean         lpm_enabled;

	/* Input arguments */
	display_state_t  input1;
	display_state_t  input2;

	/* Expected result */
	display_state_t  expected_output;

} ut_check_display_state_filter_data[] = {
	/* Ignore display-on requests during transition to shutdown
	 * and reboot, and when system state is unknown */
	DATA( SHUTDOWN, 1, OFF    , 0, 0, OFF, ON     , OFF     ),
	DATA( REBOOT  , 1, OFF    , 0, 0, OFF, ON     , OFF     ),
	DATA( UNDEF   , 1, OFF    , 0, 0, OFF, ON     , OFF     ),
	/* Do not ignore display-on request during transition when in acting
	 * dead */
	DATA( SHUTDOWN, 1, RINGING, 0, 0, OFF, ON     , OFF     ),
	DATA( REBOOT  , 1, VISIBLE, 0, 0, OFF, ON     , OFF     ),
	DATA( ACTDEAD , 1, OFF,     0, 0, OFF, ON     , ON      ),
	DATA( ACTDEAD , 1, RINGING, 0, 0, OFF, ON     , ON      ),
	DATA( ACTDEAD , 1, VISIBLE, 0, 0, OFF, ON     , ON      ),
	/* Above mentioned only applies during transition */
	DATA( SHUTDOWN, 0, OFF    , 0, 0, OFF, ON     , ON      ),
	DATA( REBOOT  , 0, OFF    , 0, 0, OFF, ON     , ON      ),
	DATA( ACTDEAD , 0, OFF    , 0, 0, OFF, ON     , ON      ),
	DATA( UNDEF   , 0, OFF    , 0, 0, OFF, ON     , OFF     ),
	/* Above mentione only applies for transitions from display OFF */
	DATA( SHUTDOWN, 1, OFF    , 0, 0, ON , OFF    , OFF     ),
	DATA( REBOOT  , 1, OFF    , 0, 0, ON , OFF    , OFF     ),
	DATA( ACTDEAD , 1, OFF    , 0, 0, ON , OFF    , OFF     ),
	DATA( UNDEF   , 1, OFF    , 0, 0, ON , OFF    , OFF     ),
	/* If we don't use low power mode, use OFF instead */
	DATA( USER    , 0, OFF    , 0, 1, ON , LPM_ON , LPM_ON  ),
	DATA( USER    , 0, OFF    , 0, 1, ON , LPM_OFF, LPM_OFF ),
	DATA( USER    , 0, OFF    , 0, 0, ON , LPM_ON , OFF     ),
	DATA( USER    , 0, OFF    , 0, 0, ON , LPM_OFF, OFF     ),
	/* If we're in user state, use LPM instead of OFF */
	DATA( USER    , 0, OFF    , 0, 1, ON , OFF    , LPM_ON  ),
	DATA( USER    , 0, OFF    , 0, 0, ON , OFF    , OFF     ),
	/* vim: AlignCtrl =<<<<<<<<><p0000000010P1111111100 [(,)] */
#undef DATA
};

static const int ut_check_display_state_filter_data_count =
	sizeof(ut_check_display_state_filter_data)
	/ sizeof(struct ut_check_display_state_filter_data);

START_TEST (ut_check_display_state_filter)
{
	ck_assert_int_lt(_i, ut_check_display_state_filter_data_count);

	struct ut_check_display_state_filter_data *const data =
		ut_check_display_state_filter_data;

	printf("data: %s\n", data[_i].tag);

	/* "initialize" display_state_filter()'s internal static
	 * cached_display_state to data.input1 */
	const gpointer input1_filtered =
		display_state_filter(GINT_TO_POINTER(data[_i].input1));

	ck_assert_int_eq(GPOINTER_TO_INT(input1_filtered), data[_i].input1);

	system_state_pipe.cached_data =
		GINT_TO_POINTER(data[_i].system_state);
	alarm_ui_state_pipe.cached_data =
		GINT_TO_POINTER(data[_i].alarm_ui_state);
	call_state_pipe.cached_data = data[_i].call_ringing
		? GINT_TO_POINTER(CALL_STATE_RINGING)
		: GINT_TO_POINTER(CALL_STATE_NONE);
	stub__submode |= data[_i].transition_submode
		? MCE_SUBMODE_TRANSITION
		: 0;
	low_power_mode_supported = use_low_power_mode =
		data[_i].lpm_enabled;

	const gpointer input2_filtered =
		display_state_filter(GINT_TO_POINTER(data[_i].input2));

	ck_assert_int_eq(GPOINTER_TO_INT(input2_filtered),
			 data[_i].expected_output);
	// The ugly hack has been commented out in display_state_filter()
	//ck_assert_int_eq(GPOINTER_TO_INT(display_state_curr_pipe.cached_data),
	//		 data[_i].expected_output);
}
END_TEST

static Suite *ut_display_filter_suite (void)
{
	Suite *s = suite_create ("ut_display_filter");

	TCase *tc_core = tcase_create ("core");
	tcase_add_unchecked_fixture(tc_core, stub_setup, stub_teardown);

	tcase_add_loop_test (tc_core, ut_check_is_dismiss_lpm_enabled,
			     0, ut_check_is_dismiss_lpm_enabled_data_count);
	tcase_add_loop_test (tc_core, ut_check_display_state_filter,
			     0, ut_check_display_state_filter_data_count);

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
