/*
 * Copyright (c) 2020 YuQing <384681@qq.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the Lesser GNU General Public License, version 3
 * or later ("LGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the Lesser GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

//fc_queue.h

#ifndef _FC_QUEUE_H
#define _FC_QUEUE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "common_define.h"
#include "fast_mblock.h"

struct fc_queue_info
{
    void *head;
    void *tail;
};

struct fc_queue
{
	void *head;
	void *tail;
    pthread_lock_cond_pair_t lc_pair;
    int next_ptr_offset;
};

#ifdef __cplusplus
extern "C" {
#endif

int fc_queue_init(struct fc_queue *queue, const int next_ptr_offset);

void fc_queue_destroy(struct fc_queue *queue);

static inline void fc_queue_terminate(struct fc_queue *queue)
{
    pthread_cond_signal(&queue->lc_pair.cond);
}

static inline void fc_queue_terminate_all(
        struct fc_queue *queue, const int count)
{
    int i;
    for (i=0; i<count; i++) {
        pthread_cond_signal(&(queue->lc_pair.cond));
    }
}

//notify by the caller
void fc_queue_push_ex(struct fc_queue *queue, void *data, bool *notify);

static inline void fc_queue_push(struct fc_queue *queue, void *data)
{
    bool notify;

    fc_queue_push_ex(queue, data, &notify);
    if (notify) {
        pthread_cond_signal(&(queue->lc_pair.cond));
    }
}

void fc_queue_push_queue_to_head_ex(struct fc_queue *queue,
        struct fc_queue_info *qinfo, bool *notify);

static inline void fc_queue_push_queue_to_head(struct fc_queue *queue,
        struct fc_queue_info *qinfo)
{
    bool notify;

    fc_queue_push_queue_to_head_ex(queue, qinfo, &notify);
    if (notify) {
        pthread_cond_signal(&(queue->lc_pair.cond));
    }
}

void *fc_queue_pop_ex(struct fc_queue *queue, const bool blocked);
#define fc_queue_pop(queue) fc_queue_pop_ex(queue, true)
#define fc_queue_try_pop(queue) fc_queue_pop_ex(queue, false)

void *fc_queue_pop_all_ex(struct fc_queue *queue, const bool blocked);
#define fc_queue_pop_all(queue) fc_queue_pop_all_ex(queue, true)
#define fc_queue_try_pop_all(queue) fc_queue_pop_all_ex(queue, false)

void fc_queue_pop_to_queue(struct fc_queue *queue,
        struct fc_queue_info *qinfo);

#ifdef __cplusplus
}
#endif

#endif
