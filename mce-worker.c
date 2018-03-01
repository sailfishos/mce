/**
 * @file mce-worker.c
 *
 * Mode Control Entity - Offload blocking operations to a worker thread
 *
 * <p>
 *
 * Copyright (C) 2015 Jolla Ltd.
 *
 * <p>
 *
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

#include "mce-worker.h"
#include "mce-log.h"

#include <sys/eventfd.h>

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

#include <glib.h>

/* ========================================================================= *
 * FUNCTIONALITY
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * MISC_UTIL
 * ------------------------------------------------------------------------- */

static guint mw_add_iowatch(int fd, bool close_on_unref, GIOCondition cnd, GIOFunc io_cb, gpointer aptr);

/* ------------------------------------------------------------------------- *
 * MCE_JOB
 * ------------------------------------------------------------------------- */

typedef struct mce_job_t mce_job_t;

/** Job object */
struct mce_job_t
{
    /** Link to the next job in line */
    mce_job_t  *mj_next;

    /** Validation context for this job */
    char       *mj_context;

    /** Name of this job */
    char       *mj_name;

    /** Callback for executing the job */
    void     *(*mj_handle)(void *);

    /** Callback for notifying job executed */
    void      (*mj_notify)(void *, void *);

    /** User data to be passed to the callbacks */
    void       *mj_param;

    /** Reply value from execute callback, passed to notification callback */
    void       *mj_reply;
};

static const char    *mce_job_context       (const mce_job_t *self);
static const char    *mce_job_name          (const mce_job_t *self);

static void           mce_job_notify        (mce_job_t *self);
static void           mce_job_execute       (mce_job_t *self);

static void           mce_job_delete        (mce_job_t *self);
static mce_job_t     *mce_job_create        (const char *context, const char *name, void *(*handle)(void *), void (*notify)(void *, void *), void *param);

/* ------------------------------------------------------------------------- *
 * MCE_JOBLIST
 * ------------------------------------------------------------------------- */

/** Job list object */
typedef struct {
    /* Pointer to the first job in the queue */
    mce_job_t  *mjl_head;
    /* Pointer to the last job in the queue */
    mce_job_t  *mjl_tail;
} mce_joblist_t;

static mce_job_t     *mce_joblist_pull      (mce_joblist_t *self);
static void           mce_joblist_push      (mce_joblist_t *self, mce_job_t *job);
static void           mce_joblist_delete    (mce_joblist_t *self);
static mce_joblist_t *mce_joblist_create    (void);

/* ------------------------------------------------------------------------- *
 * MCE_WORKER
 * ------------------------------------------------------------------------- */

static gboolean       mce_worker_notify_cb  (GIOChannel *chn, GIOCondition cnd, gpointer data);
static void           mce_worker_execute    (void);
static void          *mce_worker_main       (void *aptr);

void                  mce_worker_add_job    (const char *context, const char *name, void *(*handle)(void *), void (*notify)(void *, void *), void *param);

void                  mce_worker_add_context(const char *context);
void                  mce_worker_rem_context(const char *context);
static bool           mce_worker_has_context(const char *context);

bool                  mce_worker_init       (void);
void                  mce_worker_quit       (void);

/** Flag for: Worker thread is running */
static bool             mw_is_ready = false;

/** List of jobs to be executed */
static mce_joblist_t   *mw_req_list  = 0;

/** Mutex protecting access to mw_req_list */
static pthread_mutex_t  mw_req_mutex = PTHREAD_MUTEX_INITIALIZER;

/** eventfd descriptor for waking up worker thread after adding new jobs */
static int              mw_req_evfd  = -1;

/** Worker thread id */
static pthread_t        mw_req_tid   = 0;

/** List of jobs already executed */
static mce_joblist_t   *mw_rsp_list  = 0;

/** Mutex protecting access to mw_rsp_list */
static pthread_mutex_t  mw_rsp_mutex = PTHREAD_MUTEX_INITIALIZER;

/** eventfd descriptor for waking up main thread after executing jobs */
static int              mw_rsp_evfd  = -1;

/** I/O watch identifier for mw_rsp_evfd */
static guint            mw_rsp_wid   = 0;

/** Lookup table containing valid context strings */
static GHashTable      *mw_ctx_lut   = 0;

/** Mutex protecting access to mw_ctx_lut */
static pthread_mutex_t  mw_ctx_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ========================================================================= *
 * MISC_UTIL
 * ========================================================================= */

/** Helper for creating I/O watch for file descriptor
 */
static guint
mw_add_iowatch(int fd, bool close_on_unref,
               GIOCondition cnd, GIOFunc io_cb, gpointer aptr)
{
    guint         wid = 0;
    GIOChannel   *chn = 0;

    if( !(chn = g_io_channel_unix_new(fd)) )
        goto cleanup;

    g_io_channel_set_close_on_unref(chn, close_on_unref);

    cnd |= G_IO_ERR | G_IO_HUP | G_IO_NVAL;

    if( !(wid = g_io_add_watch(chn, cnd, io_cb, aptr)) )
        goto cleanup;

cleanup:
    if( chn != 0 ) g_io_channel_unref(chn);

    return wid;

}

/* ========================================================================= *
 * MCE_JOB
 * ========================================================================= */

/** Get job name string
 *
 * @param self job object, or NULL
 *
 * @return name of the job, or "unknown"
 */
static const char *
mce_job_name(const mce_job_t *self)
{
    const char *name = 0;
    if( self )
        name = self->mj_name;
    return name ?: "unknown";
}

/** Get job context string
 *
 * @param self job object, or NULL
 *
 * @return context of the job, or "global"
 */
static const char *
mce_job_context(const mce_job_t *self)
{
    const char *context = 0;
    if( self )
        context = self->mj_context;
    return context ?: "global";
}

/** Job executed notification
 *
 * This must be called from the mainloop thread.
 *
 * @param self job object, or NULL
 */
static void
mce_job_notify(mce_job_t *self)
{
    if( !self )
        goto EXIT;

    if( !self->mj_notify )
        goto EXIT;

    mce_log(LL_DEBUG, "job(%s:%s) notify", mce_job_context(self), mce_job_name(self));

    pthread_mutex_lock(&mw_ctx_mutex);
    if( mce_worker_has_context(self->mj_context) )
        self->mj_notify(self->mj_param, self->mj_reply);
    pthread_mutex_unlock(&mw_ctx_mutex);

EXIT:
    return;
}

/** Execute job
 *
 * This must be called from the worker thread.
 *
 * @param self job object, or NULL
 */
static void
mce_job_execute(mce_job_t *self)
{
    if( !self )
        goto EXIT;

    if( !self->mj_handle )
        goto EXIT;

    mce_log(LL_DEBUG, "job(%s:%s) execute", mce_job_context(self), mce_job_name(self));

    pthread_mutex_lock(&mw_ctx_mutex);
    if( mce_worker_has_context(self->mj_context) )
        self->mj_reply = self->mj_handle(self->mj_param);
    pthread_mutex_unlock(&mw_ctx_mutex);

EXIT:
    return;
}

/** Delete job object
 *
 * @param self job object, or NULL
 */
static void
mce_job_delete(mce_job_t *self)
{
    if( !self )
        goto EXIT;

    mce_log(LL_DEBUG, "job(%s:%s) deleted", mce_job_context(self), mce_job_name(self));

    free(self->mj_name);
    free(self->mj_context);
    free(self);

EXIT:
    return;
}

/** Create job object
 *
 * @param context  Validation context string
 * @param name     Job name string
 * @param handle   Execute callback (in worker thread)
 * @param notify   Finished callback (in main thread)
 * @param param    User data to be passed to callbacks
 *
 * @return job object
 */

static mce_job_t *
mce_job_create(const char *context,
               const char *name,
               void *(*handle)(void *),
               void (*notify)(void *, void *),
               void *param)
{
    mce_job_t *self = calloc(1, sizeof *self);

    self->mj_next    = 0;
    self->mj_context = context ? strdup(context) : 0;
    self->mj_name    = name ? strdup(name) : 0;
    self->mj_handle  = handle;
    self->mj_notify  = notify;
    self->mj_param   = param;
    self->mj_reply   = 0;

    mce_log(LL_DEBUG, "job(%s:%s) created", mce_job_context(self), mce_job_name(self));

    return self;
}

/* ========================================================================= *
 * MCE_JOBLIST
 * ========================================================================= */

/** Pull a job object from a list of jobs
 *
 * Owenership of non-null job is transferred to the caller.

 * @param self  Job list object, or NULL
 *
 * @return job object, or NULL
 */
static mce_job_t *
mce_joblist_pull(mce_joblist_t *self)
{
    mce_job_t *job = 0;

    if( !self )
        goto EXIT;

    if( !(job = self->mjl_head) )
        goto EXIT;

    if( !(self->mjl_head = job->mj_next) )
        self->mjl_tail = 0;
    job->mj_next = 0;

EXIT:
    return job;
}

/** Pull a job object from a list of jobs
 *
 * Owenership of non-null job is transferred to the joblist.
 *
 * @param self  Job list object, or NULL
 * @param job   Job object, or NULL
 */
static void
mce_joblist_push(mce_joblist_t *self, mce_job_t *job)
{
    if( !self || !job )
        goto EXIT;

    if( self->mjl_tail )
        self->mjl_tail->mj_next = job;
    else
        self->mjl_head = job;
    self->mjl_tail = job;
    job = 0;

EXIT:
    if( job ) {
        mce_log(LL_ERR, "job(%s:%s) could not be queued; deleting",
                mce_job_context(job), mce_job_name(job));
        mce_job_delete(job);
    }

    return;
}

/** Delete job list object and all contained jobs
 *
 * @param self  Job list object, or NULL
 */
static void
mce_joblist_delete(mce_joblist_t *self)
{
    mce_job_t *job;

    if( !self )
        goto EXIT;

    while( (job = mce_joblist_pull(self)) )
        mce_job_delete(job);

    free(self);

EXIT:
    return;
}

/** Create job list object
 *
 * @return job list object
 */
static mce_joblist_t *
mce_joblist_create(void)
{
    mce_joblist_t *self = calloc(1, sizeof *self);

    self->mjl_head = 0;
    self->mjl_tail = 0;

    return self;
}

/* ========================================================================= *
 * MCE_WORKER
 * ========================================================================= */

/** Check validity of job context
 *
 * Note: Caller must hold mw_ctx_mutex.
 *
 * @param context Context string, or NULL for global
 *
 * @return true if context is valid, false otherwise
 */
static bool
mce_worker_has_context(const char *context)
{
    if( !mw_is_ready )
        return false;

    if( !context )
        return true;

    if( !mw_ctx_lut )
        return false;;

    return g_hash_table_lookup(mw_ctx_lut, context) != 0;
}

/** Mark job context as valid
 *
 * @param context Context string, or NULL for nop
 */
void
mce_worker_add_context(const char *context)
{
    if( !mw_is_ready )
        goto EXIT;

    if( !context )
        goto EXIT;

    if( !mw_ctx_lut )
        goto EXIT;

    pthread_mutex_lock(&mw_ctx_mutex);
    g_hash_table_replace(mw_ctx_lut, g_strdup(context), GINT_TO_POINTER(1));
    pthread_mutex_unlock(&mw_ctx_mutex);

    mce_log(LL_DEBUG, "%s: context enabled", context);

EXIT:
    return;
}

/** Mark job context as invalid
 *
 * @param context Context string, or NULL for nop
 */
void
mce_worker_rem_context(const char *context)
{
    if( !mw_ctx_lut )
        goto EXIT;

    if( !context )
        goto EXIT;

    pthread_mutex_lock(&mw_ctx_mutex);
    g_hash_table_remove(mw_ctx_lut, context);
    pthread_mutex_unlock(&mw_ctx_mutex);

    mce_log(LL_DEBUG, "%s: context disabled", context);

EXIT:
    return;
}

/** Callback for: Handle job executed notifications
 *
 * Note: This is called from main thread.
 *
 * @param chn   I/O channel for eventfd
 * @param cnd   Wakeup reason
 * @param data  User data (not used)
 *
 * @return FALSE if io watch should be disabled, TRUE otherwise
 */
static gboolean
mce_worker_notify_cb(GIOChannel *chn, GIOCondition cnd, gpointer data)
{
    (void)data;

    gboolean keep_going = FALSE;

    if( !mw_rsp_wid )
        goto cleanup_nak;

    int fd = g_io_channel_unix_get_fd(chn);

    if( fd < 0 )
        goto cleanup_nak;

    if( cnd & ~G_IO_IN )
        goto cleanup_nak;

    if( !(cnd & G_IO_IN) )
        goto cleanup_ack;

    uint64_t cnt = 0;

    int rc = read(fd, &cnt, sizeof cnt);

    if( rc == 0 ) {
        mce_log(LL_ERR, "unexpected eof");
        goto cleanup_nak;
    }

    if( rc == -1 ) {
        if( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK )
            goto cleanup_ack;

        mce_log(LL_ERR, "read error: %m");
        goto cleanup_nak;
    }

    if( rc != sizeof cnt )
        goto cleanup_nak;

    for( ;; ) {
        pthread_mutex_lock(&mw_rsp_mutex);
        mce_job_t *job = mce_joblist_pull(mw_rsp_list);
        pthread_mutex_unlock(&mw_rsp_mutex);

        if( !job )
            break;

        mce_job_notify(job);
        mce_job_delete(job);
    }

cleanup_ack:
    keep_going = TRUE;

cleanup_nak:

    if( !keep_going ) {
        mw_rsp_wid = 0;
        mce_log(LL_CRIT, "worker notifications disabled");
    }

    return keep_going;
}

/** Execute queued jobs
 *
 * Note: This is called from worker thread
 */
static void
mce_worker_execute(void)
{
    for( ;; ) {
        pthread_mutex_lock(&mw_req_mutex);
        mce_job_t *job = mce_joblist_pull(mw_req_list);
        pthread_mutex_unlock(&mw_req_mutex);

        if( !job )
            break;

        mce_job_execute(job);

        pthread_mutex_lock(&mw_rsp_mutex);
        mce_joblist_push(mw_rsp_list, job);
        pthread_mutex_unlock(&mw_rsp_mutex);

        uint64_t cnt = 1;
        if( write(mw_rsp_evfd, &cnt, sizeof cnt) == -1 ) {
            mce_log(LL_ERR, "signaling job finished failed: %m");
        }
    }
}

/** Worker thread mainloop
 *
 * @param aptr user data (not used)
 *
 * @return NULL
 */
static void *
mce_worker_main(void *aptr)
{
    (void)aptr;

    /* Allow quick and dirty cancellation */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, 0);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, 0);

    for( ;; ) {
        uint64_t cnt = 0;
        int rc = read(mw_req_evfd, &cnt, sizeof cnt);

        if( rc == -1 ) {
            if( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK )
                continue;
            mce_log(LL_ERR, "read: %m");
            goto EXIT;
        }

        if( rc != sizeof cnt )
            continue;

        if( cnt > 0 )
            mce_worker_execute();
    }

EXIT:
    return 0;
}

/** Queue a job to be executed in worker thread
 *
 * @param context Validation context string, or NULL for global
 * @param handle  Execute job callback
 * @param notify  Job finished notification callback
 * @param param   Pointer to be passed to the callbacks
 */
void
mce_worker_add_job(const char *context, const char *name,
                   void *(*handle)(void *),
                   void (*notify)(void *, void *),
                   void *param)
{
    if( !mw_is_ready ) {
        mce_log(LL_ERR, "job(%s:%s) scheduled while not ready", context, name);
        goto EXIT;
    }

    mce_job_t *job = mce_job_create(context, name, handle, notify, param);
    pthread_mutex_lock(&mw_req_mutex);
    mce_joblist_push(mw_req_list, job);
    pthread_mutex_unlock(&mw_req_mutex);

    uint64_t cnt = 1;
    if( write(mw_req_evfd, &cnt, sizeof cnt) == -1 ) {
        mce_log(LL_ERR, "signaling job added failed: %m");
    }

EXIT:
    return;
}

/** Terminate worker thread
 */
void
mce_worker_quit(void)
{
    /* No longer ready to accept jobs */
    mw_is_ready = false;

    /* Stop worker thread */

    if( mw_req_tid ) {
        if( pthread_cancel(mw_req_tid) != 0 ) {
            mce_log(LOG_ERR, "failed to stop worker thread");
        }
        else {
            void *status = 0;
            pthread_join(mw_req_tid, &status);
            mce_log(LOG_DEBUG, "worker stopped, status = %p", status);
        }
        mw_req_tid = 0;
    }

    /* Note: The worker thread is killed asynchronously, so it is
     *       possible that the mutexes are left in locked state
     *       and thus must not be used after this stage.
     */

    /* Remove request pipeline */

    mce_joblist_delete(mw_req_list),
        mw_req_list = 0;

    if( mw_req_evfd != -1 )
        close(mw_req_evfd), mw_req_evfd = -1;

    /* Remove notify pipeline */

    if( mw_rsp_wid )
        g_source_remove(mw_rsp_wid), mw_rsp_wid = 0;

    mce_joblist_delete(mw_rsp_list),
        mw_rsp_list = 0;

    if( mw_rsp_evfd != -1 )
        close(mw_rsp_evfd), mw_req_evfd = -1;

    /* Remove context lookup table */

    if( mw_ctx_lut )
        g_hash_table_unref(mw_ctx_lut), mw_ctx_lut = 0;
}

/** Start worker thread
 *
 * @return true on success, false on failure
 */
bool
mce_worker_init(void)
{
    bool ack = false;

    /* Setup context lookup table */

    mw_ctx_lut = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, 0);

    /* Setup notify pipeline */

    if( !(mw_rsp_list = mce_joblist_create()) )
        goto EXIT;

    if( (mw_rsp_evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) == -1 )
        goto EXIT;

    mw_rsp_wid = mw_add_iowatch(mw_rsp_evfd, false, G_IO_IN,
                                mce_worker_notify_cb, 0);
    if( !mw_rsp_wid )
        goto EXIT;

    /* Setup request pipeline */

    if( !(mw_req_list = mce_joblist_create()) )
        goto EXIT;

    if( (mw_req_evfd = eventfd(0, EFD_CLOEXEC)) == -1 )
        goto EXIT;

    /* Start worker thread */
    if( pthread_create(&mw_req_tid, 0, mce_worker_main, 0) != 0 ) {
        mw_req_tid = 0;
        goto EXIT;
    }

    /* Note: From now on joblist access must use mutex locking */

    /* Ready to accept jobs */
    mw_is_ready = true;

    ack = true;

EXIT:

    /* All or nothing */
    if( !ack )
        mce_worker_quit();

    return ack;
}
