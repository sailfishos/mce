/**
 * @file mce-fbdev.c
 * Frame buffer device handling code for the Mode Control Entity
 * <p>
 * Copyright 2015 Jolla Ltd.
 * <p>
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

#include "mce-fbdev.h"
#include "mce-log.h"
#include "mce.h"

#ifdef ENABLE_HYBRIS
# include "mce-hybris.h"
#endif

#include <linux/fb.h>

#include <sys/ioctl.h>

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

/* ========================================================================= *
 * CONSTANTS
 * ========================================================================= */

/** Path to the framebuffer device */
#define FB_DEVICE "/dev/fb0"

/* ========================================================================= *
 * STATE_DATA
 * ========================================================================= */

/** File descriptor for frame buffer device */
static int mce_fbdev_handle = -1;

/** Flag for: use hybris for fb power control */
#ifdef ENABLE_HYBRIS
static bool fbdev_use_hybris = false;
#endif

/** Flag for: Opening frame buffer device is allowed */
static bool mce_fbdev_allow_open = false;

/* ========================================================================= *
 * FBDEV_FILE_DESCRIPTOR
 * ========================================================================= */

/** Frame buffer is open predicate
 *
 * @return true if frame buffer device is currently opened, false otherwise
 */
bool mce_fbdev_is_open(void)
{
    return mce_fbdev_handle != -1;
}

/** Open frame buffer device unless denied
 *
 * @return true on success, or false on failure
 */
bool mce_fbdev_open(void)
{
#ifdef ENABLE_HYBRIS
    if( fbdev_use_hybris )
        goto EXIT;
#endif

    if( mce_fbdev_handle != -1 )
        goto EXIT;

    if( !mce_fbdev_allow_open )
        goto EXIT;

    mce_log(LL_NOTICE, "open frame buffer device");

    if( (mce_fbdev_handle = open(FB_DEVICE, O_RDWR)) == -1 ) {
        if( errno != ENOENT )
            mce_log(LL_WARN, "failed to open frame buffer device: %m");
        goto EXIT;
    }

    mce_log(LL_DEBUG, "frame buffer device opened");

EXIT:
    return mce_fbdev_handle != -1;
}

/** Close frame buffer device
 */
void mce_fbdev_close(void)
{
    if( mce_fbdev_handle == -1 )
        goto EXIT;

    mce_log(LL_NOTICE, "closing frame buffer device");
    close(mce_fbdev_handle), mce_fbdev_handle = -1;
    mce_log(LL_DEBUG, "closed frame buffer device");

EXIT:
    return;
}

/** Reopen frame buffer device unless denied
 */
void mce_fbdev_reopen(void)
{
    if( mce_fbdev_allow_open )
        mce_fbdev_close();

    mce_fbdev_open();
}

/* ========================================================================= *
 * POST_EXIT_LINGER
 * ========================================================================= */

/** Signal handler that just exits
 *
 * @param sig signal number (unused)
 */
static void mce_fbdev_linger_signal_hander(int sig)
{
    (void) sig;

    _exit(EXIT_SUCCESS);
}

/** Create a child process to keep frame buffer device open after mce exits
 *
 * The frame buffer device powers off automatically when the last open
 * file descriptor gets closed.
 *
 * To allow the shutdown logo to stay on screen after lipstick and mce
 * have been terminated, we create a detached child process that hangs
 * on to frame buffer device.
 *
 * @param delay_ms how long the child process should linger
 */
void mce_fbdev_linger_after_exit(int delay_ms)
{
    static const char msg[] = "closing frame buffer device after delay\n";

    /* Fork a child process */

    int child_pid = fork();

    /* Deal with parent side and return to caller */

    if( child_pid != 0 ) {
        if( child_pid < 0 )
            mce_log(LL_ERR, "forking fbdev linger child failed: %m");
        else
            mce_log(LL_DEBUG, "fbdev linger child: pid %d", child_pid);
        return;
    }

    /* Detach from parent so that we will not get killed with it */

    setsid();

    /* The parent process needs to pay special attention to signals
     * in order not to leave wakelocks active on exit. Also some
     * signals are transferred to mainloop via a pipe. None of this
     * should happen if the child process gets signals -> remove
     * all signal handlers the parent process has installed. */

    mce_signal_handlers_remove();

    /* Since the intent is that the child process exits very close to
     * power off, it is unlikely that core can be succesfully dumped
     * and processed -> trap all signals that do core dump by default
     * and make an _exit() instead. */

    static const int trap[] =
    {
        SIGQUIT, SIGILL, SIGABRT, SIGFPE, SIGSEGV,
        SIGBUS, SIGSYS, SIGTRAP, SIGXCPU, SIGXFSZ,
        -1
    };

    for( size_t i = 0; trap[i] != -1; ++i )
        signal(trap[i], mce_fbdev_linger_signal_hander);

    /* Close all files, except fbdev & stderr */

    int nfd = getdtablesize();
    for( int fd = 0; fd < nfd; ++fd ) {
        if( fd != mce_fbdev_handle && fd != STDERR_FILENO )
            close(fd);
    }

    if( delay_ms < 500 )
        delay_ms = 500;

    /* Wait ... */

    struct timespec ts =
    {
        .tv_sec  = (time_t)(delay_ms / 1000),
        .tv_nsec = (long)(delay_ms % 1000) * 1000000,
    };

    while( nanosleep(&ts, &ts) == -1 && errno == EINTR ) { /* nop */ }

    /* If journald is still up, the end-of-linger message written to stderr
     * ends up in journal and attributed to parent mce process.
     *
     * ... and in case journald has already made an exit, we do not want
     * to die by SIGPIPE, so it needs to be ignored. */

    signal(SIGPIPE, SIG_IGN);

    if( write(STDERR_FILENO, msg, sizeof msg - 1) < 0 ) { /* dontcare */ }

    /* Exit - the frame buffer device will power off if we
     * were the last process to have an open file descriptor */

    _exit(EXIT_SUCCESS);
}

/* ========================================================================= *
 * FRAMEBUFFER_POWER
 * ========================================================================= */

/** Set the frame buffer power state
 *
 * MCE uses this function for display power control only if autosuspend
 * control sysfs files are not present.
 *
 * If there is a hw composer that is also doing fbdev ioctl() calls,
 * there is some change that kernel side troubles can be caused by
 * having two entities trying to control the same resource.
 *
 * @param power_in  true to power up, false to power down
 */
void mce_fbdev_set_power(bool power_on)
{
    mce_log(LL_DEBUG, "fbdev power %s", power_on ? "up" : "down");

    if( mce_fbdev_handle != -1 ) {
        int value = power_on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN;

        if( ioctl(mce_fbdev_handle, FBIOBLANK, value) == -1 )
            mce_log(LL_ERR, "%s: ioctl(FBIOBLANK,%d): %m", FB_DEVICE, value);
        else
            mce_log(LL_DEBUG, "success");
    }
#ifdef ENABLE_HYBRIS
    else if( fbdev_use_hybris ) {
        mce_hybris_framebuffer_set_power(power_on);
    }
#endif

    return;
}

/* ========================================================================= *
 * MODULE_INIT
 * ========================================================================= */

/** Initialize frame buffer module
 */
void mce_fbdev_init(void)
{
    /* allow opening frame buffer device */
    mce_fbdev_allow_open = true;

    if( mce_fbdev_open() ) {
        mce_log(LL_NOTICE, "using ioctl for fb power control");
    }
#ifdef ENABLE_HYBRIS
    else if( mce_hybris_framebuffer_init() ) {
        mce_log(LL_NOTICE, "using libhybris for fb power control");
        fbdev_use_hybris = true;
    }
#endif
    else {
        mce_log(LL_WARN, "no fb power control available");;
    }
}

/** De-initialize frame buffer module
 */
void mce_fbdev_quit(void)
{
    /* deny opening frame buffer device */
    mce_fbdev_allow_open = false;

    mce_fbdev_close();

#ifdef ENABLE_HYBRIS
    if( fbdev_use_hybris ) {
        fbdev_use_hybris = false;
        mce_hybris_framebuffer_quit();
    }
#endif
}
