/*
 * nvmtl_gpu_time_queue.h - one IOAccelCommandQueue per Metal process, so the kernel can show the process's GPU time
 * (DRIVER-PLAN N49 open item 1). SPDX-License-Identifier: MIT
 *
 * Activity Monitor's per-process % GPU / GPU Time is SafeEjectGPU's sum of the "accumulatedGPUTime" of the
 * IOAccelSubmitter2 entries (IOAccelContext2 and its kinds, IOAccelCommandQueue) under the accelerator whose
 * IOUserClientCreator names the pid. The accelerator (accel/rdna3-accel.cpp, rd3AccelPerfCredit) adds each process's
 * ring busy time to the first such entry of the pid. A Metal app on this plugin had none: MTLIOAccelDevice opens
 * IOAccelDevice2 + IOAccelSharedUserClient2 only, and Apple's MTLIOAccelCommandQueue (which opens the queue) is not
 * used, NVMTLCommandQueue runs on Vulkan.
 *
 * What Apple's Metal does per MTLCommandQueue (macOS 26.4.1, read from the binaries on the owner's VM):
 *   -[MTLIOAccelCommandQueue initWithDevice:descriptor:]: IOAccelCommandQueueCreateWithQoS([device sharedRef], qos);
 *   IOAccelCommandQueueCreateWithQoS (IOAccelerator.framework): IOServiceOpen(accelerator, mach_task_self(), 8, &c),
 *     IOConnectAddClient(c, the shared user client's connection), then a notification port (selector 0, block fences)
 *     and the process path (selector 5);
 *   IOGraphicsAccelerator2::newUserClient: type 8 -> newCommandQueue() (the family's default: a plain
 *     IOAccelCommandQueue), IOAccelCommandQueue::init, attach(accelerator) - no start;
 *   IOAccelCommandQueue::connectClient(IOAccelSharedUserClient2 of the same accelerator and task): start(), which is
 *     IOAccelSubmitter2::start (creates the 64-bit accumulatedGPUTime / lastSubmittedTime) + commandQueueStart.
 * The plugin does the first two steps only: it submits nothing through the queue, so it needs no notification port.
 * Selector 5 (s_set_get_info, read from the family) copies the 1 KiB process path into the submitter (+0xd8, read
 * only by IOGraphicsAccelerator2::reportGPURestart to name the process in a family GPU-restart report), sets the QoS
 * (commandQueueStart already set the default, 2, which IOAccelCommandQueueCreate passes) and returns the trace object
 * ID: no registry property, nothing Activity Monitor or our report reads, so it is left out (one kernel call less in
 * WindowServer's device creation).
 *
 * Plain C, no IOKit types: tests/test_gpu_time_queue.c drives it with fake calls; NVMTLDevice.m passes the real ones
 * (IOServiceOpen / IOConnectAddClient / IOServiceClose, found with dlsym: the plugin does not link IOKit).
 */
#ifndef NVMTL_GPU_TIME_QUEUE_H
#define NVMTL_GPU_TIME_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* IOGraphicsAccelerator2::newUserClient's switch: 0 surface, 1/3/7 contexts, 2 2D context, 4 display pipe, 5 IOAccelDevice2,
 * 6 IOAccelSharedUserClient2, 8 IOAccelCommandQueue, 9 GL drawable (0x20 Metal surface, 0x21 memory info) */
#define NVMTL_GPUQ_CLIENT_TYPE 8u

enum nvmtl_gpuq_step {
    NVMTL_GPUQ_OPENED = 0,     /* the queue is open and started: an IOAccelSubmitter2 of this process under the accelerator */
    NVMTL_GPUQ_OFF,            /* NVMTL_GPUTIME_QUEUE=0                                                                    */
    NVMTL_GPUQ_NO_API,         /* IOServiceOpen / IOConnectAddClient / IOServiceClose not found                           */
    NVMTL_GPUQ_NO_ACCEL,       /* no accelerator port                                                                     */
    NVMTL_GPUQ_NO_SHARED,      /* the device has no IOAccelSharedUserClient2 connection (IOAccelSharedGetConnect 0)       */
    NVMTL_GPUQ_OPEN_FAILED,    /* IOServiceOpen(type 8) refused, or gave no connection                                    */
    NVMTL_GPUQ_ADD_FAILED      /* IOConnectAddClient refused (the kernel's connectClient / start): the queue was closed  */
};

typedef struct {
    int32_t (*service_open)(void *ctx, uint32_t service, uint32_t type, uint32_t *conn);   /* IOServiceOpen(service, mach_task_self(), type) */
    int32_t (*add_client)(void *ctx, uint32_t conn, uint32_t client);                      /* IOConnectAddClient */
    int32_t (*close)(void *ctx, uint32_t conn);                                            /* IOServiceClose */
    void *ctx;
} nvmtl_gpuq_ops;

typedef struct {
    int step;            /* enum nvmtl_gpuq_step                                     */
    int32_t ret;         /* the IOReturn of the failing call (0 when none failed)    */
    uint32_t conn;       /* the open queue's connection (NVMTL_GPUQ_OPENED only)     */
    int32_t close_ret;   /* what IOServiceClose answered for a connection the failure left */
    int closed;          /* IOServiceClose was called (ADD_FAILED, or OPEN_FAILED with a connection) */
} nvmtl_gpuq_result;

/* the knob NVMTL_GPUTIME_QUEUE: a line of rdna3mtl.conf, or the environment of a development build (a release build
 * clears every NVMTL_ variable it inherits, so there only the conf file works). "0" switches the queue off, anything
 * else (or unset) leaves it on */
static inline int nvmtl_gpuq_enabled(const char *knob)
{
    return !(knob && knob[0] == '0' && knob[1] == 0);
}

/* open and start the queue: IOServiceOpen(accel, type 8), then IOConnectAddClient(queue, shared). A queue that does not
 * start is closed again: a half-made queue would be an IOAccelSubmitter2 without accumulatedGPUTime. */
static inline nvmtl_gpuq_result nvmtl_gpuq_open(const nvmtl_gpuq_ops *ops, uint32_t accel, uint32_t shared_conn)
{
    nvmtl_gpuq_result r = { NVMTL_GPUQ_OPENED, 0, 0, 0, 0 };
    if (!ops || !ops->service_open || !ops->add_client || !ops->close) { r.step = NVMTL_GPUQ_NO_API; return r; }
    if (!accel) { r.step = NVMTL_GPUQ_NO_ACCEL; return r; }
    if (!shared_conn) { r.step = NVMTL_GPUQ_NO_SHARED; return r; }
    uint32_t c = 0;
    const int32_t o = ops->service_open(ops->ctx, accel, NVMTL_GPUQ_CLIENT_TYPE, &c);
    if (o != 0 || !c) {
        r.step = NVMTL_GPUQ_OPEN_FAILED;
        r.ret = o;
        if (o == 0 && c == 0) r.ret = -1;     /* "success" with no connection: never seen, but not a queue */
        else if (o != 0 && c) { r.close_ret = ops->close(ops->ctx, c); r.closed = 1; }   /* a connection with a failure: give it back */
        return r;
    }
    const int32_t a = ops->add_client(ops->ctx, c, shared_conn);
    if (a != 0) {
        r.step = NVMTL_GPUQ_ADD_FAILED;
        r.ret = a;
        r.close_ret = ops->close(ops->ctx, c);
        r.closed = 1;
        return r;
    }
    r.conn = c;
    return r;
}

/* a failure worth one more try at the process's next Metal device (the shared connection or the accelerator may not
 * have been ready); a refused start (ADD_FAILED) or a missing API would fail the same way again */
static inline int nvmtl_gpuq_retry_ok(int step)
{
    return step == NVMTL_GPUQ_NO_SHARED || step == NVMTL_GPUQ_OPEN_FAILED;
}

/* what became of a connection the failure left: "" (none), " (the queue was closed again)" (IOServiceClose 0), or
 * " (closing it FAILED too: IOReturn 0x...)" - the half-made queue may still be there */
static inline const char *nvmtl_gpuq_close_note(const nvmtl_gpuq_result *r, char *buf, size_t cap)
{
    if (!buf || !cap) return "";
    buf[0] = 0;
    const int closed = r->step == NVMTL_GPUQ_ADD_FAILED || (r->step == NVMTL_GPUQ_OPEN_FAILED && r->closed);
    if (!closed) return buf;
    if (r->close_ret == 0) snprintf(buf, cap, " (the queue was closed again)");
    else snprintf(buf, cap, " (closing it FAILED too: IOReturn 0x%08x)", (unsigned)r->close_ret);
    return buf;
}

static inline const char *nvmtl_gpuq_step_name(int step)
{
    switch (step) {
    case NVMTL_GPUQ_OPENED:      return "opened";
    case NVMTL_GPUQ_OFF:         return "off (NVMTL_GPUTIME_QUEUE=0)";
    case NVMTL_GPUQ_NO_API:      return "no IOServiceOpen / IOConnectAddClient / IOServiceClose in the process";
    case NVMTL_GPUQ_NO_ACCEL:    return "no accelerator port";
    case NVMTL_GPUQ_NO_SHARED:   return "the device has no shared user client connection (IOAccelSharedGetConnect)";
    case NVMTL_GPUQ_OPEN_FAILED: return "IOServiceOpen(accelerator, type 8 = IOAccelCommandQueue)";
    case NVMTL_GPUQ_ADD_FAILED:  return "IOConnectAddClient(queue, shared): the kernel's connectClient / start";
    default:                     return "?";
    }
}

#endif
