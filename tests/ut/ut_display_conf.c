#include <check.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

/* Tested module */
#include "../../modules/display.c"

/* ------------------------------------------------------------------------- *
 * STUBS
 * ------------------------------------------------------------------------- */

/*
 * g_access() stub
 */

typedef struct
{
	const gchar *file;
	int mode;
} stub__access_item_t;

static const stub__access_item_t *stub__access = NULL;

EXTERN_STUB (
int, g_access, (const gchar *filename, int mode))
{
	for( size_t i = 0; stub__access[i].file; ++i ) {
		if( strcmp(stub__access[i].file, filename) == 0 ) {
			if( stub__access[i].mode == -1 ) {
				return -1;
			} else {
				return (stub__access[i].mode & mode) == mode ? 0 : -1;
			}
		}
	}

	ck_abort_msg("Unexpected filename: '%s'", filename);

	return 1;
}

/*
 * mce_conf_get_string_list() stub
 */

typedef struct
{
	const gchar *key;
	const gchar **value;
} stub__conf_string_list_item_t;

static const stub__conf_string_list_item_t *stub__conf_string_lists = NULL;

EXTERN_STUB (
gchar **, mce_conf_get_string_list, (const gchar *group,
				     const gchar *key,
				     gsize *length))
{
	ck_assert_str_eq(group, "modules/display");
	ck_assert(length == NULL);

	for( size_t i = 0; stub__conf_string_lists[i].key; ++i ) {
		if( strcmp(stub__conf_string_lists[i].key, key) == 0 ) {
			return g_strdupv((gchar **)stub__conf_string_lists[i].value);
		}
	}

	ck_abort_msg("Unexpected key: '%s'", key);

	return NULL;
}

/* ------------------------------------------------------------------------- *
 * TESTS
 * ------------------------------------------------------------------------- */

START_TEST (ut_check_get_display_type_from_config)
{
	display_type_t expected_display_type;
	const gchar *expected_brightness_output_path;
	const gchar *expected_max_brightness_file;
	gboolean expected_cabc_supported;

	switch( _i ) {
	case 0:
		expected_display_type = DISPLAY_TYPE_GENERIC;
		expected_brightness_output_path = "/brightness_file_1";
		expected_max_brightness_file = "/max_brightness_file_1";
		expected_cabc_supported = FALSE;

		stub__conf_string_lists =
			(const stub__conf_string_list_item_t[]){
			{
				"brightness_dir",
				(const gchar *[]){
					"/brightness_dir_0",
					"/brightness_dir_1",
					"/brightness_dir_2",
					NULL,
				}
			}, {
				"brightness",
				(const gchar *[]){
					"/brightness_file_0",
					"/brightness_file_1",
					"/brightness_file_2",
					NULL,
				}
			}, {
				"max_brightness",
				(const gchar *[]){
					"/max_brightness_file_0",
					"/max_brightness_file_1",
					"/max_brightness_file_2",
					NULL,
				}
			}, {
				NULL,
				NULL
			},
		};

		stub__access = (const stub__access_item_t[]){
			{ "/brightness_dir_0",		-1 },
			{ "/brightness_dir_1",		-1 },
			{ "/brightness_dir_2",		-1 },

			{ "/brightness_file_0",		-1 },
			{ "/brightness_file_1",		R_OK | W_OK },
			{ "/brightness_file_2",		-1 },

			{ "/max_brightness_file_0",	-1 },
			{ "/max_brightness_file_1",	R_OK },
			{ "/max_brightness_file_2",	-1 },
			{ NULL,				-1 },
		};

		break;

	case 1:
		expected_display_type = DISPLAY_TYPE_GENERIC;
		expected_brightness_output_path = "/brightness_dir_3/brightness";
		expected_max_brightness_file = "/brightness_dir_3/max_brightness";
		expected_cabc_supported = FALSE;

		stub__conf_string_lists =
			(const stub__conf_string_list_item_t[]){
			{
				"brightness_dir",
				(const gchar *[]){
					"/brightness_dir_0",
					"/brightness_dir_1",
					"/brightness_dir_2",
					"/brightness_dir_3",
					"/brightness_dir_4",
					NULL,
				}
			}, {
				"brightness",
				(const gchar *[]){
					"/brightness_file_0",
					"/brightness_file_1",
					"/brightness_file_2",
					NULL,
				}
			}, {
				"max_brightness",
				(const gchar *[]){
					"/max_brightness_file_0",
					"/max_brightness_file_1",
					"/max_brightness_file_2",
					NULL,
				}
			}, {
				NULL,
				NULL
			},
		};

		stub__access = (const stub__access_item_t[]){
			{ "/brightness_dir_0",			-1 },

			{ "/brightness_dir_1",			R_OK },
			{ "/brightness_dir_1/brightness",	-1 },
			{ "/brightness_dir_1/max_brightness",	-1 },

			{ "/brightness_dir_2",			R_OK },
			{ "/brightness_dir_2/brightness",	R_OK },
			{ "/brightness_dir_2/max_brightness",	R_OK },

			{ "/brightness_dir_3",			R_OK },
			{ "/brightness_dir_3/brightness",	W_OK },
			{ "/brightness_dir_3/max_brightness",	R_OK },

			{ "/brightness_dir_4",			-1 },

			{ "/brightness_file_0",			-1 },
			{ "/brightness_file_1",			R_OK | W_OK },
			{ "/brightness_file_2",			-1 },

			{ "/max_brightness_file_0",		-1 },
			{ "/max_brightness_file_1",		R_OK },
			{ "/max_brightness_file_2",		-1 },
			{ NULL,					-1 },
		};

		break;

	default:
		ck_assert_msg(FALSE, "Looping out of range");
	};

	cabc_mode_file            = (gchar *)-1; // At the time of writing,
	cabc_available_modes_file = (gchar *)-1; // old data is not free()d.
	cabc_supported            = TRUE;

	display_type_t display_type = DISPLAY_TYPE_UNSET;

	if( !get_display_type_from_config(&display_type) ) {
		ck_abort_msg("get_display_type_from_config() failed");
	}

	ck_assert_int_eq(display_type, expected_display_type);
	ck_assert_str_eq(brightness_output.path, expected_brightness_output_path);
	ck_assert_str_eq(max_brightness_file, expected_max_brightness_file);
	ck_assert(cabc_mode_file == NULL);
	ck_assert(cabc_available_modes_file == NULL);
	ck_assert_int_eq(cabc_supported, expected_cabc_supported);
}
END_TEST

static Suite *ut_display_suite (void)
{
	Suite *s = suite_create ("ut_display");

	TCase *tc_core = tcase_create ("core");
	tcase_add_loop_test (tc_core, ut_check_get_display_type_from_config, 0,
			     2);
	suite_add_tcase (s, tc_core);

	return s;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	int number_failed;
	Suite *s = ut_display_suite ();
	SRunner *sr = srunner_create (s);
	srunner_run_all (sr, CK_NORMAL);
	number_failed = srunner_ntests_failed (sr);
	srunner_free (sr);
	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
