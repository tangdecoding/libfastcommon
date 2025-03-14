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

//fast_allocator.c

#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include "logger.h"
#include "shared_func.h"
#include "sched_thread.h"
#include "fast_allocator.h"

#define BYTES_ALIGN(x, pad_mask)  (((x) + pad_mask) & (~pad_mask))

#define ADD_ALLOCATOR_TO_ARRAY(acontext, allocator, _pooled) \
	do { \
		(allocator)->index = acontext->allocator_array.count; \
		(allocator)->magic_number = rand();   \
		(allocator)->pooled = _pooled;        \
		acontext->allocator_array.allocators[ \
			acontext->allocator_array.count++] = allocator; \
	 /* logInfo("count: %d, magic_number: %d", acontext->allocator_array.count, \
            (allocator)->magic_number); */  \
	} while (0)


static int fast_allocator_malloc_trunk_check(const int alloc_bytes, void *args)
{
	struct fast_allocator_context *acontext;
	acontext = (struct fast_allocator_context *)args;
	if (acontext->alloc_bytes_limit == 0)
	{
		return 0;
	}

	if (acontext->alloc_bytes + alloc_bytes > acontext->alloc_bytes_limit)
	{
		return EOVERFLOW;
	}

	return acontext->allocator_array.malloc_bytes + alloc_bytes <=
		acontext->allocator_array.malloc_bytes_limit ? 0 : EOVERFLOW;
}

static void fast_allocator_malloc_trunk_notify_func(const int alloc_bytes, void *args)
{
	if (alloc_bytes > 0)
	{
		__sync_add_and_fetch(&((struct fast_allocator_context *)args)->
			allocator_array.malloc_bytes, alloc_bytes);
	}
	else
	{
		__sync_sub_and_fetch(&((struct fast_allocator_context *)args)->
			allocator_array.malloc_bytes, -1 * alloc_bytes);
	}
}

static int allocator_array_check_capacity(struct fast_allocator_context *acontext,
	const int allocator_count)
{
	int bytes;
    int target_count;
    int alloc_count;
	struct fast_allocator_info  **new_allocators;

    target_count = acontext->allocator_array.count + allocator_count;
	if (acontext->allocator_array.alloc >= target_count)
	{
		return 0;
	}
	if (acontext->allocator_array.alloc == 0)
	{
        if (target_count < 128)
        {
            alloc_count = 128;
        }
        else if (target_count < 256)
        {
            alloc_count = 256;
        }
        else if (target_count < 512)
        {
            alloc_count = 512;
        }
        else if (target_count < 1024)
        {
            alloc_count = 1024;
        }
        else
        {
            alloc_count = 2 * target_count;
        }
	}
	else
	{
        alloc_count = acontext->allocator_array.alloc;
		do
		{
			alloc_count *= 2;
		} while (alloc_count < target_count);
	}

	bytes = sizeof(struct fast_allocator_info *) * alloc_count;
	new_allocators = (struct fast_allocator_info **)fc_malloc(bytes);
	if (new_allocators == NULL)
	{
		return ENOMEM;
	}

	if (acontext->allocator_array.allocators != NULL)
	{
		memcpy(new_allocators, acontext->allocator_array.allocators,
			sizeof(struct fast_allocator_info *) *
			acontext->allocator_array.count);
		free(acontext->allocator_array.allocators);
	}
    acontext->allocator_array.alloc = alloc_count;
	acontext->allocator_array.allocators = new_allocators;
	return 0;
}

static int region_init(struct fast_allocator_context *acontext,
        const char *mblock_name_prefix, struct fast_mblock_object_callbacks
        *object_callbacks, struct fast_region_info *region)
{
    const int64_t alloc_elements_limit = 0;
	int result;
	int bytes;
	int element_size;
    struct fast_mblock_trunk_callbacks trunk_callbacks;
	struct fast_allocator_info *allocator;
    char *name;
    char name_buff[FAST_MBLOCK_NAME_SIZE];

	region->pad_mask = region->step - 1;
	region->count = (region->end - region->start) / region->step;
	bytes = sizeof(struct fast_allocator_info) * region->count;
	region->allocators = (struct fast_allocator_info *)fc_malloc(bytes);
	if (region->allocators == NULL)
	{
		return ENOMEM;
	}
	memset(region->allocators, 0, bytes);

	if ((result=allocator_array_check_capacity(acontext, region->count)) != 0)
	{
		return result;
	}

    if (region->count == 1) {
        if (region->start == 0) {
            region->step += acontext->extra_size;
        } else {
            region->start += acontext->extra_size;
        }
        region->end += acontext->extra_size;
    }

    trunk_callbacks.check_func = fast_allocator_malloc_trunk_check;
    trunk_callbacks.notify_func = fast_allocator_malloc_trunk_notify_func;
    name = name_buff;
	result = 0;
 	allocator = region->allocators;
	for (element_size = region->start + region->step;
         element_size <= region->end;
         element_size += region->step, allocator++)
    {
        if (mblock_name_prefix != NULL)
        {
            snprintf(name, FAST_MBLOCK_NAME_SIZE, "%s-%d",
                    mblock_name_prefix, element_size);
        }
        else
        {
            name = NULL;
        }

        trunk_callbacks.args = acontext;
		result = fast_mblock_init_ex2(&allocator->mblock, name, element_size,
                region->alloc_elements_once, alloc_elements_limit,
                object_callbacks, acontext->need_lock, &trunk_callbacks);
		if (result != 0)
		{
			break;
		}

		ADD_ALLOCATOR_TO_ARRAY(acontext, allocator, true);
	}

	return result;
}

static void region_destroy(struct fast_allocator_context *acontext,
	struct fast_region_info *region)
{
	int element_size;
	struct fast_allocator_info *allocator;

	allocator = region->allocators;
	for (element_size=region->start+region->step; element_size<=region->end;
		element_size+=region->step,allocator++)
	{
		fast_mblock_destroy(&allocator->mblock);
	}

	free(region->allocators);
	region->allocators = NULL;
}

int fast_allocator_init_ex(struct fast_allocator_context *acontext,
        const char *mblock_name_prefix, const int obj_size,
        struct fast_mblock_object_callbacks *object_callbacks,
        struct fast_region_info *regions, const int region_count,
        const int64_t alloc_bytes_limit, const double expect_usage_ratio,
        const int reclaim_interval, const bool need_lock)
{
	int result;
	int bytes;
	int previous_end;
	struct fast_region_info *pRegion;
	struct fast_region_info *region_end;

	srand(time(NULL));
	memset(acontext, 0, sizeof(*acontext));
	if (region_count <= 0)
	{
		return EINVAL;
	}

	bytes = sizeof(struct fast_region_info) * region_count;
	acontext->regions = (struct fast_region_info *)fc_malloc(bytes);
	if (acontext->regions == NULL)
	{
		return ENOMEM;
	}
	memcpy(acontext->regions, regions, bytes);
	acontext->region_count = region_count;
	acontext->alloc_bytes_limit = alloc_bytes_limit;
	if (expect_usage_ratio < 0.01 || expect_usage_ratio > 1.00)
	{
		acontext->allocator_array.expect_usage_ratio = 0.80;
	}
	else
	{
		acontext->allocator_array.expect_usage_ratio = expect_usage_ratio;
	}
	acontext->allocator_array.malloc_bytes_limit = alloc_bytes_limit /
		acontext->allocator_array.expect_usage_ratio;
	acontext->allocator_array.reclaim_interval = reclaim_interval;
	acontext->extra_size = sizeof(struct fast_allocator_wrapper) + obj_size;
	acontext->need_lock = need_lock;
	result = 0;
	previous_end = 0;
	region_end = acontext->regions + acontext->region_count;
	for (pRegion=acontext->regions; pRegion<region_end; pRegion++)
	{
		if (pRegion->start != previous_end)
		{
			logError("file: "__FILE__", line: %d, "
				"invalid start: %d != last end: %d",
				__LINE__, pRegion->start, previous_end);
			result = EINVAL;
			break;
		}
		if (pRegion->start >= pRegion->end)
		{
			logError("file: "__FILE__", line: %d, "
				"invalid start: %d >= end: %d",
				__LINE__, pRegion->start, pRegion->end);
			result = EINVAL;
			break;
		}
		if (pRegion->step <= 0)
		{
			logError("file: "__FILE__", line: %d, "
				"invalid step: %d <= 0",
				__LINE__, pRegion->step);
			result = EINVAL;
			break;
		}

        if ((pRegion->end - pRegion->start) / pRegion->step > 1)
        {
            if (!is_power2(pRegion->step))
            {
                logError("file: "__FILE__", line: %d, "
                        "invalid step: %d, expect power of 2",
                        __LINE__, pRegion->step);
                result = EINVAL;
                break;
            }
            if (pRegion->start % pRegion->step != 0)
            {
                logError("file: "__FILE__", line: %d, "
                        "invalid start: %d, must multiple of step: %d",
                        __LINE__, pRegion->start, pRegion->step);
                result = EINVAL;
                break;
            }
            if (pRegion->end % pRegion->step != 0)
            {
                logError("file: "__FILE__", line: %d, "
                        "invalid end: %d, must multiple of step: %d",
                        __LINE__, pRegion->end, pRegion->step);
                result = EINVAL;
                break;
            }
        }
		previous_end = pRegion->end;

		if ((result=region_init(acontext, mblock_name_prefix,
                        object_callbacks, pRegion)) != 0)
		{
			break;
		}
	}

	if (result != 0)
	{
		return result;
	}

	if ((result=allocator_array_check_capacity(acontext, 1)) != 0)
	{
		return result;
	}

	ADD_ALLOCATOR_TO_ARRAY(acontext, &acontext->
            allocator_array.malloc_allocator, false);

	/*
	logInfo("sizeof(struct fast_allocator_wrapper): %d, allocator_array count: %d",
		(int)sizeof(struct fast_allocator_wrapper), acontext->allocator_array.count);
	*/
	return result;
}

int fast_allocator_init(struct fast_allocator_context *acontext,
        const char *mblock_name_prefix, const int64_t alloc_bytes_limit,
        const double expect_usage_ratio, const int reclaim_interval,
        const bool need_lock)
{
#define DEFAULT_REGION_COUNT 5

    const int obj_size = 0;
    struct fast_region_info regions[DEFAULT_REGION_COUNT];

    FAST_ALLOCATOR_INIT_REGION(regions[0],     0,   256,    8, 4096);
    FAST_ALLOCATOR_INIT_REGION(regions[1],   256,  1024,   16, 1024);
    FAST_ALLOCATOR_INIT_REGION(regions[2],  1024,  4096,   64,  256);
    FAST_ALLOCATOR_INIT_REGION(regions[3],  4096, 16384,  256,   64);
    FAST_ALLOCATOR_INIT_REGION(regions[4], 16384, 65536, 1024,   16);

    return fast_allocator_init_ex(acontext, mblock_name_prefix, obj_size,
            NULL, regions, DEFAULT_REGION_COUNT, alloc_bytes_limit,
            expect_usage_ratio, reclaim_interval, need_lock);
}

void fast_allocator_destroy(struct fast_allocator_context *acontext)
{
	struct fast_region_info *pRegion;
	struct fast_region_info *region_end;

	if (acontext->regions != NULL)
	{
		region_end = acontext->regions + acontext->region_count;
		for (pRegion=acontext->regions; pRegion<region_end; pRegion++)
		{
			region_destroy(acontext, pRegion);
		}
		free(acontext->regions);
	}

	if (acontext->allocator_array.allocators != NULL)
	{
		free(acontext->allocator_array.allocators);
	}
	memset(acontext, 0, sizeof(*acontext));
}

static struct fast_allocator_info *get_allocator(struct fast_allocator_context
        *acontext, int *alloc_bytes)
{
	struct fast_region_info *pRegion;
	struct fast_region_info *region_end;

	region_end = acontext->regions + acontext->region_count;
	for (pRegion=acontext->regions; pRegion<region_end; pRegion++)
	{
		if (*alloc_bytes <= pRegion->end)
        {
            if (pRegion->count == 1) {
                *alloc_bytes = pRegion->allocators[0].mblock.info.element_size;
                return pRegion->allocators + 0;
            } else {
                *alloc_bytes = BYTES_ALIGN(*alloc_bytes, pRegion->pad_mask);
                return pRegion->allocators + ((*alloc_bytes -
                            pRegion->start) / pRegion->step) - 1;
            }
        }
	}

	return &acontext->allocator_array.malloc_allocator;
}

int fast_allocator_retry_reclaim(struct fast_allocator_context *acontext,
	int64_t *total_reclaim_bytes)
{
	int64_t malloc_bytes;
	int reclaim_count;
	int i;

	*total_reclaim_bytes = 0;
	if (acontext->allocator_array.last_reclaim_time +
		acontext->allocator_array.reclaim_interval > get_current_time())
	{
		return EAGAIN;
	}

	acontext->allocator_array.last_reclaim_time = get_current_time();
	malloc_bytes = acontext->allocator_array.malloc_bytes;
    /*
	logInfo("malloc_bytes: %"PRId64", ratio: %f", malloc_bytes,
        (double)acontext->alloc_bytes / (double)malloc_bytes);
        */

	if (malloc_bytes == 0 || (double)acontext->alloc_bytes /
		(double)malloc_bytes >= acontext->allocator_array.expect_usage_ratio)
	{
		return EAGAIN;
	}

	for (i=0; i<acontext->allocator_array.count; i++)
	{
		if (fast_mblock_reclaim(&acontext->allocator_array.
			allocators[i]->mblock, 0, &reclaim_count, NULL) == 0)
		{
			//logInfo("reclaim_count: %d", reclaim_count);
			*total_reclaim_bytes += reclaim_count *
				acontext->allocator_array.allocators[i]->
					mblock.info.trunk_size;
		}
	}

	return *total_reclaim_bytes > 0 ? 0 : EAGAIN;
}

void *fast_allocator_alloc(struct fast_allocator_context *acontext,
	const int bytes)
{
	int alloc_bytes;
	int64_t total_reclaim_bytes;
	struct fast_allocator_info *allocator_info;
	void *ptr;
	void *obj;

	if (bytes < 0)
	{
		return NULL;
	}

	alloc_bytes = acontext->extra_size + bytes;
	allocator_info = get_allocator(acontext, &alloc_bytes);
	if (allocator_info->pooled)
	{
		ptr = fast_mblock_alloc_object(&allocator_info->mblock);
		if (ptr == NULL)
		{
			if (acontext->allocator_array.reclaim_interval < 0)
			{
				return NULL;
			}
			if (fast_allocator_retry_reclaim(acontext, &total_reclaim_bytes) != 0)
			{
				return NULL;
			}
			//logInfo("reclaimed bytes: %"PRId64, total_reclaim_bytes);
			if (total_reclaim_bytes < allocator_info->mblock.info.trunk_size)
			{
				return NULL;
			}
			ptr = fast_mblock_alloc_object(&allocator_info->mblock);
			if (ptr == NULL)
			{
				return NULL;
			}
		}
        obj = (char *)ptr + sizeof(struct fast_allocator_wrapper);
	}
	else
	{
		if (fast_allocator_malloc_trunk_check(alloc_bytes, acontext) != 0)
		{
			return NULL;
		}
		ptr = fc_malloc(alloc_bytes);
		if (ptr == NULL)
		{
			return NULL;
		}
		fast_allocator_malloc_trunk_notify_func(alloc_bytes, acontext);

        obj = (char *)ptr + sizeof(struct fast_allocator_wrapper);
        if (acontext->allocator_array.allocators[0]->mblock.
                object_callbacks.init_func != NULL)
        {
            struct fast_mblock_man *mblock;
            mblock = &acontext->allocator_array.allocators[0]->mblock;
            mblock->object_callbacks.init_func(obj,
                    mblock->object_callbacks.args);
        }
	}

	((struct fast_allocator_wrapper *)ptr)->allocator_index =
        allocator_info->index;
	((struct fast_allocator_wrapper *)ptr)->magic_number =
        allocator_info->magic_number;
	((struct fast_allocator_wrapper *)ptr)->alloc_bytes = alloc_bytes;
	__sync_add_and_fetch(&acontext->alloc_bytes, alloc_bytes);
	return obj;
}

void fast_allocator_free(struct fast_allocator_context *acontext, void *obj)
{
	struct fast_allocator_wrapper *pWrapper;
	struct fast_allocator_info *allocator_info;
	void *ptr;

	if (obj == NULL)
	{
		return;
	}

	ptr = (char *)obj - sizeof(struct fast_allocator_wrapper);
	pWrapper = (struct fast_allocator_wrapper *)ptr;
	if (pWrapper->allocator_index < 0 || pWrapper->allocator_index >=
		acontext->allocator_array.count)
	{
		logError("file: "__FILE__", line: %d, "
				"invalid allocator index: %d",
				__LINE__, pWrapper->allocator_index);
		return;
	}

	allocator_info = acontext->allocator_array.
        allocators[pWrapper->allocator_index];
	if (pWrapper->magic_number != allocator_info->magic_number)
	{
		logError("file: "__FILE__", line: %d, "
				"invalid magic number: %d != %d",
				__LINE__, pWrapper->magic_number,
				allocator_info->magic_number);
		return;
	}

	__sync_sub_and_fetch(&acontext->alloc_bytes, pWrapper->alloc_bytes);
	pWrapper->allocator_index = -1;
	pWrapper->magic_number = 0;
	if (allocator_info->pooled)
	{
		fast_mblock_free_object(&allocator_info->mblock, ptr);
	}
	else
    {
        fast_allocator_malloc_trunk_notify_func(-1 *
                pWrapper->alloc_bytes, acontext);

        if (acontext->allocator_array.allocators[0]->mblock.
                object_callbacks.destroy_func != NULL)
        {
            struct fast_mblock_man *mblock;
            mblock = &acontext->allocator_array.allocators[0]->mblock;
            mblock->object_callbacks.destroy_func(obj,
                    mblock->object_callbacks.args);
        }
        free(ptr);
    }
}

char *fast_allocator_memdup(struct fast_allocator_context *acontext,
        const char *src, const int len)
{
    char *dest;
    dest = (char *)fast_allocator_alloc(acontext, len);
    if (dest == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__, len);
        return NULL;
    }

    memcpy(dest, src, len);
    return dest;
}
