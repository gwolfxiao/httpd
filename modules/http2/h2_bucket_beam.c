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

#include <apr_lib.h>
#include <apr_strings.h>
#include <apr_time.h>
#include <apr_buckets.h>
#include <apr_thread_mutex.h>
#include <apr_thread_cond.h>

#include <httpd.h>

#include "h2_util.h"
#include "h2_bucket_beam.h"

static void h2_beam_emitted(h2_bucket_beam *beam, apr_bucket *bred);

/*******************************************************************************
 * beam bucket with reference to beam and bucket it represents
 ******************************************************************************/

extern const apr_bucket_type_t h2_bucket_type_beam;

#define H2_BUCKET_IS_BEAM(e)     (e->type == &h2_bucket_type_beam)

typedef struct {
    apr_bucket_refcount refcount;
    h2_bucket_beam *beam;
    apr_bucket *bred;
} h2_beam_bucket;

static const char Dummy = '\0';

static apr_status_t beam_bucket_read(apr_bucket *b, const char **str, 
                                     apr_size_t *len, apr_read_type_e block)
{
    h2_beam_bucket *d = b->data;
    if (d->bred) {
        const char *data;
        apr_status_t status = apr_bucket_read(d->bred, &data, len, block);
        if (status == APR_SUCCESS) {
            *str = data + b->start;
            *len = b->length;
        }
        return status;
    }
    *str = &Dummy;
    *len = 0;
    return APR_SUCCESS;
}

static void beam_bucket_destroy(void *data)
{
    h2_beam_bucket *d = data;

    if (apr_bucket_shared_destroy(d)) {
        if (d->bred) {
            h2_beam_emitted(d->beam, d->bred);
        }
        apr_bucket_free(d);
    }
}

static apr_bucket * h2_beam_bucket_make(apr_bucket *b, 
                                        h2_bucket_beam *beam,
                                        apr_bucket *bred)
{
    h2_beam_bucket *d;

    d = apr_bucket_alloc(sizeof(*d), b->list);
    d->beam = beam;
    d->bred = bred;

    b = apr_bucket_shared_make(b, d, 0, bred? bred->length : 0);
    b->type = &h2_bucket_type_beam;

    return b;
}

static apr_bucket *h2_beam_bucket_create(h2_bucket_beam *beam,
                                         apr_bucket *bred,
                                         apr_bucket_alloc_t *list)
{
    apr_bucket *b = apr_bucket_alloc(sizeof(*b), list);

    APR_BUCKET_INIT(b);
    b->free = apr_bucket_free;
    b->list = list;
    return h2_beam_bucket_make(b, beam, bred);
}

APU_DECLARE_DATA const apr_bucket_type_t h2_bucket_type_beam = {
    "BEAM", 5, APR_BUCKET_DATA,
    beam_bucket_destroy,
    beam_bucket_read,
    apr_bucket_setaside_noop,
    apr_bucket_shared_split,
    apr_bucket_shared_copy
};

/*******************************************************************************
 * h2_blist, a brigade without allocations
 ******************************************************************************/
 
apr_size_t h2_util_bl_print(char *buffer, apr_size_t bmax, 
                            const char *tag, const char *sep, 
                            h2_blist *bl)
{
    apr_size_t off = 0;
    const char *sp = "";
    apr_bucket *b;
    
    if (bl) {
        memset(buffer, 0, bmax--);
        off += apr_snprintf(buffer+off, bmax-off, "%s(", tag);
        for (b = H2_BLIST_FIRST(bl); 
             bmax && (b != H2_BLIST_SENTINEL(bl));
             b = APR_BUCKET_NEXT(b)) {
            
            off += h2_util_bucket_print(buffer+off, bmax-off, b, sp);
            sp = " ";
        }
        off += apr_snprintf(buffer+off, bmax-off, ")%s", sep);
    }
    else {
        off += apr_snprintf(buffer+off, bmax-off, "%s(null)%s", tag, sep);
    }
    return off;
}



/*******************************************************************************
 * bucket beam that can transport buckets across threads
 ******************************************************************************/

static apr_status_t enter_yellow(h2_bucket_beam *beam, 
                                 apr_thread_mutex_t **plock, int *pacquired)
{
    if (beam->m_enter) {
        return beam->m_enter(beam->m_ctx, plock, pacquired);
    }
    *plock = NULL;
    *pacquired = 0;
    return APR_SUCCESS;
}

static void leave_yellow(h2_bucket_beam *beam, 
                         apr_thread_mutex_t *lock, int acquired)
{
    if (acquired && beam->m_leave) {
        beam->m_leave(beam->m_ctx, lock, acquired);
    }
}

static apr_off_t calc_buffered(h2_bucket_beam *beam)
{
    apr_off_t len = 0;
    apr_bucket *b;
    for (b = H2_BLIST_FIRST(&beam->red); 
         b != H2_BLIST_SENTINEL(&beam->red);
         b = APR_BUCKET_NEXT(b)) {
        if (b->length == ((apr_size_t)-1)) {
            /* do not count */
        }
        else if (APR_BUCKET_IS_FILE(b)) {
            /* if unread, has no real mem footprint. how to test? */
        }
        else {
            len += b->length;
        }
    }
    return len;
}

static void r_purge_reds(h2_bucket_beam *beam)
{
    apr_bucket *bred;
    /* delete all red buckets in purge brigade, needs to be called
     * from red thread only */
    while (!H2_BLIST_EMPTY(&beam->purge)) {
        bred = H2_BLIST_FIRST(&beam->purge);
        apr_bucket_delete(bred);
    }
}

static apr_size_t calc_space_left(h2_bucket_beam *beam)
{
    if (beam->max_buf_size > 0) {
        apr_off_t len = calc_buffered(beam);
        return (beam->max_buf_size > len? (beam->max_buf_size - len) : 0);
    }
    return APR_SIZE_MAX;
}

static apr_status_t wait_cond(h2_bucket_beam *beam, apr_thread_mutex_t *lock)
{
    if (beam->timeout > 0) {
        return apr_thread_cond_timedwait(beam->m_cond, lock, beam->timeout);
    }
    else {
        return apr_thread_cond_wait(beam->m_cond, lock);
    }
}

static apr_status_t r_wait_space(h2_bucket_beam *beam, apr_read_type_e block,
                                 apr_thread_mutex_t *lock, apr_off_t *premain) 
{
    *premain = calc_space_left(beam);
    while (!beam->aborted && *premain <= 0 
           && (block == APR_BLOCK_READ) && lock) {
        apr_status_t status = wait_cond(beam, lock);
        if (APR_STATUS_IS_TIMEUP(status)) {
            return status;
        }
        r_purge_reds(beam);
        *premain = calc_space_left(beam);
    }
    return beam->aborted? APR_ECONNABORTED : APR_SUCCESS;
}

static void h2_beam_prep_purge(h2_bucket_beam *beam, apr_bucket *bred)
{
    APR_BUCKET_REMOVE(bred);
    H2_BLIST_INSERT_TAIL(&beam->purge, bred);
}

static void h2_beam_emitted(h2_bucket_beam *beam, apr_bucket *bred)
{
    apr_thread_mutex_t *lock;
    int acquired;

    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        /* even when beam buckets are split, only the one where
         * refcount drops to 0 will call us */
        --beam->live_beam_buckets;
        /* invoked from green thread, the last beam bucket for the red
         * bucket bred is about to be destroyed.
         * remove it from the hold, where it should be now */
        h2_beam_prep_purge(beam, bred);
        /* notify anyone waiting on space to become available */
        if (!lock) {
            r_purge_reds(beam);
        }
        else if (beam->m_cond) {
            apr_thread_cond_broadcast(beam->m_cond);
        }
        leave_yellow(beam, lock, acquired);
    }
}

static void report_consumption(h2_bucket_beam *beam)
{
    if (beam->consumed_fn && (beam->received_bytes != beam->reported_bytes)) {
        beam->consumed_fn(beam->consumed_ctx, beam, 
                          beam->received_bytes - beam->reported_bytes);
        beam->reported_bytes = beam->received_bytes;
    }
}

static void h2_blist_cleanup(h2_blist *bl)
{
    apr_bucket *e;

    while (!H2_BLIST_EMPTY(bl)) {
        e = H2_BLIST_FIRST(bl);
        apr_bucket_delete(e);
    }
}

static apr_status_t beam_cleanup(void *data)
{
    h2_bucket_beam *beam = data;

    AP_DEBUG_ASSERT(beam->live_beam_buckets == 0);
    h2_blist_cleanup(&beam->red);
    h2_blist_cleanup(&beam->purge);
    h2_blist_cleanup(&beam->hold);
    return APR_SUCCESS;
}

apr_status_t h2_beam_destroy(h2_bucket_beam *beam)
{
    apr_pool_cleanup_kill(beam->life_pool, beam, beam_cleanup);
    return beam_cleanup(beam);
}

apr_status_t h2_beam_create(h2_bucket_beam **pbeam, apr_pool_t *life_pool, 
                            int id, const char *tag, 
                            apr_size_t max_buf_size)
{
    h2_bucket_beam *beam;
    apr_status_t status = APR_SUCCESS;
    
    beam = apr_pcalloc(life_pool, sizeof(*beam));
    if (!beam) {
        return APR_ENOMEM;
    }

    beam->id = id;
    beam->tag = tag;
    H2_BLIST_INIT(&beam->red);
    H2_BLIST_INIT(&beam->hold);
    H2_BLIST_INIT(&beam->purge);
    beam->life_pool = life_pool;
    beam->max_buf_size = max_buf_size;

    apr_pool_cleanup_register(life_pool, beam, beam_cleanup, 
                              apr_pool_cleanup_null);
    *pbeam = beam;
    
    return status;
}

void h2_beam_buffer_size_set(h2_bucket_beam *beam, apr_size_t buffer_size)
{
    apr_thread_mutex_t *lock;
    int acquired;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        beam->max_buf_size = buffer_size;
        leave_yellow(beam, lock, acquired);
    }
}

apr_size_t h2_beam_buffer_size_get(h2_bucket_beam *beam)
{
    apr_thread_mutex_t *lock;
    int acquired;
    apr_size_t buffer_size = 0;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        buffer_size = beam->max_buf_size;
        leave_yellow(beam, lock, acquired);
    }
    return buffer_size;
}

void h2_beam_mutex_set(h2_bucket_beam *beam, 
                       h2_beam_mutex_enter m_enter,
                       h2_beam_mutex_leave m_leave,
                       apr_thread_cond_t *cond,
                       void *m_ctx)
{
    apr_thread_mutex_t *lock;
    h2_beam_mutex_leave *prev_leave;
    void *prev_ctx;
    int acquired;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        prev_ctx = beam->m_ctx;
        prev_leave = beam->m_leave;
        beam->m_enter = m_enter;
        beam->m_leave = m_leave;
        beam->m_ctx   = m_ctx;
        beam->m_cond  = cond;
        if (acquired && prev_leave) {
            /* special tactics when NULLing a lock */
            prev_leave(prev_ctx, lock, acquired);
        }
    }
}

void h2_beam_timeout_set(h2_bucket_beam *beam, apr_interval_time_t timeout)
{
    apr_thread_mutex_t *lock;
    int acquired;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        beam->timeout = timeout;
        leave_yellow(beam, lock, acquired);
    }
}

apr_interval_time_t h2_beam_timeout_get(h2_bucket_beam *beam)
{
    apr_thread_mutex_t *lock;
    int acquired;
    apr_interval_time_t timeout = 0;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        timeout = beam->timeout;
        leave_yellow(beam, lock, acquired);
    }
    return timeout;
}

void h2_beam_abort(h2_bucket_beam *beam)
{
    apr_thread_mutex_t *lock;
    int acquired;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        r_purge_reds(beam);
        h2_blist_cleanup(&beam->red);
        beam->aborted = 1;
        report_consumption(beam);
        if (beam->m_cond) {
            apr_thread_cond_broadcast(beam->m_cond);
        }
        leave_yellow(beam, lock, acquired);
    }
}

static apr_status_t beam_close(h2_bucket_beam *beam)
{
    if (!beam->closed) {
        beam->closed = 1;
        if (beam->m_cond) {
            apr_thread_cond_broadcast(beam->m_cond);
        }
    }
    return APR_SUCCESS;
}

apr_status_t h2_beam_close(h2_bucket_beam *beam)
{
    apr_thread_mutex_t *lock;
    int acquired;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        r_purge_reds(beam);
        beam_close(beam);
        report_consumption(beam);
        leave_yellow(beam, lock, acquired);
    }
    return beam->aborted? APR_ECONNABORTED : APR_SUCCESS;
}

void h2_beam_shutdown(h2_bucket_beam *beam)
{
    apr_thread_mutex_t *lock;
    int acquired;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        r_purge_reds(beam);
        h2_blist_cleanup(&beam->red);
        beam_close(beam);
        report_consumption(beam);
        leave_yellow(beam, lock, acquired);
    }
}

void h2_beam_reset(h2_bucket_beam *beam)
{
    apr_thread_mutex_t *lock;
    int acquired;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        beam_cleanup(beam);
        beam->closed = beam->close_sent = 0;
        beam->sent_bytes = beam->received_bytes = beam->reported_bytes = 0;
        leave_yellow(beam, lock, acquired);
    }
}

static apr_status_t append_bucket(h2_bucket_beam *beam, 
                                  apr_bucket *bred,
                                  apr_read_type_e block,
                                  apr_pool_t *pool,
                                  apr_thread_mutex_t *lock)
{
    const char *data;
    apr_size_t len;
    apr_off_t space_left = 0;
    apr_status_t status;
    
    if (APR_BUCKET_IS_METADATA(bred)) {
        if (APR_BUCKET_IS_EOS(bred)) {
            beam->closed = 1;
        }
        APR_BUCKET_REMOVE(bred);
        H2_BLIST_INSERT_TAIL(&beam->red, bred);
        return APR_SUCCESS;
    }
    else if (APR_BUCKET_IS_FILE(bred)) {
        /* file bucket lengths do not really count */
    }
    else {
        space_left = calc_space_left(beam);
        if (space_left > 0 && bred->length == ((apr_size_t)-1)) {
            const char *data;
            status = apr_bucket_read(bred, &data, &len, APR_BLOCK_READ);
            if (status != APR_SUCCESS) {
                return status;
            }
        }
        
        if (space_left < bred->length) {
            status = r_wait_space(beam, block, lock, &space_left);
            if (status != APR_SUCCESS) {
                return status;
            }
            if (space_left <= 0) {
                return APR_EAGAIN;
            }
        }
        /* space available, maybe need bucket split */
    }
    

    /* The fundamental problem is that reading a red bucket from
     * a green thread is a total NO GO, because the bucket might use
     * its pool/bucket_alloc from a foreign thread and that will
     * corrupt. */
    status = APR_ENOTIMPL;
    if (beam->closed && bred->length > 0) {
        status = APR_EOF;
    }
    else if (APR_BUCKET_IS_TRANSIENT(bred)) {
        /* this takes care of transient buckets and converts them
         * into heap ones. Other bucket types might or might not be
         * affected by this. */
        status = apr_bucket_setaside(bred, pool);
    }
    else if (APR_BUCKET_IS_HEAP(bred) || APR_BUCKET_IS_POOL(bred)) {
        /* For heap/pool buckets read from a green thread is fine. The
         * data will be there and live until the bucket itself is
         * destroyed. */
        status = APR_SUCCESS;
    }
    else if (APR_BUCKET_IS_FILE(bred)) {
        /* For file buckets the problem is their internal readpool that
         * is used on the first read to allocate buffer/mmap.
         * Since setting aside a file bucket will de-register the
         * file cleanup function from the previous pool, we need to
         * call that from a red thread. Do it now and make our
         * yellow pool the owner. 
         * Additionally, we allow callbacks to prevent beaming file
         * handles across. The use case for this is to limit the number 
         * of open file handles and rather use a less efficient beam
         * transport. */ 
        apr_file_t *fd = ((apr_bucket_file *)bred->data)->fd;
        int can_beam = 1;
        if (beam->last_beamed != fd && beam->can_beam_fn) {
            can_beam = beam->can_beam_fn(beam->can_beam_ctx, beam, fd);
        }
        if (can_beam) {
            beam->last_beamed = fd;
            status = apr_bucket_setaside(bred, pool);
        }
    }
    
    if (status == APR_ENOTIMPL) {
        /* we have no knowledge about the internals of this bucket,
         * but on read, it needs to make the data available somehow.
         * So we do this while still in a red thread. The data will
         * live at least os long as the red bucket itself. */
        if (space_left < APR_BUCKET_BUFF_SIZE) {
            space_left = APR_BUCKET_BUFF_SIZE;
        }
        if (space_left < bred->length) {
            apr_bucket_split(bred, space_left);
        }
        status = apr_bucket_read(bred, &data, &len, APR_BLOCK_READ);
        if (status == APR_SUCCESS) {
            status = apr_bucket_setaside(bred, pool);
        }
    }
    
    if (status != APR_SUCCESS && status != APR_ENOTIMPL) {
        return status;
    }
    
    APR_BUCKET_REMOVE(bred);
    H2_BLIST_INSERT_TAIL(&beam->red, bred);
    beam->sent_bytes += bred->length;
    
    return APR_SUCCESS;
}

apr_status_t h2_beam_send(h2_bucket_beam *beam, 
                          apr_bucket_brigade *red_brigade, 
                          apr_read_type_e block)
{
    apr_thread_mutex_t *lock;
    apr_bucket *bred;
    apr_status_t status = APR_SUCCESS;
    int acquired;

    /* Called from the red thread to add buckets to the beam */
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        r_purge_reds(beam);
        
        if (beam->aborted) {
            status = APR_ECONNABORTED;
        }
        else if (red_brigade) {
            while (!APR_BRIGADE_EMPTY(red_brigade)
                   && status == APR_SUCCESS) {
                bred = APR_BRIGADE_FIRST(red_brigade);
                status = append_bucket(beam, bred, block, red_brigade->p, lock);
            }
            if (beam->m_cond) {
                apr_thread_cond_broadcast(beam->m_cond);
            }
        }
        report_consumption(beam);
        leave_yellow(beam, lock, acquired);
    }
    return status;
}

apr_status_t h2_beam_receive(h2_bucket_beam *beam, 
                             apr_bucket_brigade *bb, 
                             apr_read_type_e block,
                             apr_off_t readbytes)
{
    apr_thread_mutex_t *lock;
    apr_bucket *bred, *bgreen;
    int acquired, transferred = 0;
    apr_status_t status = APR_SUCCESS;
    apr_off_t remain = readbytes;
    
    /* Called from the green thread to take buckets from the beam */
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
transfer:
        if (beam->aborted) {
            status = APR_ECONNABORTED;
            goto leave;
        }

        /* transfer enough buckets from our green brigade, if we have one */
        while (beam->green
               && !APR_BRIGADE_EMPTY(beam->green)
               && (readbytes <= 0 || remain >= 0)) {
            bgreen = APR_BRIGADE_FIRST(beam->green);
            if (readbytes > 0 && bgreen->length > 0 && remain <= 0) {
                break;
            }            
            APR_BUCKET_REMOVE(bgreen);
            APR_BRIGADE_INSERT_TAIL(bb, bgreen);
            remain -= bgreen->length;
            ++transferred;
        }

        /* transfer from our red brigade, transforming red buckets to
         * green ones until we have enough */
        while (!H2_BLIST_EMPTY(&beam->red) && (readbytes <= 0 || remain >= 0)) {
            bred = H2_BLIST_FIRST(&beam->red);
            bgreen = NULL;
            
            if (readbytes > 0 && bred->length > 0 && remain <= 0) {
                break;
            }
                        
            if (APR_BUCKET_IS_METADATA(bred)) {
                if (APR_BUCKET_IS_EOS(bred)) {
                    beam->close_sent = 1;
                    bgreen = apr_bucket_eos_create(bb->bucket_alloc);
                }
                else if (APR_BUCKET_IS_FLUSH(bred)) {
                    bgreen = apr_bucket_flush_create(bb->bucket_alloc);
                }
                else {
                    /* put red into hold, no green sent out */
                }
            }
            else if (APR_BUCKET_IS_FILE(bred)) {
                /* This is set aside into the target brigade pool so that 
                 * any read operation messes with that pool and not 
                 * the red one. */
                apr_bucket_file *f = (apr_bucket_file *)bred->data;
                apr_file_t *fd = f->fd;
                int setaside = (f->readpool != bb->p);
                
                if (setaside) {
                    status = apr_file_setaside(&fd, fd, bb->p);
                    if (status != APR_SUCCESS) {
                        goto leave;
                    }
                    ++beam->files_beamed;
                }
                apr_brigade_insert_file(bb, fd, bred->start, bred->length, 
                                        bb->p);
                remain -= bred->length;
                ++transferred;
            }
            else {
                /* create a "green" standin bucket. we took care about the
                 * underlying red bucket and its data when we placed it into
                 * the red brigade.
                 * the beam bucket will notify us on destruction that bred is
                 * no longer needed. */
                bgreen = h2_beam_bucket_create(beam, bred, bb->bucket_alloc);
                ++beam->live_beam_buckets;
            }
            
            /* Place the red bucket into our hold, to be destroyed when no
             * green bucket references it any more. */
            APR_BUCKET_REMOVE(bred);
            H2_BLIST_INSERT_TAIL(&beam->hold, bred);
            beam->received_bytes += bred->length;
            if (bgreen) {
                APR_BRIGADE_INSERT_TAIL(bb, bgreen);
                remain -= bgreen->length;
                ++transferred;
            }
        }

        if (readbytes > 0 && remain < 0) {
            /* too much, put some back */
            remain = readbytes;
            for (bgreen = APR_BRIGADE_FIRST(bb);
                 bgreen != APR_BRIGADE_SENTINEL(bb);
                 bgreen = APR_BUCKET_NEXT(bgreen)) {
                 remain -= bgreen->length;
                 if (remain < 0) {
                     apr_bucket_split(bgreen, bgreen->length+remain);
                     beam->green = apr_brigade_split_ex(bb, 
                                                        APR_BUCKET_NEXT(bgreen), 
                                                        beam->green);
                     break;
                 }
            }
        }
                
        if (transferred) {
            status = APR_SUCCESS;
        }
        else if (beam->closed) {
            if (!beam->close_sent) {
                apr_bucket *b = apr_bucket_eos_create(bb->bucket_alloc);
                APR_BRIGADE_INSERT_TAIL(bb, b);
                beam->close_sent = 1;
                status = APR_SUCCESS;
            }
            else {
                status = APR_EOF;
            }
        }
        else if (block == APR_BLOCK_READ && lock && beam->m_cond) {
            status = wait_cond(beam, lock);
            if (status != APR_SUCCESS) {
                goto leave;
            }
            goto transfer;
        }
        else {
            status = APR_EAGAIN;
        }
leave:        
        leave_yellow(beam, lock, acquired);
    }
    return status;
}

void h2_beam_on_consumed(h2_bucket_beam *beam, 
                         h2_beam_consumed_callback *cb, void *ctx)
{
    apr_thread_mutex_t *lock;
    int acquired;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        beam->consumed_fn = cb;
        beam->consumed_ctx = ctx;
        leave_yellow(beam, lock, acquired);
    }
}

void h2_beam_on_file_beam(h2_bucket_beam *beam, 
                          h2_beam_can_beam_callback *cb, void *ctx)
{
    apr_thread_mutex_t *lock;
    int acquired;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        beam->can_beam_fn = cb;
        beam->can_beam_ctx = ctx;
        leave_yellow(beam, lock, acquired);
    }
}


apr_off_t h2_beam_get_buffered(h2_bucket_beam *beam)
{
    apr_thread_mutex_t *lock;
    apr_bucket *b;
    apr_off_t l = 0;
    int acquired;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        for (b = H2_BLIST_FIRST(&beam->red); 
            b != H2_BLIST_SENTINEL(&beam->red);
            b = APR_BUCKET_NEXT(b)) {
            /* should all have determinate length */
            l += b->length;
        }
        leave_yellow(beam, lock, acquired);
    }
    return l;
}

apr_off_t h2_beam_get_mem_used(h2_bucket_beam *beam)
{
    apr_thread_mutex_t *lock;
    apr_bucket *b;
    apr_off_t l = 0;
    int acquired;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        for (b = H2_BLIST_FIRST(&beam->red); 
            b != H2_BLIST_SENTINEL(&beam->red);
            b = APR_BUCKET_NEXT(b)) {
            if (APR_BUCKET_IS_FILE(b)) {
                /* do not count */
            }
            else {
                /* should all have determinate length */
                l += b->length;
            }
        }
        leave_yellow(beam, lock, acquired);
    }
    return l;
}

int h2_beam_empty(h2_bucket_beam *beam)
{
    apr_thread_mutex_t *lock;
    int empty = 1;
    int acquired;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        empty = (H2_BLIST_EMPTY(&beam->red) 
                 && (!beam->green || APR_BRIGADE_EMPTY(beam->green)));
        leave_yellow(beam, lock, acquired);
    }
    return empty;
}

int h2_beam_closed(h2_bucket_beam *beam)
{
    return beam->closed;
}

int h2_beam_was_received(h2_bucket_beam *beam)
{
    apr_thread_mutex_t *lock;
    int happend = 0;
    int acquired;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        happend = (beam->received_bytes > 0);
        leave_yellow(beam, lock, acquired);
    }
    return happend;
}

apr_size_t h2_beam_get_files_beamed(h2_bucket_beam *beam)
{
    apr_thread_mutex_t *lock;
    apr_size_t n = 0;
    int acquired;
    
    if (enter_yellow(beam, &lock, &acquired) == APR_SUCCESS) {
        n = beam->files_beamed;
        leave_yellow(beam, lock, acquired);
    }
    return n;
}

