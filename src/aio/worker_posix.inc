/*
    Copyright (c) 2013-2014 Martin Sustrik  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.
    Copyright 2016 Garrett D'Amore <garrett@damore.org>
    Copyright 2016 Franklin "Snaipe" Mathieu <franklinmathieu@gmail.com>

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "ctx.h"

#include "../utils/err.h"
#include "../utils/fast.h"
#include "../utils/cont.h"
#include "../utils/attr.h"
#include "../utils/queue.h"

#define NN_STATUS_IDLE 0
#define NN_STATUS_STOP 1
#define NN_STATUS_PAUSE 2

/*  Private functions. */
static void nn_worker_routine (void *arg);
static void nn_worker_postfork_routine (void *arg);

void nn_worker_fd_init (struct nn_worker_fd *self, int src,
    struct nn_fsm *owner)
{
    self->src = src;
    self->owner = owner;
}

void nn_worker_fd_term (NN_UNUSED struct nn_worker_fd *self)
{
}

void nn_worker_add_fd (struct nn_worker *self, int s, struct nn_worker_fd *fd)
{
    nn_poller_add (&self->poller, s, &fd->hndl);
}

void nn_worker_rm_fd (struct nn_worker *self, struct nn_worker_fd *fd)
{
    nn_poller_rm (&self->poller, &fd->hndl);
}

void nn_worker_set_in (struct nn_worker *self, struct nn_worker_fd *fd)
{
    nn_poller_set_in (&self->poller, &fd->hndl);
}

void nn_worker_reset_in (struct nn_worker *self, struct nn_worker_fd *fd)
{
    nn_poller_reset_in (&self->poller, &fd->hndl);
}

void nn_worker_set_out (struct nn_worker *self, struct nn_worker_fd *fd)
{
    nn_poller_set_out (&self->poller, &fd->hndl);
}

void nn_worker_reset_out (struct nn_worker *self, struct nn_worker_fd *fd)
{
    nn_poller_reset_out (&self->poller, &fd->hndl);
}

void nn_worker_add_timer (struct nn_worker *self, int timeout,
    struct nn_worker_timer *timer)
{
    nn_timerset_add (&self->timerset, timeout, &timer->hndl);
}

void nn_worker_rm_timer (struct nn_worker *self, struct nn_worker_timer *timer)
{
    nn_timerset_rm (&self->timerset, &timer->hndl);
}

void nn_worker_task_init (struct nn_worker_task *self, int src,
    struct nn_fsm *owner)
{
    self->src = src;
    self->owner = owner;
    nn_queue_item_init (&self->item);
}

void nn_worker_task_term (struct nn_worker_task *self)
{
    nn_queue_item_term (&self->item);
}

int nn_worker_init (struct nn_worker *self)
{
    int rc;

    rc = nn_efd_init (&self->efd);
    if (rc < 0)
        return rc;

    nn_mutex_init (&self->sync);
    nn_queue_init (&self->tasks);
    nn_queue_item_init (&self->stop);
    nn_queue_item_init (&self->pause);
    nn_poller_init (&self->poller);
    nn_poller_add (&self->poller, nn_efd_getfd (&self->efd), &self->efd_hndl);
    nn_poller_set_in (&self->poller, &self->efd_hndl);
    nn_timerset_init (&self->timerset);
    nn_thread_init (&self->thread, nn_worker_routine, self);

    self->paused = 0;
    self->resumed = 0;
    nn_cond_init (&self->resume_cond);
    nn_mutex_init (&self->resume_mutex);
    nn_cond_init (&self->pause_cond);
    nn_mutex_init (&self->pause_mutex);

    return 0;
}

void nn_worker_revive (struct nn_worker *self)
{
    /*  Collect resources allocated by the dead thread.
        This is only relevant when the pthread no longer exists, and
        we do not check this function call for failures. */
    pthread_join (self->thread.handle, NULL);

    /*  Revive poller  */
    nn_poller_revive (&self->poller);

    /*  Reset event file descriptor  */
    nn_poller_rm (&self->poller, &self->efd_hndl);
    nn_efd_term (&self->efd);
    nn_efd_init (&self->efd);
    nn_poller_add (&self->poller, nn_efd_getfd (&self->efd), &self->efd_hndl);
    nn_poller_set_in (&self->poller, &self->efd_hndl);

    /*  Empty the current worker queue */
    while (nn_queue_pop (&self->tasks) != NULL) {
        continue;
    }

    /*  Reinitialize the pause mutexes and condition variables since they
        are in an invalid state after a fork(2). */
    self->paused = 0;
    self->resumed = 0;
    nn_cond_init (&self->resume_cond);
    nn_mutex_init (&self->resume_mutex);
    nn_cond_init (&self->pause_cond);
    nn_mutex_init (&self->pause_mutex);

    /*  Revive the worker thread */
    nn_thread_init (&self->thread, nn_worker_postfork_routine, self);
}

void nn_worker_force_cleanup (struct nn_worker *self) {
    /*  Wait till worker thread terminates. */
    nn_thread_term (&self->thread);

    nn_timerset_term (&self->timerset);
    nn_poller_term (&self->poller);
    nn_efd_term (&self->efd);
    nn_queue_item_term (&self->stop);
    nn_queue_item_term (&self->pause);
    nn_queue_term (&self->tasks);
    nn_mutex_term (&self->sync);
}

void nn_worker_term (struct nn_worker *self)
{
    /*  Ask worker thread to terminate. */
    nn_mutex_lock (&self->sync);
    nn_queue_push (&self->tasks, &self->stop);
    nn_efd_signal (&self->efd);
    nn_mutex_unlock (&self->sync);

    /*  Clean up. */
    nn_worker_force_cleanup (self);

    nn_cond_term (&self->resume_cond);
    nn_cond_term (&self->pause_cond);
    nn_mutex_term (&self->resume_mutex);
    nn_mutex_term (&self->pause_mutex);
}

void nn_worker_pause (struct nn_worker *self)
{
    nn_assert (!self->paused);

    nn_mutex_lock (&self->pause_mutex);

    /*  Ask worker thread to pause. */
    nn_mutex_lock (&self->sync);
    nn_queue_push (&self->tasks, &self->pause);
    nn_efd_signal (&self->efd);
    nn_mutex_unlock (&self->sync);

    /*  Wait until the thread is paused  */
    while (!self->paused) {
        nn_cond_wait (&self->pause_cond, &self->pause_mutex);
    }

    nn_mutex_lock (&self->resume_mutex);
    nn_mutex_unlock (&self->pause_mutex);
}

void nn_worker_resume (struct nn_worker *self)
{
    nn_assert (self->paused);

    nn_mutex_lock (&self->pause_mutex);

    /*  Let the worker resume  */
    self->paused = 0;
    nn_cond_signal (&self->resume_cond);
    nn_mutex_unlock (&self->resume_mutex);

    /*  Wait until the thread is resumed  */
    while (!self->resumed) {
        nn_cond_wait (&self->pause_cond, &self->pause_mutex);
    }
    self->resumed = 0;

    nn_mutex_lock (&self->resume_mutex);
    nn_mutex_unlock (&self->pause_mutex);
    nn_mutex_unlock (&self->resume_mutex);
}

void nn_worker_execute (struct nn_worker *self, struct nn_worker_task *task)
{
    nn_mutex_lock (&self->sync);
    nn_queue_push (&self->tasks, &task->item);
    nn_efd_signal (&self->efd);
    nn_mutex_unlock (&self->sync);
}

void nn_worker_cancel (struct nn_worker *self, struct nn_worker_task *task)
{
    nn_mutex_lock (&self->sync);
    nn_queue_remove (&self->tasks, &task->item);
    nn_mutex_unlock (&self->sync);
}

static inline void nn_worker_copy_task_queue (struct nn_worker *self, 
    struct nn_queue *dst)
{
    /*  Make a local copy of the task queue. This way
        the application threads are not blocked and can post new
        tasks while the existing tasks are being processed. Also,
        new tasks can be posted from within task handlers. */
    nn_mutex_lock (&self->sync);
    nn_efd_unsignal (&self->efd);
    memcpy (dst, &self->tasks, sizeof (*dst));
    nn_queue_init (&self->tasks);
    nn_mutex_unlock (&self->sync);
}

static int nn_worker_process_tasks (struct nn_worker *self, struct nn_queue *tasks)
{
    struct nn_queue_item *item;
    struct nn_worker_task *task;
    int status;

    status = NN_STATUS_IDLE;

    while (1) {

        /*  Next worker task. */
        item = nn_queue_pop (tasks);
        if (nn_slow (!item))
            break;

        /*  If the worker thread is asked to pause, schedule a
        pause after everything else is processed. */
        if (nn_slow (item == &self->pause)) {
            status = NN_STATUS_PAUSE;
            continue;
        }

        /*  If the worker thread is asked to stop, do so. */
        if (nn_slow (item == &self->stop)) {
            /*  Make sure we remove all the other workers from
            the queue, because we're not doing anything with
            them. */
            while (nn_queue_pop (tasks) != NULL) {
                continue;
            }
            nn_queue_term (tasks);
            status = NN_STATUS_STOP;
            break;
        }

        /*  It's a user-defined task. Notify the user that it has
        arrived in the worker thread. */
        task = nn_cont (item, struct nn_worker_task, item);
        nn_ctx_enter (task->owner->ctx);
        nn_fsm_feed (task->owner, task->src, NN_WORKER_TASK_EXECUTE, task);
        nn_ctx_leave (task->owner->ctx);
    }
    nn_queue_term (tasks);
    return status;
}

static void nn_worker_postfork_routine (void *arg)
{
    int rc;
    struct nn_worker *self;
    int pevent;
    struct nn_poller_hndl *phndl;
    struct nn_queue tasks;
    int status = NN_STATUS_IDLE;

    self = (struct nn_worker*) arg;

    /*  Infinite loop. It will be interrupted only when the object is
        shut down. */
    while (1) {

        /*  Wait for new events and/or timeouts. */
        rc = nn_poller_wait (&self->poller,
            nn_timerset_timeout (&self->timerset));
        errnum_assert (rc == 0, -rc);

        /*  Process all events from the poller. */
        while (1) {

            /*  Get next poller event, such as IN or OUT. */
            rc = nn_poller_event (&self->poller, &pevent, &phndl);
            if (nn_slow (rc == -EAGAIN))
                break;

            /*  Only process worker tasks. */
            if (phndl != &self->efd_hndl)
                continue;

            nn_assert (pevent == NN_POLLER_IN);

            nn_worker_copy_task_queue (self, &tasks);
            status = nn_worker_process_tasks (self, &tasks);
            switch (status) {
            case NN_STATUS_STOP:
                return;
            case NN_STATUS_IDLE:
            default:
                continue;
            }
        }
    }

}

static void nn_worker_routine (void *arg)
{
    int rc;
    struct nn_worker *self;
    int pevent;
    struct nn_poller_hndl *phndl;
    struct nn_timerset_hndl *thndl;
    struct nn_queue tasks;
    struct nn_worker_fd *fd;
    struct nn_worker_timer *timer;
    int paused = 0;
    int stop = 0;
    int status = NN_STATUS_IDLE;

    self = (struct nn_worker*) arg;

    /*  Infinite loop. It will be interrupted only when the object is
        shut down. */
    while (1) {

        /*  If the worker thread is marked as paused, do so. */
        if (nn_slow (paused)) {

            /*  Process all non-io worker tasks until none are left */
            while (!nn_queue_empty(&self->tasks))
            {
                nn_worker_copy_task_queue (self, &tasks);
                status = nn_worker_process_tasks (self, &tasks);
                stop = stop || status == NN_STATUS_STOP;
            }

            nn_mutex_lock (&self->resume_mutex);

            /*  Notify the calling thread that the worker paused  */
            nn_mutex_lock (&self->pause_mutex);
            self->paused = 1;
            nn_cond_signal (&self->pause_cond);
            nn_mutex_unlock (&self->pause_mutex);

            /*  Pause until the thread is resumed  */
            while (self->paused) {
                nn_cond_wait (&self->resume_cond,
                    &self->resume_mutex);
            }

            /*  Notify the calling thread that the worker resumed */
            nn_mutex_lock (&self->pause_mutex);
            self->resumed = 1;
            nn_cond_signal (&self->pause_cond);
            nn_mutex_unlock (&self->pause_mutex);

            nn_mutex_unlock (&self->resume_mutex);

            paused = 0;

            if (nn_slow (stop))
                return;
        }

        /*  Wait for new events and/or timeouts. */
        rc = nn_poller_wait (&self->poller,
            nn_timerset_timeout (&self->timerset));
        errnum_assert (rc == 0, -rc);

        /*  Process all expired timers. */
        while (1) {
            rc = nn_timerset_event (&self->timerset, &thndl);
            if (rc == -EAGAIN)
                break;
            errnum_assert (rc == 0, -rc);
            timer = nn_cont (thndl, struct nn_worker_timer, hndl);
            nn_ctx_enter (timer->owner->ctx);
            nn_fsm_feed (timer->owner, -1, NN_WORKER_TIMER_TIMEOUT, timer);
            nn_ctx_leave (timer->owner->ctx);
        }

        /*  Process all events from the poller. */
        while (1) {

            /*  Get next poller event, such as IN or OUT. */
            rc = nn_poller_event (&self->poller, &pevent, &phndl);
            if (nn_slow (rc == -EAGAIN))
                break;

            /*  If there are any new incoming worker tasks, process them. */
            if (phndl == &self->efd_hndl) {
                nn_assert (pevent == NN_POLLER_IN);

                nn_worker_copy_task_queue (self, &tasks);
                status = nn_worker_process_tasks (self, &tasks);
                switch (status) {
                case NN_STATUS_STOP:
                    return;
                case NN_STATUS_PAUSE:
                    paused = 1;
                case NN_STATUS_IDLE:
                default:
                    continue;
                }
            }

            /*  It's a true I/O event. Invoke the handler. */
            fd = nn_cont (phndl, struct nn_worker_fd, hndl);
            nn_ctx_enter (fd->owner->ctx);
            nn_fsm_feed (fd->owner, fd->src, pevent, fd);
            nn_ctx_leave (fd->owner->ctx);
        }
    }
}

