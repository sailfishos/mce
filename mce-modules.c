/**
 * @file mce-modules.c
 * Module handling for MCE
 * <p>
 * Copyright © 2007-2009 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 *
 * mce is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * mce is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mce.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <glib.h>
#include <gmodule.h>

#include <stdio.h>			/* fprintf(), stdout */
#include <string.h>			/* strcmp() */

#include "mce.h"			/* module_info_struct */
#include "mce-modules.h"

#include "mce-log.h"			/* mce_log(), LL_* */
#include "mce-conf.h"			/* mce_conf_get_string(),
					 * mce_conf_get_string_list()
					 */

/** List of all loaded modules */
static GSList *modules = NULL;

/**
 * Dump information about mce modules to stdout
 */
void mce_modules_dump_info(void)
{
	GModule *module;
	gint i;

	for (i = 0; (module = g_slist_nth_data(modules, i)) != NULL; i++) {
		const gchar *modulename = g_module_name(module);
		module_info_struct *modinfo;
		gchar *tmp = NULL;
		gpointer mip;

		fprintf(stdout,
			_("\n"
			  "Module: %s\n"),
			modulename);

		if (g_module_symbol(module,
				    "module_info",
				    &mip) == FALSE) {
			fprintf(stdout,
				"        %-32s\n",
				"module lacks information");
			continue;
		}

		modinfo = (module_info_struct *)mip;

		fprintf(stdout,
			"        %-32s %s\n",
			_("name:"),
			modinfo->name ? modinfo->name : _("<undefined>"));

		if (modinfo->depends != NULL)
			tmp = g_strjoinv(",", (gchar **)(modinfo->depends));

		fprintf(stdout,
			"        %-32s %s\n",
			_("depends:"),
			tmp ? tmp : "");

		g_free(tmp);
		tmp = NULL;

		if (modinfo->recommends != NULL)
			tmp = g_strjoinv(",", (gchar **)(modinfo->recommends));

		fprintf(stdout,
			"        %-32s %s\n",
			_("recommends:"),
			tmp ? tmp : "");

		g_free(tmp);
		tmp = NULL;

		if (modinfo->provides != NULL)
			tmp = g_strjoinv(",", (gchar **)(modinfo->provides));

		fprintf(stdout,
			"        %-32s %s\n",
			_("provides:"),
			tmp ? tmp : "");

		g_free(tmp);
		tmp = NULL;

		if (modinfo->enhances != NULL)
			tmp = g_strjoinv(",", (gchar **)(modinfo->enhances));

		fprintf(stdout,
			"        %-32s %s\n",
			_("enhances:"),
			tmp ? tmp : "");

		g_free(tmp);
		tmp = NULL;

		if (modinfo->conflicts != NULL)
			tmp = g_strjoinv(",", (gchar **)(modinfo->conflicts));

		fprintf(stdout,
			"        %-32s %s\n",
			_("conflicts:"),
			tmp ? tmp : "");

		g_free(tmp);
		tmp = NULL;

		if (modinfo->replaces != NULL)
			tmp = g_strjoinv(",", (gchar **)(modinfo->replaces));

		fprintf(stdout,
			"        %-32s %s\n",
			_("replaces:"),
			tmp ? tmp : "");

		g_free(tmp);

		fprintf(stdout,
			"        %-32s %d\n",
			_("priority:"),
			modinfo->priority);
	}
}

/** Construct path for named mce plugin
 *
 * @param directory Location of the plugin
 * @module_name Name of the plugin
 *
 * @return Path to shared object
 */
static gchar * mce_modules_build_path(const gchar *directory,
				      const gchar *module_name)
{
	return g_strdup_printf("%s/%s.so", directory, module_name);
}

/**
 * Init function for the mce-modules component
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_modules_init(void)
{
	gchar **modlist = NULL;
	gsize length;
	gchar *path = NULL;

	/* Get the module path */
	path = mce_conf_get_string(MCE_CONF_MODULES_GROUP,
				   MCE_CONF_MODULES_PATH,
				   DEFAULT_MCE_MODULE_PATH);

	/* Get the list modules to load */
	modlist = mce_conf_get_string_list(MCE_CONF_MODULES_GROUP,
					   MCE_CONF_MODULES_MODULES,
					   &length);

	if (modlist != NULL) {
		gint i;

		for (i = 0; modlist[i]; i++) {
			GModule *module;
			gchar *tmp = mce_modules_build_path(path, modlist[i]);

			mce_log(LL_INFO,
				"Loading module: %s from %s",
				modlist[i], path);

			if ((module = g_module_open(tmp, 0)) != NULL) {
				/* XXX: check dependencies, conflicts, et al */
				modules = g_slist_prepend(modules, module);
			} else {
				const char *err = g_module_error();
				mce_log(LL_ERR, "%s", err ?: "unknown error");
				mce_log(LL_ERR,
					"Failed to load module: %s; skipping",
					modlist[i]);
			}

			g_free(tmp);
		}

		g_strfreev(modlist);
	}

	g_free(path);

	return TRUE;
}

/**
 * Exit function for the mce-modules component
 */
void mce_modules_exit(void)
{
	GModule *module;
	gint i;

	if (modules != NULL) {
		for (i = 0; (module = g_slist_nth_data(modules, i)) != NULL; i++) {
			g_module_close(module);
		}

		g_slist_free(modules);
		modules = NULL;
	}

	return;
}
