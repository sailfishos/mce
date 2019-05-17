/**
 * @file mce-modules.c
 * Module handling for MCE
 * <p>
 * Copyright © 2007-2009 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
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

#include "mce-modules.h"

#include "mce.h"
#include "mce-log.h"
#include "mce-conf.h"

#include <stdio.h>

#include <gmodule.h>

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
			"\n"
			"Module: %s\n", modulename);

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
			"name:",
			modinfo->name ? modinfo->name : "<undefined>");

		if (modinfo->depends != NULL)
			tmp = g_strjoinv(",", (gchar **)(modinfo->depends));

		fprintf(stdout,
			"        %-32s %s\n",
			"depends:",
			tmp ? tmp : "");

		g_free(tmp);
		tmp = NULL;

		if (modinfo->recommends != NULL)
			tmp = g_strjoinv(",", (gchar **)(modinfo->recommends));

		fprintf(stdout,
			"        %-32s %s\n",
			"recommends:",
			tmp ? tmp : "");

		g_free(tmp);
		tmp = NULL;

		if (modinfo->provides != NULL)
			tmp = g_strjoinv(",", (gchar **)(modinfo->provides));

		fprintf(stdout,
			"        %-32s %s\n",
			"provides:",
			tmp ? tmp : "");

		g_free(tmp);
		tmp = NULL;

		if (modinfo->enhances != NULL)
			tmp = g_strjoinv(",", (gchar **)(modinfo->enhances));

		fprintf(stdout,
			"        %-32s %s\n",
			"enhances:",
			tmp ? tmp : "");

		g_free(tmp);
		tmp = NULL;

		if (modinfo->conflicts != NULL)
			tmp = g_strjoinv(",", (gchar **)(modinfo->conflicts));

		fprintf(stdout,
			"        %-32s %s\n",
			"conflicts:",
			tmp ? tmp : "");

		g_free(tmp);
		tmp = NULL;

		if (modinfo->replaces != NULL)
			tmp = g_strjoinv(",", (gchar **)(modinfo->replaces));

		fprintf(stdout,
			"        %-32s %s\n",
			"replaces:",
			tmp ? tmp : "");

		g_free(tmp);

		fprintf(stdout,	"        %-32s %d\n",
			"priority:",
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
			if( mce_in_valgrind_mode() ) {
				/* Do not actually unmap the plugins so that
				 * valgrind can still locate the symbols at
				 * exit time. */
				gpointer addr = 0;
				g_module_symbol(module, "g_module_unload", &addr);
				if( addr ) {
					mce_log(LL_WARN, "simulating module %s unload",
						g_module_name(module));
					void (*unload)(GModule *) = addr;
					unload(module);
				}
				else {
					mce_log(LL_WARN, "skipping module %s unload",
						g_module_name(module));
				}
				continue;
			}
			g_module_close(module);
		}

		g_slist_free(modules);
		modules = NULL;
	}

	return;
}
