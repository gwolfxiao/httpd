/* Copyright 2015 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>

#include <apr_thread_cond.h>

#include <mpm_common.h>
#include <httpd.h>
#include <http_core.h>
#include <http_log.h>

#include "h2.h"
#include "h2_private.h"
#include "h2_conn.h"
#include "h2_ctx.h"
#include "h2_h2.h"
#include "h2_mplx.h"
#include "h2_task.h"
#include "h2_worker.h"

static void* APR_THREAD_FUNC execute(apr_thread_t *thread, void *wctx)
{
    h2_worker *worker = (h2_worker *)wctx;
    int sticky;
    
    while (!worker->aborted) {
        h2_task *task;
        
        /* Get a h2_task from the main workers queue. */
        worker->get_next(worker, worker->ctx, &task, &sticky);
        while (task) {
        
            h2_task_do(task);
            /* report the task done and maybe get another one from the same
             * mplx (= master connection), if we can be sticky. 
             */
            if (sticky && !worker->aborted) {
                h2_mplx_task_done(task->mplx, task, &task);
            }
            else {
                h2_mplx_task_done(task->mplx, task, NULL);
                task = NULL;
            }
        }
    }

    worker->worker_done(worker, worker->ctx);
    return NULL;
}

h2_worker *h2_worker_create(int id,
                            apr_pool_t *pool,
                            apr_threadattr_t *attr,
                            h2_worker_mplx_next_fn *get_next,
                            h2_worker_done_fn *worker_done,
                            void *ctx)
{
    h2_worker *w = apr_pcalloc(pool, sizeof(h2_worker));
    if (w) {
        w->id = id;
        APR_RING_ELEM_INIT(w, link);
        w->get_next = get_next;
        w->worker_done = worker_done;
        w->ctx = ctx;
        apr_thread_create(&w->thread, attr, execute, w, pool);
    }
    return w;
}

apr_status_t h2_worker_destroy(h2_worker *worker)
{
    if (worker->thread) {
        apr_status_t status;
        apr_thread_join(&status, worker->thread);
        worker->thread = NULL;
    }
    return APR_SUCCESS;
}

void h2_worker_abort(h2_worker *worker)
{
    worker->aborted = 1;
}

int h2_worker_is_aborted(h2_worker *worker)
{
    return worker->aborted;
}


