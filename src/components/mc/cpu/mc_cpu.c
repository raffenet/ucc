/**
 * Copyright (C) Mellanox Technologies Ltd. 2020-2021.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "mc_cpu.h"
#include "reduce/mc_cpu_reduce.h"
#include "utils/ucc_malloc.h"
#include <sys/types.h>

static ucc_config_field_t ucc_mc_cpu_config_table[] = {
    {"", "", NULL, ucc_offsetof(ucc_mc_cpu_config_t, super),
     UCC_CONFIG_TYPE_TABLE(ucc_mc_config_table)},

    {"MPOOL_ELEM_SIZE", "1Mb", "The size of each element in mc cpu mpool",
     ucc_offsetof(ucc_mc_cpu_config_t, mpool_elem_size),
     UCC_CONFIG_TYPE_MEMUNITS},

    {"MPOOL_MAX_ELEMS", "8", "The max amount of elements in mc cpu mpool",
     ucc_offsetof(ucc_mc_cpu_config_t, mpool_max_elems), UCC_CONFIG_TYPE_UINT},

    {NULL}

};

static ucc_status_t ucc_mc_cpu_init(const ucc_mc_params_t *mc_params)
{
    ucc_strncpy_safe(ucc_mc_cpu.super.config->log_component.name,
                     ucc_mc_cpu.super.super.name,
                     sizeof(ucc_mc_cpu.super.config->log_component.name));
    ucc_mc_cpu.thread_mode = mc_params->thread_mode;
    // lock assures single mpool initiation when multiple threads concurrently execute
    // different collective operations thus concurrently entering init function.
    ucc_spinlock_init(&ucc_mc_cpu.mpool_init_spinlock, 0);
    return UCC_OK;
}

static ucc_status_t ucc_mc_cpu_mem_alloc(ucc_mc_buffer_header_t **h_ptr,
                                         size_t                   size)
{
    size_t        size_with_h = size + sizeof(ucc_mc_buffer_header_t);
    ucc_mc_buffer_header_t *h =
        (ucc_mc_buffer_header_t *)ucc_malloc(size_with_h, "mc cpu");
    if (ucc_unlikely(!h)) {
        mc_error(&ucc_mc_cpu.super, "failed to allocate %zd bytes",
                 size_with_h);
        return UCC_ERR_NO_MEMORY;
    }
    h->from_pool = 0;
    h->addr      = PTR_OFFSET(h, sizeof(ucc_mc_buffer_header_t));
    h->mt        = UCC_MEMORY_TYPE_HOST;
    *h_ptr       = h;
    mc_debug(&ucc_mc_cpu.super, "MC allocated %ld bytes with ucc_malloc", size);
    return UCC_OK;
}

static ucc_status_t ucc_mc_cpu_mem_pool_alloc(ucc_mc_buffer_header_t **h_ptr,
                                              size_t                   size)
{
    ucc_mc_buffer_header_t *h = NULL;
    if (size <= MC_CPU_CONFIG->mpool_elem_size) {
        h = (ucc_mc_buffer_header_t *)ucc_mpool_get(&ucc_mc_cpu.mpool);
    }
    if (!h) {
        // Slow path
        return ucc_mc_cpu_mem_alloc(h_ptr, size);
    }
    mc_debug(&ucc_mc_cpu.super, "MC allocated %ld bytes from cpu mpool", size);
    *h_ptr = h;
    return UCC_OK;
}

static ucc_status_t ucc_mc_cpu_chunk_alloc(ucc_mpool_t *mp, //NOLINT
                                           size_t *size_p,
                                           void **chunk_p)
{
    *chunk_p = ucc_malloc(*size_p, "mc cpu");
    if (!*chunk_p) {
        mc_error(&ucc_mc_cpu.super, "failed to allocate %zd bytes", *size_p);
        return UCC_ERR_NO_MEMORY;
    }
    return UCC_OK;
}

static void ucc_mc_cpu_chunk_init(ucc_mpool_t *mp, //NOLINT
                                  void *obj, void *chunk) //NOLINT
{
    ucc_mc_buffer_header_t *h = (ucc_mc_buffer_header_t *)obj;
    h->from_pool              = 1;
    h->addr                   = PTR_OFFSET(h, sizeof(ucc_mc_buffer_header_t));
    h->mt                     = UCC_MEMORY_TYPE_HOST;
}

static void ucc_mc_cpu_chunk_release(ucc_mpool_t *mp, void *chunk) //NOLINT
{
    ucc_free(chunk);
}

static ucc_mpool_ops_t ucc_mc_ops = {.chunk_alloc   = ucc_mc_cpu_chunk_alloc,
                                     .chunk_release = ucc_mc_cpu_chunk_release,
                                     .obj_init      = ucc_mc_cpu_chunk_init,
                                     .obj_cleanup   = NULL};

static ucc_status_t ucc_mc_cpu_mem_free(ucc_mc_buffer_header_t *h_ptr)
{
    ucc_free(h_ptr);
    return UCC_OK;
}

static ucc_status_t ucc_mc_cpu_mem_pool_free(ucc_mc_buffer_header_t *h_ptr)
{
    if (!h_ptr->from_pool) {
        return ucc_mc_cpu_mem_free(h_ptr);
    }
    ucc_mpool_put(h_ptr);
    return UCC_OK;
}

static ucc_status_t
ucc_mc_cpu_mem_pool_alloc_with_init(ucc_mc_buffer_header_t **h_ptr, size_t size)
{
    // lock assures single mpool initiation when multiple threads concurrently
    // execute different collective operations each entering init function.
    ucc_spin_lock(&ucc_mc_cpu.mpool_init_spinlock);

    if (MC_CPU_CONFIG->mpool_max_elems == 0) {
        ucc_mc_cpu.super.ops.mem_alloc = ucc_mc_cpu_mem_alloc;
        ucc_mc_cpu.super.ops.mem_free  = ucc_mc_cpu_mem_free;
        ucc_spin_unlock(&ucc_mc_cpu.mpool_init_spinlock);
        return ucc_mc_cpu_mem_alloc(h_ptr, size);
    }

    if (!ucc_mc_cpu.mpool_init_flag) {
        ucc_status_t status = ucc_mpool_init(
            &ucc_mc_cpu.mpool, 0,
            sizeof(ucc_mc_buffer_header_t) + MC_CPU_CONFIG->mpool_elem_size, 0,
            UCC_CACHE_LINE_SIZE, 1, MC_CPU_CONFIG->mpool_max_elems, &ucc_mc_ops,
            ucc_mc_cpu.thread_mode, "mc cpu mpool buffers");
        if (ucc_unlikely(status != UCC_OK)) {
            ucc_spin_unlock(&ucc_mc_cpu.mpool_init_spinlock);
            return status;
        }
        ucc_mc_cpu.super.ops.mem_alloc = ucc_mc_cpu_mem_pool_alloc;
        ucc_mc_cpu.mpool_init_flag     = 1;
    }
    ucc_spin_unlock(&ucc_mc_cpu.mpool_init_spinlock);
    return ucc_mc_cpu_mem_pool_alloc(h_ptr, size);
}

static ucc_status_t ucc_mc_cpu_reduce_multi(const void *src1, const void *src2,
                                            void *dst, size_t size,
                                            size_t count, size_t stride,
                                            ucc_datatype_t     dt,
                                            ucc_reduction_op_t op)
{
    switch(dt) {
    case UCC_DT_INT8:
        return ucc_mc_cpu_reduce_multi_int8(src1, src2, dst, size, count,
                                            stride, op);
    case UCC_DT_INT16:
        return ucc_mc_cpu_reduce_multi_int16(src1, src2, dst, size, count,
                                             stride, op);
    case UCC_DT_INT32:
        return ucc_mc_cpu_reduce_multi_int32(src1, src2, dst, size, count,
                                             stride, op);
    case UCC_DT_INT64:
        return ucc_mc_cpu_reduce_multi_int64(src1, src2, dst, size, count,
                                             stride, op);
    case UCC_DT_UINT8:
        return ucc_mc_cpu_reduce_multi_uint8(src1, src2, dst, size, count,
                                             stride, op);
    case UCC_DT_UINT16:
        return ucc_mc_cpu_reduce_multi_uint16(src1, src2, dst, size, count,
                                              stride, op);
    case UCC_DT_UINT32:
        return ucc_mc_cpu_reduce_multi_uint32(src1, src2, dst, size, count,
                                              stride, op);
    case UCC_DT_UINT64:
        return ucc_mc_cpu_reduce_multi_uint64(src1, src2, dst, size, count,
                                              stride, op);
    case UCC_DT_FLOAT32:
        ucc_assert(4 == sizeof(float));
        return ucc_mc_cpu_reduce_multi_float(src1, src2, dst, size, count,
                                             stride, op);
    case UCC_DT_FLOAT64:
        ucc_assert(8 == sizeof(double));
        return ucc_mc_cpu_reduce_multi_double(src1, src2, dst, size, count,
                                              stride, op);
    default:
        mc_error(&ucc_mc_cpu.super, "unsupported reduction type (%d)", dt);
        return UCC_ERR_NOT_SUPPORTED;
    }
    return UCC_OK;
}

static ucc_status_t ucc_mc_cpu_reduce(const void *src1, const void *src2,
                                      void *dst, size_t count,
                                      ucc_datatype_t dt, ucc_reduction_op_t op)
{
    return ucc_mc_cpu_reduce_multi(src1, src2, dst, 1, count, 0, dt, op);
}

static ucc_status_t ucc_mc_cpu_memcpy(void *dst, const void *src, size_t len,
                                      ucc_memory_type_t dst_mem, //NOLINT
                                      ucc_memory_type_t src_mem) //NOLINT
{
    ucc_assert((dst_mem == UCC_MEMORY_TYPE_HOST) &&
               (src_mem == UCC_MEMORY_TYPE_HOST));
    memcpy(dst, src, len);
    return UCC_OK;
}

static ucc_status_t ucc_mc_cpu_mem_query(const void *ptr, //NOLINT
                                         ucc_mem_attr_t *mem_attr) //NOLINT
{
    /* not supposed to be used */
    mc_error(&ucc_mc_cpu.super, "host memory component shouldn't be used for"
                                "mem type detection");
    return UCC_ERR_NOT_SUPPORTED;
}

ucc_status_t ucc_ee_cpu_task_post(void *ee_context, //NOLINT
                                  void **ee_req)
{
    *ee_req = NULL;
    return UCC_OK;
}

ucc_status_t ucc_ee_cpu_task_query(void *ee_req) //NOLINT
{
    return UCC_OK;
}

ucc_status_t ucc_ee_cpu_task_end(void *ee_req) //NOLINT
{
    return UCC_OK;
}

ucc_status_t ucc_ee_cpu_create_event(void **event)
{
    *event = NULL;
    return UCC_OK;
}

ucc_status_t ucc_ee_cpu_destroy_event(void *event) //NOLINT
{
    return UCC_OK;
}

ucc_status_t ucc_ee_cpu_event_post(void *ee_context, //NOLINT
                                   void *event) //NOLINT
{
    return UCC_OK;
}

ucc_status_t ucc_ee_cpu_event_test(void *event) //NOLINT
{
    return UCC_OK;
}

static ucc_status_t ucc_mc_cpu_finalize()
{
    if (ucc_mc_cpu.mpool_init_flag) {
        ucc_mpool_cleanup(&ucc_mc_cpu.mpool, 1);
        ucc_mc_cpu.mpool_init_flag     = 0;
        ucc_mc_cpu.super.ops.mem_alloc = ucc_mc_cpu_mem_pool_alloc_with_init;
    }
    ucc_spinlock_destroy(&ucc_mc_cpu.mpool_init_spinlock);
    return UCC_OK;
}

ucc_mc_cpu_t ucc_mc_cpu = {
    .super.super.name       = "cpu mc",
    .super.ref_cnt          = 0,
    .super.type             = UCC_MEMORY_TYPE_HOST,
    .super.ee_type          = UCC_EE_CPU_THREAD,
    .super.init             = ucc_mc_cpu_init,
    .super.finalize         = ucc_mc_cpu_finalize,
    .super.ops.mem_query    = ucc_mc_cpu_mem_query,
    .super.ops.mem_alloc    = ucc_mc_cpu_mem_pool_alloc_with_init,
    .super.ops.mem_free     = ucc_mc_cpu_mem_pool_free,
    .super.ops.reduce       = ucc_mc_cpu_reduce,
    .super.ops.reduce_multi = ucc_mc_cpu_reduce_multi,
    .super.ops.memcpy       = ucc_mc_cpu_memcpy,
    .super.config_table =
        {
            .name   = "CPU memory component",
            .prefix = "MC_CPU_",
            .table  = ucc_mc_cpu_config_table,
            .size   = sizeof(ucc_mc_cpu_config_t),
        },
    .super.ee_ops.ee_task_post     = ucc_ee_cpu_task_post,
    .super.ee_ops.ee_task_query    = ucc_ee_cpu_task_query,
    .super.ee_ops.ee_task_end      = ucc_ee_cpu_task_end,
    .super.ee_ops.ee_create_event  = ucc_ee_cpu_create_event,
    .super.ee_ops.ee_destroy_event = ucc_ee_cpu_destroy_event,
    .super.ee_ops.ee_event_post    = ucc_ee_cpu_event_post,
    .super.ee_ops.ee_event_test    = ucc_ee_cpu_event_test,
    .mpool_init_flag               = 0,
};

UCC_CONFIG_REGISTER_TABLE_ENTRY(&ucc_mc_cpu.super.config_table,
                                &ucc_config_global_list);
