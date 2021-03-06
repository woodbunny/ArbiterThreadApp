/*
    FUSE: Filesystem in Userspace
    Copyright (C) 2001-2005  Miklos Szeredi <miklos@szeredi.hu>

    This program can be distributed under the terms of the GNU LGPL.
    See the file COPYING.LIB.
*/

#include "fuse_i.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>

#define FUSE_MAX_WORKERS 10

struct fuse_worker {
    struct fuse *f;
    pthread_t threads[FUSE_MAX_WORKERS];
    void *data;
    fuse_processor_t proc;
};

static pthread_key_t context_key;
static pthread_mutex_t context_lock = PTHREAD_MUTEX_INITIALIZER;
static int context_ref;

static int start_thread(struct fuse_worker *w, pthread_t *thread_id);

static void *do_work(void *data)
{
    struct fuse_worker *w = (struct fuse_worker *) data;
    struct fuse *f = w->f;
    struct fuse_context *ctx;
    int is_mainthread = (f->numworker == 1);
	char *mt = NULL;

	mt = getenv("ARBITER_MT");
	AB_INFO("ARBITER_MT=%s\n", mt);

	AB_INFO("%u: try malloc ctx...\n", ab_pthread_self());
    ctx = (struct fuse_context *) malloc(sizeof(struct fuse_context));
    if (ctx == NULL) {
        fprintf(stderr, "fuse: failed to allocate fuse context\n");
        pthread_mutex_lock(&f->worker_lock);
        f->numavail --;
        pthread_mutex_unlock(&f->worker_lock);
        return NULL;
    }
    pthread_setspecific(context_key, ctx);

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	AB_INFO("%u: entering loop...\n", ab_pthread_self());
    while (1) {
        struct fuse_cmd *cmd;

        if (fuse_exited(f))
            break;

        AB_DBG("%u: read cmd...\n", ab_pthread_self());
        cmd = fuse_read_cmd(w->f);
        if (cmd == NULL)
            continue;

		AB_INFO("%u: f->numavail=%d\n", ab_pthread_self(), f->numavail);
        //if (strncmp(mt, "Y", 1)==0 && f->numavail == 0 && f->numworker < FUSE_MAX_WORKERS) {
		AB_INFO("%u: numworker=%d\n", ab_pthread_self(), f->numworker);
        if (f->numworker < 3) {
            pthread_mutex_lock(&f->worker_lock);
            if (f->numworker < FUSE_MAX_WORKERS) {
                /* FIXME: threads should be stored in a list instead
                   of an array */
                int res;
                pthread_t *thread_id = &w->threads[f->numworker];
                f->numavail ++;
                f->numworker ++;
                pthread_mutex_unlock(&f->worker_lock);
                res = start_thread(w, thread_id);
                if (res == -1) {
                    pthread_mutex_lock(&f->worker_lock);
                    f->numavail --;
                    pthread_mutex_unlock(&f->worker_lock);
                }
            } else
                pthread_mutex_unlock(&f->worker_lock);
        }

		AB_INFO("%u: processing cmd...\n", ab_pthread_self());
        w->proc(w->f, cmd, w->data);
        //sleep(1000);
    }

    /* Wait for cancellation */
    if (!is_mainthread)
        pause();

    return NULL;
}

static int start_thread(struct fuse_worker *w, pthread_t *thread_id)
{
    sigset_t oldset;
    sigset_t newset;
    int res;

    label_t L = {};
    own_t O = {};
    /* Disallow signal reception in worker threads */
    sigfillset(&newset);
    //pthread_sigmask(SIG_SETMASK, &newset, &oldset);
    //res = pthread_create(thread_id, NULL, do_work, w);
    res = ab_pthread_create(thread_id, NULL, do_work, w, L, O);
	AB_INFO("%u: ab_pthread_create done!\n", ab_pthread_self());
    //pthread_sigmask(SIG_SETMASK, &oldset, NULL);
    if (res != 0) {
        fprintf(stderr, "fuse: error creating thread: %s\n", strerror(res));
        return -1;
    }

    //pthread_detach(*thread_id);
	AB_INFO("%u: start_thread done!\n", ab_pthread_self());
    return 0;
}

static struct fuse_context *mt_getcontext(void)
{
    struct fuse_context *ctx =
        (struct fuse_context *) pthread_getspecific(context_key);
    if (ctx == NULL)
        fprintf(stderr, "fuse: no thread specific data for this thread\n");

    return ctx;
}

static void mt_freecontext(void *data)
{
    free(data);
}

static int mt_create_context_key()
{
    int err = 0;
    pthread_mutex_lock(&context_lock);
    if (!context_ref) {
        err = pthread_key_create(&context_key, mt_freecontext);
        if (err)
            fprintf(stderr, "fuse: failed to create thread specific key: %s\n",
                    strerror(err));
        else
            fuse_set_getcontext_func(mt_getcontext);
    }
    if (!err)
        context_ref ++;
    pthread_mutex_unlock(&context_lock);
    return err;
}

static void mt_delete_context_key()
{
    pthread_mutex_lock(&context_lock);
    context_ref--;
    if (!context_ref) {
        fuse_set_getcontext_func(NULL);
        pthread_key_delete(context_key);
    }
    pthread_mutex_unlock(&context_lock);
}

int fuse_loop_mt_proc(struct fuse *f, fuse_processor_t proc, void *data)
{
    struct fuse_worker *w;
    int i;

    label_t L = {};

    w = ab_malloc(sizeof(struct fuse_worker), L);
    if (w == NULL) {
        fprintf(stderr, "fuse: failed to allocate worker structure\n");
        return -1;
    }
    memset(w, 0, sizeof(struct fuse_worker));
    w->f = f;
    w->data = data;
    w->proc = proc;

    if (mt_create_context_key() != 0) {
        ab_free(w);
        return -1;
    }
    f->numworker = 1;
    do_work(w);

    pthread_mutex_lock(&f->lock);
    for (i = 1; i < f->numworker; i++)
        pthread_cancel(w->threads[i]);
    pthread_mutex_unlock(&f->lock);
    mt_delete_context_key();
    ab_free(w);
    f->exited = 0;
    return 0;
}

int fuse_loop_mt(struct fuse *f)
{
    if (f == NULL)
        return -1;

    return fuse_loop_mt_proc(f, (fuse_processor_t) fuse_process_cmd, NULL);
}

__asm__(".symver fuse_loop_mt_proc,__fuse_loop_mt@");
