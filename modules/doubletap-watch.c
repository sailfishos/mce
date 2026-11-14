/**
 * @file doubletap-watch.c
 * Doubletap watcher control module -- this handles touchscreen drivers
 * that report changes to sysfs
 * <p>
 * Copyright (c) 2026 Jolla Mobile Ltd
 * <p>
 * @author Richard Gráčik <morc@370.network>
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

#include "doubletap.h"


#include "../mce.h"
#include "../mce-log.h"
#include "../mce-conf.h"
#include "../evdev.h"

#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include <linux/input.h>

#include <gmodule.h>

/** Sysfs node "pointer" */
static guint dbltap_watch_id = 0;

/** Read the sysfs node */
static bool dbltap_watch_read(int fd)
{
        char buf[16] = {0};
        ssize_t doubletap_pressed = pread(fd, buf, sizeof buf - 1, 0);
        return doubletap_pressed > 0 && buf[0] == '1';
}

/** Inject a doubletap event */
static void dbltap_watch_wakeup(void)
{
        const struct input_event ev = {
                .type  = EV_MSC,
                .code  = MSC_GESTURE,
                .value = GESTURE_DOUBLETAP,
        };
        mce_log(LL_DEVEL, "waking up using the watched path");

        datapipe_exec_full(&user_activity_event_pipe, &ev);

        const struct input_event *evp = &ev;
        datapipe_exec_full(&keypress_event_pipe, &evp);
}

/** Sysfs watch callback */
static gboolean dbltap_watch_cb(GIOChannel *chn, GIOCondition cnd,
                                gpointer aptr)
{
        (void)aptr;

        if( cnd & (G_IO_NVAL | G_IO_HUP) ) {
                mce_log(LL_WARN, "doubletap watch lost");
                dbltap_watch_id = 0;
                return FALSE;
        }

        if( dbltap_watch_read(g_io_channel_unix_get_fd(chn)) )
                dbltap_watch_wakeup();

        return TRUE;
}

/** Init function for the doubletap watcher module
 *
 * @param module (not used)
 *
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
	(void)module;

        int fd = -1;

	/* Config from ini-files */
        gchar *path = mce_conf_get_string(MCE_CONF_DOUBLETAP_GROUP,
                                          MCE_CONF_DOUBLETAP_WATCH_PATH,
                                          NULL);

        /* Disable module when no path is passed*/
	if( !path )
                goto EXIT;

	/* Throw up error if the path is not openable */
        if( (fd = open(path, O_RDONLY | O_CLOEXEC)) == -1 ) {
                mce_log(LL_ERR, "%s: %s: open: %s",
                        g_module_name(module), path, strerror(errno));
                goto EXIT;
        }

        (void)dbltap_watch_read(fd);

        GIOChannel *chn = g_io_channel_unix_new(fd);
        g_io_channel_set_close_on_unref(chn, TRUE);
        dbltap_watch_id = g_io_add_watch(chn,
                                         G_IO_PRI | G_IO_ERR |
                                         G_IO_HUP | G_IO_NVAL,
                                         dbltap_watch_cb, 0);
        g_io_channel_unref(chn);

        mce_log(LL_NOTICE, "%s: watching %s", g_module_name(module), path);
EXIT:
        g_free(path);
        return NULL;
}

/** Exit function for the doubletap module
 *
 * @param module (not used)
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
        (void)module;

        /* Remove watched sources */
        if( dbltap_watch_id )
                g_source_remove(dbltap_watch_id), dbltap_watch_id = 0;
	
	return;
}
