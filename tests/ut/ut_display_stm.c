#include <check.h>
#include <glib.h>

#include "common.h"

/* Tested module */
#include "../../modules/display.c"

/* ------------------------------------------------------------------------- *
 * STUBS
 * ------------------------------------------------------------------------- */

EXTERN_DUMMY_STUB (
gboolean, mce_write_number_string_to_file, (output_state_t *output,
					    const gulong number));

EXTERN_DUMMY_STUB (
DBusConnection *, dbus_connection_get, (void));

EXTERN_DUMMY_STUB (
gconstpointer, datapipe_exec_full, (datapipe_t *const datapipe,
				    gpointer indata,
				    const datapipe_use_t use_cache,
				    const datapipe_cache_t cache_indata));

/* mce-sensorfw.c stubs */

EXTERN_STUB (
void, mce_sensorfw_suspend, (void))
{
}

EXTERN_STUB (
void, mce_sensorfw_resume, (void))
{
}

/* libwakelock stub */

static GHashTable *stub__wakelock_locks = NULL;

EXTERN_STUB (
void, wakelock_lock, (const char *name, long long ns))
{
	ck_assert(!g_hash_table_lookup_extended(stub__wakelock_locks, name,
						NULL, NULL));
	ck_assert_int_eq(ns, -1);

	g_hash_table_insert(stub__wakelock_locks,
			    g_strdup(name), NULL);
}

EXTERN_STUB (
void, wakelock_unlock, (const char *name))
{
	ck_assert(g_hash_table_lookup_extended(stub__wakelock_locks, name,
					       NULL, NULL));

	g_hash_table_remove(stub__wakelock_locks, name);
}

static bool stub__wakelock_locked(const char *name)
{
	if( name == NULL )
		return g_hash_table_size(stub__wakelock_locks) != 0;
	else
		return g_hash_table_lookup_extended(stub__wakelock_locks, name,
						    NULL, NULL);
}

static ut_tristate_t stub__wakelock_suspend_allowed_wanted = UT_TRISTATE_UNDEF;

EXTERN_STUB (
void, wakelock_allow_suspend, (void))
{
	stub__wakelock_suspend_allowed_wanted = UT_TRISTATE_TRUE;
}

EXTERN_STUB (
void, wakelock_block_suspend, (void))
{
	stub__wakelock_suspend_allowed_wanted = UT_TRISTATE_FALSE;
}

/* suspend_allow_state() stub
 *
 * Keep in sync with real suspend_allow_state() -- only examine variables which
 * are touched during stm execution.
 */
LOCAL_STUB (
int, suspend_allow_state, (void))
{
#if 0
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
#endif
	bool block_late  = false;
	bool block_early = false;
#if 0
	/* no late suspend in ACTDEAD etc */
	if( system_state != MCE_SYSTEM_STATE_USER )
		block_late = true;

	/* no late suspend during bootup */
	if( desktop_ready_id || !init_done )
		block_late = true;

	/* no late suspend during shutdown */
	if( shutdown_started )
		block_late = true;

	/* no more suspend at module unload */
	if( module_unloading )
		block_early = true;
#endif
	/* do not suspend while ui side might still be drawing */
	if( renderer_ui_state != RENDERER_DISABLED )
		block_early = true;

	/* adjust based on setting */
	switch( suspend_policy ) {
	case SUSPEND_POLICY_DISABLED:
		block_early = true;
		break;

	case SUSPEND_POLICY_EARLY_ONLY:
		block_late = true;
		break;

	default:
	case SUSPEND_POLICY_ENABLED:
		break;
	}

	return block_early ? 0 : block_late ? 1 : 2;
}

/* display_state_curr stub */

static display_state_t stub__display_state = MCE_DISPLAY_UNDEF;

static bool stub__display_state_pre_trigger_called = false;

LOCAL_STUB (
void, display_state_pre_trigger, (display_state_t prev_state,
				  display_state_t display_state_curr))
{
	(void)prev_state;
	(void)display_state_curr;
	stub__display_state_pre_trigger_called = true;
}

static bool stub__display_state_post_trigger_called = false;

LOCAL_STUB (
void, display_state_post_trigger, (display_state_t prev_state,
				   display_state_t display_state_curr))
{
	(void)prev_state;
	stub__display_state = display_state_curr;
	stub__display_state_post_trigger_called = true;
}

/* renderer_state stub */

static renderer_state_t stub__renderer_ui_state_wanted = RENDERER_UNKNOWN;

LOCAL_STUB (
gboolean, renderer_set_state, (renderer_state_t state))
{
	renderer_ui_state = RENDERER_UNKNOWN;
	stub__renderer_ui_state_wanted = state;
	return TRUE;
}

/* Stub init/cleanup */

static void stub_setup(void)
{
	stub__wakelock_locks = g_hash_table_new_full(g_str_hash, g_str_equal,
						     g_free, NULL);
}

static void stub_teardown(void)
{
	g_hash_table_destroy(stub__wakelock_locks), stub__wakelock_locks = NULL;
}

/* ------------------------------------------------------------------------- *
 * TESTS
 * ------------------------------------------------------------------------- */

START_TEST (ut_check_initial_state)
{
	ck_assert_int_eq(dstate, STM_UNSET);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_UNDEF);
	ck_assert_int_eq(stm_want, MCE_DISPLAY_UNDEF);
	ck_assert_int_eq(stm_next, MCE_DISPLAY_UNDEF);
}
END_TEST

START_TEST (ut_check_stay_initial)
{
	stm_rethink();

	ck_assert_int_eq(dstate, STM_UNSET);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_UNDEF);
	/* TODO: shouldn't we stay unlocked? */
	//ck_assert(!stub__wakelock_locked(NULL));
}
END_TEST

START_TEST (ut_check_undef_to_on)
{
	stm_want = MCE_DISPLAY_ON;

	stm_lipstick_on_dbus = true;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_RENDERER_WAIT_START);
	ck_assert_int_eq(renderer_ui_state, RENDERER_UNKNOWN);
	ck_assert_int_eq(stub__renderer_ui_state_wanted, RENDERER_ENABLED);

	renderer_ui_state = stub__renderer_ui_state_wanted,
			  stub__renderer_ui_state_wanted = RENDERER_UNKNOWN;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_STAY_POWER_ON);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_ON);
	ck_assert(stub__wakelock_locked("mce_display_on"));
}
END_TEST

START_TEST (ut_check_undef_to_on_no_lipstick)
{
	stm_want = MCE_DISPLAY_ON;

	stm_lipstick_on_dbus = false;

	stm_rethink();

	/* stm_renderer_disable() was not called */
	ck_assert_int_eq(stub__renderer_ui_state_wanted, RENDERER_UNKNOWN);

	ck_assert_int_eq(dstate, STM_STAY_POWER_ON);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_ON);
	ck_assert(stub__wakelock_locked("mce_display_on"));
}
END_TEST

START_TEST (ut_check_on_to_off)
{
	stm_curr = stm_next = MCE_DISPLAY_ON;
	stm_want = MCE_DISPLAY_OFF;
	dstate = STM_STAY_POWER_ON;

	stm_lipstick_on_dbus = true;
	stm_enable_rendering_needed = false;
	waitfb.thread = (pthread_t)-1;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_RENDERER_WAIT_STOP);
	ck_assert_int_eq(renderer_ui_state, RENDERER_UNKNOWN);
	ck_assert_int_eq(stub__renderer_ui_state_wanted, RENDERER_DISABLED);

	renderer_ui_state = stub__renderer_ui_state_wanted,
			  stub__renderer_ui_state_wanted = RENDERER_UNKNOWN;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_WAIT_SUSPEND);
	ck_assert_int_eq(stub__wakelock_suspend_allowed_wanted, UT_TRISTATE_TRUE);

	waitfb.suspended = true;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_STAY_POWER_OFF);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_OFF);
	ck_assert(!stub__wakelock_locked("mce_display_on"));
}
END_TEST

START_TEST (ut_check_on_to_off_no_lipstick)
{
	stm_curr = stm_next = MCE_DISPLAY_ON;
	stm_want = MCE_DISPLAY_OFF;
	dstate = STM_STAY_POWER_ON;

	stm_lipstick_on_dbus = false;
	waitfb.thread = (pthread_t)-1;

	stm_rethink();

	/* stm_renderer_disable() was not called */
	ck_assert_int_eq(stub__renderer_ui_state_wanted, RENDERER_UNKNOWN);
	/* stm_suspend_start() was not called */
	ck_assert_int_eq(stub__wakelock_suspend_allowed_wanted,
			 UT_TRISTATE_UNDEF);

	ck_assert_int_eq(dstate, STM_STAY_LOGICAL_OFF);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_OFF);
	ck_assert(!stub__wakelock_locked("mce_display_on"));
}
END_TEST

START_TEST (ut_check_on_to_off_suspend_early_only)
{
	stm_curr = stm_next = MCE_DISPLAY_ON;
	stm_want = MCE_DISPLAY_OFF;
	dstate = STM_STAY_POWER_ON;

	stm_lipstick_on_dbus = true;
	stm_enable_rendering_needed = false;
	waitfb.thread = (pthread_t)-1;
	suspend_policy = SUSPEND_POLICY_EARLY_ONLY;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_RENDERER_WAIT_STOP);
	ck_assert_int_eq(renderer_ui_state, RENDERER_UNKNOWN);
	ck_assert_int_eq(stub__renderer_ui_state_wanted, RENDERER_DISABLED);

	renderer_ui_state = stub__renderer_ui_state_wanted,
			  stub__renderer_ui_state_wanted = RENDERER_UNKNOWN;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_WAIT_SUSPEND);
	ck_assert_int_eq(stub__wakelock_suspend_allowed_wanted, UT_TRISTATE_TRUE);

	waitfb.suspended = true;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_STAY_POWER_OFF);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_OFF);
	ck_assert(stub__wakelock_locked("mce_display_on"));
}
END_TEST

START_TEST (ut_check_on_to_off_suspend_disabled)
{
	stm_curr = stm_next = MCE_DISPLAY_ON;
	stm_want = MCE_DISPLAY_OFF;
	dstate = STM_STAY_POWER_ON;

	stm_lipstick_on_dbus = true;
	stm_enable_rendering_needed = false;
	waitfb.thread = (pthread_t)-1;
	suspend_policy = SUSPEND_POLICY_DISABLED;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_RENDERER_WAIT_STOP);
	ck_assert_int_eq(renderer_ui_state, RENDERER_UNKNOWN);
	ck_assert_int_eq(stub__renderer_ui_state_wanted, RENDERER_DISABLED);

	renderer_ui_state = stub__renderer_ui_state_wanted,
			  stub__renderer_ui_state_wanted = RENDERER_UNKNOWN;

	stm_rethink();

	/* stm_suspend_start() was not called */
	ck_assert_int_eq(stub__wakelock_suspend_allowed_wanted,
			 UT_TRISTATE_UNDEF);

	ck_assert_int_eq(dstate, STM_STAY_LOGICAL_OFF);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_OFF);
	/* TODO: shouldn't we get locked? Probably not as suspend is disabled. */
	//ck_assert(stub__wakelock_locked("mce_display_on"));
}
END_TEST

START_TEST (ut_check_off_to_on)
{
	stm_curr = stm_next = MCE_DISPLAY_OFF;
	stm_want = MCE_DISPLAY_ON;
	dstate = STM_STAY_POWER_OFF;

	stm_lipstick_on_dbus = true;
	waitfb.thread = (pthread_t)-1;
	waitfb.suspended = true;
	renderer_ui_state = RENDERER_DISABLED;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_WAIT_RESUME);
	ck_assert_int_eq(stub__wakelock_suspend_allowed_wanted,
			 UT_TRISTATE_FALSE);

	waitfb.suspended = false;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_RENDERER_WAIT_START);
	ck_assert_int_eq(renderer_ui_state, RENDERER_UNKNOWN);
	ck_assert_int_eq(stub__renderer_ui_state_wanted, RENDERER_ENABLED);

	renderer_ui_state = stub__renderer_ui_state_wanted,
			  stub__renderer_ui_state_wanted = RENDERER_UNKNOWN;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_STAY_POWER_ON);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_ON);
	ck_assert(stub__wakelock_locked("mce_display_on"));
}
END_TEST

START_TEST (ut_check_off_to_on_suspend_disabled)
{
	stm_curr = stm_next = MCE_DISPLAY_OFF;
	stm_want = MCE_DISPLAY_ON;
	dstate = STM_STAY_LOGICAL_OFF;

	stm_lipstick_on_dbus = true;
	waitfb.thread = (pthread_t)-1;
	suspend_policy = SUSPEND_POLICY_DISABLED;
	renderer_ui_state = RENDERER_DISABLED;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_RENDERER_WAIT_START);
	ck_assert_int_eq(renderer_ui_state, RENDERER_UNKNOWN);
	ck_assert_int_eq(stub__renderer_ui_state_wanted, RENDERER_ENABLED);

	renderer_ui_state = stub__renderer_ui_state_wanted,
			  stub__renderer_ui_state_wanted = RENDERER_UNKNOWN;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_STAY_POWER_ON);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_ON);
	/* TODO: shouldn't we get locked? Probably not as suspend is disabled. */
	//ck_assert(stub__wakelock_locked("mce_display_on"));
}
END_TEST

START_TEST (ut_check_enable_suspend_while_off)
{
	stm_curr = stm_next = MCE_DISPLAY_OFF;
	stm_want = MCE_DISPLAY_UNDEF;
	dstate = STM_STAY_LOGICAL_OFF;

	stm_lipstick_on_dbus = true;
	waitfb.thread = (pthread_t)-1;
	renderer_ui_state = RENDERER_DISABLED;

	suspend_policy = MCE_DEFAULT_USE_AUTOSUSPEND; /* The change */

	stm_rethink();

	ck_assert_int_eq(dstate, STM_WAIT_SUSPEND);
	ck_assert_int_eq(stub__wakelock_suspend_allowed_wanted, UT_TRISTATE_TRUE);

	waitfb.suspended = true;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_STAY_POWER_OFF);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_OFF);
	ck_assert(!stub__wakelock_locked("mce_display_on"));
}
END_TEST

START_TEST (ut_check_enable_early_suspend_while_off)
{
	stm_curr = stm_next = MCE_DISPLAY_OFF;
	stm_want = MCE_DISPLAY_UNDEF;
	dstate = STM_STAY_LOGICAL_OFF;

	stm_lipstick_on_dbus = true;
	waitfb.thread = (pthread_t)-1;
	renderer_ui_state = RENDERER_DISABLED;

	suspend_policy = SUSPEND_POLICY_EARLY_ONLY; /* The change */

	stm_rethink();

	ck_assert_int_eq(dstate, STM_WAIT_SUSPEND);
	ck_assert_int_eq(stub__wakelock_suspend_allowed_wanted, UT_TRISTATE_TRUE);

	waitfb.suspended = true;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_STAY_POWER_OFF);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_OFF);
	ck_assert(stub__wakelock_locked("mce_display_on"));
}
END_TEST

START_TEST (ut_check_disable_suspend_while_off)
{
	stm_curr = stm_next = MCE_DISPLAY_OFF;
	stm_want = MCE_DISPLAY_UNDEF;
	dstate = STM_STAY_POWER_OFF;

	stm_lipstick_on_dbus = true;
	stm_enable_rendering_needed = false;
	waitfb.thread = (pthread_t)-1;
	waitfb.suspended = true;
	renderer_ui_state = RENDERER_DISABLED;

	suspend_policy = SUSPEND_POLICY_DISABLED; /* The change */

	stm_rethink();

	ck_assert_int_eq(dstate, STM_STAY_LOGICAL_OFF);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_OFF);
	/* TODO: suspend is disabled - shouldn't wakelock_block_suspend() be
	 * called? */
	//ck_assert_int_eq(stub__wakelock_suspend_allowed_wanted,
	//		 UT_TRISTATE_FALSE);
	/* TODO: suspend is disabled - shouldn't the lock be released? */
	//ck_assert(!stub__wakelock_locked("mce_display_on"));
}
END_TEST

START_TEST (ut_check_disable_late_suspend_while_off)
{
	stm_curr = stm_next = MCE_DISPLAY_OFF;
	stm_want = MCE_DISPLAY_UNDEF;
	dstate = STM_STAY_POWER_OFF;

	stm_lipstick_on_dbus = true;
	waitfb.thread = (pthread_t)-1;
	waitfb.suspended = true;
	renderer_ui_state = RENDERER_DISABLED;

	suspend_policy = SUSPEND_POLICY_EARLY_ONLY; /* The change */

	stm_rethink();

	ck_assert_int_eq(dstate, STM_STAY_POWER_OFF);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_OFF);
	ck_assert(stub__wakelock_locked("mce_display_on"));
}
END_TEST

START_TEST (ut_check_off_to_on_renderer_fail)
{
	stm_curr = stm_next = MCE_DISPLAY_OFF;
	stm_want = MCE_DISPLAY_ON;
	dstate = STM_STAY_POWER_OFF;

	stm_lipstick_on_dbus = true;
	waitfb.thread = (pthread_t)-1;
	waitfb.suspended = true;
	renderer_ui_state = RENDERER_DISABLED;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_WAIT_RESUME);
	ck_assert_int_eq(stub__wakelock_suspend_allowed_wanted,
			 UT_TRISTATE_FALSE);

	waitfb.suspended = false;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_RENDERER_WAIT_START);
	ck_assert_int_eq(renderer_ui_state, RENDERER_UNKNOWN);
	ck_assert_int_eq(stub__renderer_ui_state_wanted, RENDERER_ENABLED);

	/* Pretend rendered failed to start */
	renderer_ui_state = RENDERER_DISABLED,
			  stub__renderer_ui_state_wanted = RENDERER_UNKNOWN;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_STAY_POWER_ON);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_ON);
	ck_assert(stub__wakelock_locked("mce_display_on"));
}
END_TEST

START_TEST (ut_check_on_to_off_renderer_fail)
{
	stm_curr = stm_next = MCE_DISPLAY_ON;
	stm_want = MCE_DISPLAY_OFF;
	dstate = STM_STAY_POWER_ON;

	stm_lipstick_on_dbus = true;
	stm_enable_rendering_needed = false;
	waitfb.thread = (pthread_t)-1;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_RENDERER_WAIT_STOP);
	ck_assert_int_eq(renderer_ui_state, RENDERER_UNKNOWN);
	ck_assert_int_eq(stub__renderer_ui_state_wanted, RENDERER_DISABLED);

	/* Pretent renderer failed to stop */
	renderer_ui_state = RENDERER_ENABLED,
			  stub__renderer_ui_state_wanted = RENDERER_UNKNOWN;

	stm_rethink();

	/* Note: suspend is disabled while RENDERER_ENABLED */

	ck_assert_int_eq(dstate, STM_STAY_POWER_ON);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_ON);
	ck_assert(!stub__wakelock_locked("mce_display_on"));
}
END_TEST

START_TEST (ut_check_off_to_on_no_lipstick)
{
	stm_curr = stm_next = MCE_DISPLAY_OFF;
	stm_want = MCE_DISPLAY_ON;
	dstate = STM_STAY_POWER_OFF;

	stm_lipstick_on_dbus = false;
	waitfb.thread = (pthread_t)-1;
	waitfb.suspended = true;
	renderer_ui_state = RENDERER_DISABLED;

	stm_rethink();

	ck_assert_int_eq(dstate, STM_WAIT_RESUME);
	ck_assert_int_eq(stub__wakelock_suspend_allowed_wanted,
			 UT_TRISTATE_FALSE);

	waitfb.suspended = false;

	stm_rethink();

	/* When lipstick is not available, renderer_ui_state is set to
	 * RENDERER_ENABLED without calling renderer_set_state() */
	ck_assert_int_eq(stub__renderer_ui_state_wanted, RENDERER_UNKNOWN);

	ck_assert_int_eq(dstate, STM_STAY_POWER_ON);
	ck_assert_int_eq(stm_curr, MCE_DISPLAY_ON);
	ck_assert(stub__wakelock_locked("mce_display_on"));

	/* TODO: this test shows it if not neccesary to check
	 * stm_lipstick_on_dbus in stm_rethink_step(), case
	 * STM_RENDERER_INIT_START -- this would bechecked in
	 * stm_renderer_enable() anyway */
}
END_TEST

static Suite *ut_display_stm_suite (void)
{
	Suite *s = suite_create ("ut_display_stm");

	TCase *tc_core = tcase_create ("core");
	tcase_add_unchecked_fixture(tc_core, stub_setup, stub_teardown);

	tcase_add_test (tc_core, ut_check_initial_state);
	tcase_add_test (tc_core, ut_check_stay_initial);
	tcase_add_test (tc_core, ut_check_undef_to_on);
	tcase_add_test (tc_core, ut_check_undef_to_on_no_lipstick);
	tcase_add_test (tc_core, ut_check_on_to_off);
	tcase_add_test (tc_core, ut_check_on_to_off_no_lipstick);
	tcase_add_test (tc_core, ut_check_on_to_off_suspend_early_only);
	tcase_add_test (tc_core, ut_check_on_to_off_suspend_disabled);
	tcase_add_test (tc_core, ut_check_off_to_on);
	tcase_add_test (tc_core, ut_check_off_to_on_suspend_disabled);
	tcase_add_test (tc_core, ut_check_enable_suspend_while_off);
	tcase_add_test (tc_core, ut_check_enable_early_suspend_while_off);
	tcase_add_test (tc_core, ut_check_disable_suspend_while_off);
	tcase_add_test (tc_core, ut_check_disable_late_suspend_while_off);
	tcase_add_test (tc_core, ut_check_off_to_on_renderer_fail);
	tcase_add_test (tc_core, ut_check_on_to_off_renderer_fail);
	tcase_add_test (tc_core, ut_check_off_to_on_no_lipstick);

	suite_add_tcase (s, tc_core);

	return s;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	int number_failed;
	Suite *s = ut_display_stm_suite ();
	SRunner *sr = srunner_create (s);
	srunner_run_all (sr, CK_NORMAL);
	number_failed = srunner_ntests_failed (sr);
	srunner_free (sr);
	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
