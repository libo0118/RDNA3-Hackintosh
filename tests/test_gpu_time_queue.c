/* The Metal plugin's per-process IOAccelCommandQueue (metal/plugin/nvmtl_gpu_time_queue.h, DRIVER-PLAN N49 open
 * item 1): the knob, every failure step with its IOReturn, a queue that does not start closed again, the success
 * path keeping the connection. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include "nvmtl_gpu_time_queue.h"
#include "nvmtl_log_failure.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

struct fake {
    int32_t open_ret, add_ret, close_ret;
    uint32_t give_conn;
    unsigned opens, adds, closes;
    uint32_t open_service, open_type, add_conn, add_client, closed_conn;
};
static int32_t f_open(void *ctx, uint32_t service, uint32_t type, uint32_t *conn)
{
    struct fake *f = ctx;
    f->opens++; f->open_service = service; f->open_type = type;
    *conn = f->give_conn;
    return f->open_ret;
}
static int32_t f_add(void *ctx, uint32_t conn, uint32_t client)
{
    struct fake *f = ctx;
    f->adds++; f->add_conn = conn; f->add_client = client;
    return f->add_ret;
}
static int32_t f_close(void *ctx, uint32_t conn)
{
    struct fake *f = ctx;
    f->closes++; f->closed_conn = conn;
    return f->close_ret;
}

int main(void)
{
    /* the knob: only "0" switches it off */
    CHECK(nvmtl_gpuq_enabled(NULL));
    CHECK(nvmtl_gpuq_enabled("1"));
    CHECK(nvmtl_gpuq_enabled(""));
    CHECK(nvmtl_gpuq_enabled("00"));
    CHECK(!nvmtl_gpuq_enabled("0"));

    /* success: open type 8 on the accelerator, add the shared connection, keep the queue */
    struct fake f = { 0, 0, 0, 0x1234u, 0, 0, 0, 0, 0, 0, 0, 0 };
    nvmtl_gpuq_ops ops = { f_open, f_add, f_close, &f };
    nvmtl_gpuq_result r = nvmtl_gpuq_open(&ops, 0x55u, 0x77u);
    CHECK(r.step == NVMTL_GPUQ_OPENED && r.ret == 0 && r.conn == 0x1234u);
    CHECK(f.opens == 1 && f.open_service == 0x55u && f.open_type == 8u);
    CHECK(f.adds == 1 && f.add_conn == 0x1234u && f.add_client == 0x77u);
    CHECK(f.closes == 0);

    /* IOConnectAddClient refused: the queue is closed again and the IOReturn kept */
    struct fake g = { 0, (int32_t)0xe00002c2, 0, 0x99u, 0, 0, 0, 0, 0, 0, 0, 0 };
    ops.ctx = &g;
    r = nvmtl_gpuq_open(&ops, 0x55u, 0x77u);
    CHECK(r.step == NVMTL_GPUQ_ADD_FAILED && r.ret == (int32_t)0xe00002c2 && r.conn == 0);
    CHECK(g.closes == 1 && g.closed_conn == 0x99u && r.close_ret == 0);

    /* IOServiceOpen refused: nothing to close, the IOReturn kept, no AddClient */
    struct fake h = { (int32_t)0xe00002be, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    ops.ctx = &h;
    r = nvmtl_gpuq_open(&ops, 0x55u, 0x77u);
    CHECK(r.step == NVMTL_GPUQ_OPEN_FAILED && r.ret == (int32_t)0xe00002be);
    CHECK(h.adds == 0 && h.closes == 0);

    /* "success" without a connection is not a queue; a failure that still gave a connection closes it */
    struct fake z = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    ops.ctx = &z;
    r = nvmtl_gpuq_open(&ops, 0x55u, 0x77u);
    CHECK(r.step == NVMTL_GPUQ_OPEN_FAILED && r.ret == -1 && z.adds == 0 && z.closes == 0);
    struct fake y = { (int32_t)0xe00002bc, 0, 0, 0x42u, 0, 0, 0, 0, 0, 0, 0, 0 };
    ops.ctx = &y;
    r = nvmtl_gpuq_open(&ops, 0x55u, 0x77u);
    CHECK(r.step == NVMTL_GPUQ_OPEN_FAILED && y.closes == 1 && y.closed_conn == 0x42u && y.adds == 0);

    /* missing pieces: no call is made */
    struct fake n = { 0, 0, 0, 1u, 0, 0, 0, 0, 0, 0, 0, 0 };
    ops.ctx = &n;
    CHECK(nvmtl_gpuq_open(&ops, 0, 0x77u).step == NVMTL_GPUQ_NO_ACCEL);
    CHECK(nvmtl_gpuq_open(&ops, 0x55u, 0).step == NVMTL_GPUQ_NO_SHARED);
    CHECK(nvmtl_gpuq_open(NULL, 0x55u, 0x77u).step == NVMTL_GPUQ_NO_API);
    nvmtl_gpuq_ops half = { f_open, NULL, f_close, &n };
    CHECK(nvmtl_gpuq_open(&half, 0x55u, 0x77u).step == NVMTL_GPUQ_NO_API);
    CHECK(n.opens == 0 && n.adds == 0 && n.closes == 0);

    /* the close note: only a connection the failure left, and a failed close says so */
    char nb[96];
    nvmtl_gpuq_result c0 = { NVMTL_GPUQ_ADD_FAILED, (int32_t)0xe00002c2, 0, 0, 1 };
    CHECK(!strcmp(nvmtl_gpuq_close_note(&c0, nb, sizeof nb), " (the queue was closed again)"));
    nvmtl_gpuq_result c1 = { NVMTL_GPUQ_ADD_FAILED, (int32_t)0xe00002c2, 0, (int32_t)0xe00002c0, 1 };
    CHECK(!strcmp(nvmtl_gpuq_close_note(&c1, nb, sizeof nb), " (closing it FAILED too: IOReturn 0xe00002c0)"));
    nvmtl_gpuq_result c2 = { NVMTL_GPUQ_OPEN_FAILED, (int32_t)0xe00002be, 0, 0, 0 };
    CHECK(!strcmp(nvmtl_gpuq_close_note(&c2, nb, sizeof nb), ""));
    nvmtl_gpuq_result c3 = { NVMTL_GPUQ_OPEN_FAILED, (int32_t)0xe00002bc, 0, 0, 1 };
    CHECK(!strcmp(nvmtl_gpuq_close_note(&c3, nb, sizeof nb), " (the queue was closed again)"));
    struct fake w = { 0, (int32_t)0xe00002c2, (int32_t)0xe00002c0, 0x31u, 0, 0, 0, 0, 0, 0, 0, 0 };
    ops.ctx = &w;
    r = nvmtl_gpuq_open(&ops, 0x55u, 0x77u);
    CHECK(r.step == NVMTL_GPUQ_ADD_FAILED && r.closed && r.close_ret == (int32_t)0xe00002c0);

    /* one more try only where the next device could see it differently */
    CHECK(nvmtl_gpuq_retry_ok(NVMTL_GPUQ_NO_SHARED) && nvmtl_gpuq_retry_ok(NVMTL_GPUQ_OPEN_FAILED));
    CHECK(!nvmtl_gpuq_retry_ok(NVMTL_GPUQ_ADD_FAILED) && !nvmtl_gpuq_retry_ok(NVMTL_GPUQ_NO_API));
    CHECK(!nvmtl_gpuq_retry_ok(NVMTL_GPUQ_OPENED) && !nvmtl_gpuq_retry_ok(NVMTL_GPUQ_OFF) && !nvmtl_gpuq_retry_ok(NVMTL_GPUQ_NO_ACCEL));

    /* a release build keeps the line either way (the syslog filter), opened as well as failed */
    CHECK(nvmtl_log_failure("GPU time queue: IOAccelCommandQueue opened for Chess (pid 812; connection 0x2a03)"));
    CHECK(nvmtl_log_failure("GPU time queue: off (NVMTL_GPUTIME_QUEUE=0) for Maps (pid 903)"));

    /* every step has a name for the log line */
    for (int s = NVMTL_GPUQ_OPENED; s <= NVMTL_GPUQ_ADD_FAILED; s++) CHECK(nvmtl_gpuq_step_name(s)[0] != '?');
    CHECK(nvmtl_gpuq_step_name(99)[0] == '?');

    if (failures) { fprintf(stderr, "test_gpu_time_queue: %d failure(s)\n", failures); return 1; }
    printf("test_gpu_time_queue: ok\n");
    return 0;
}
