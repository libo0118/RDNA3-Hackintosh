/*
 * Threaded host test of the BO mapping descriptor's release protocol
 * (common/rdna3_bo_map.h, RDNA3BoMemory::taggedRelease and
 * RDNA3Accel::drmBoMapDescriptor in kext/src/RDNA3Drm.cpp; ECC review round
 * 1 of DRIVER-PLAN N47 (b)).
 *
 * The kext's rules, modelled with pthreads and C11 atomics:
 * - a lookup (under the outer lock, fDrmLock) takes the leaf lock
 *   (fBoMapLock), reads the BO's weak link and retains what it finds;
 *   without a descriptor it makes one (one reference) and links it;
 * - a holder may retain again without any lock (an OSObject retain);
 * - a release takes the leaf lock and asks rd3_bomap_release_unlinks with
 *   the count before it: the last one unlinks, drops the lock and then
 *   decrements and frees; any other decrements under the lock.
 * - a new descriptor is charged (rdna3_drm_bomap_charge, under the outer
 *   lock) for itself; the free takes the outer lock and gives back exactly
 *   that descriptor's charge (drmBoMapRelease), possibly after a newer
 *   descriptor of the same BO was made and charged (ECC review round 2).
 * Many threads look up, retain and release one BO's descriptor at once, so
 * lookups race the last release and two holders release together. A lookup
 * that retains a freed descriptor, a final release that does not find the
 * count at 1, or a uid count that is not the number of live descriptors is
 * a failure; ASan sees a use after free.
 *
 * SPDX-License-Identifier: MIT
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdna3_bo_map.h"

typedef struct obj {
    atomic_int refs;
    atomic_int dead;
    rdna3_bomap_charge charge;
} obj;

/* the UAPI's device and BO, touched only under drm_lock */
static rdna3_drm_device dev;
static rdna3_drm_file file;
static rdna3_bo the_bo;
static int live;   /* descriptors made and not yet freed (drm_lock) */
static atomic_int quota_refused, bad_count;

/* drm_lock held: the uid's charge count is the number of live descriptors */
static void check_count(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < RD_BOMAP_OWNERS; i++)
        if (dev.bomap.o[i].used && dev.bomap.o[i].uid == 501)
            n = dev.bomap.o[i].n;
    if (n != (uint32_t)live || dev.bomap.n != (uint32_t)live)
        atomic_fetch_add(&bad_count, 1);
}

static pthread_mutex_t drm_lock = PTHREAD_MUTEX_INITIALIZER;   /* fDrmLock */
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;   /* fBoMapLock */
static void *link_ptr;                                         /* rdna3_bo::os_map */
static atomic_int made, freed, bad_retain, bad_final;

static obj *lookup(void)
{
    obj *o;
    pthread_mutex_lock(&drm_lock);
    pthread_mutex_lock(&map_lock);
    o = (obj *)rd3_bomap_lookup(&link_ptr);
    if (o) {
        if (atomic_load(&o->dead) || atomic_fetch_add(&o->refs, 1) < 1)
            atomic_fetch_add(&bad_retain, 1);
    }
    pthread_mutex_unlock(&map_lock);
    if (!o) {
        o = (obj *)calloc(1, sizeof(*o));
        if (!o)
            abort();
        if (rdna3_drm_bomap_charge(&dev, &file, &the_bo, &o->charge)) {
            atomic_fetch_add(&quota_refused, 1);
            free(o);
            pthread_mutex_unlock(&drm_lock);
            return NULL;
        }
        live++;
        check_count();
        atomic_store(&o->refs, 1);
        pthread_mutex_lock(&map_lock);
        rd3_bomap_link(&link_ptr, o);
        pthread_mutex_unlock(&map_lock);
        atomic_fetch_add(&made, 1);
    }
    pthread_mutex_unlock(&drm_lock);
    return o;
}

static void release(obj *o)
{
    pthread_mutex_lock(&map_lock);
    if (rd3_bomap_release_unlinks(&link_ptr, o, atomic_load(&o->refs), 1)) {
        pthread_mutex_unlock(&map_lock);
        if (atomic_fetch_sub(&o->refs, 1) != 1)
            atomic_fetch_add(&bad_final, 1);
        atomic_store(&o->dead, 1);
        pthread_mutex_lock(&drm_lock);   /* drmBoMapRelease */
        rdna3_drm_bomap_uncharge(&dev, &the_bo, &o->charge);
        live--;
        check_count();
        pthread_mutex_unlock(&drm_lock);
        free(o);
        atomic_fetch_add(&freed, 1);
        return;
    }
    atomic_fetch_sub(&o->refs, 1);
    pthread_mutex_unlock(&map_lock);
}

#define THREADS 8
#define ITERS   100000

static void *worker(void *arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u + 1;
    for (int i = 0; i < ITERS; i++) {
        obj *o = lookup();
        if (!o)
            continue;
        seed = seed * 1103515245u + 12345u;
        if (seed & 0x10000) {   /* a holder's own extra retain, no lock */
            atomic_fetch_add(&o->refs, 1);
            release(o);
        }
        release(o);
    }
    return NULL;
}

int main(void)
{
    pthread_t t[THREADS];
    int i, failures = 0;
    memset(&dev, 0, sizeof(dev));
    dev.vis_vram_size = 256ull << 20;
    file.dev = &dev;
    file.owner = 501;
    the_bo.size = 4096;
    for (i = 0; i < THREADS; i++)
        if (pthread_create(&t[i], NULL, worker, (void *)(uintptr_t)(i + 1))) {
            fprintf(stderr, "test_bo_map_race: pthread_create failed\n");
            return 1;
        }
    for (i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
    if (atomic_load(&bad_retain) || atomic_load(&bad_final)) {
        fprintf(stderr, "test_bo_map_race: %d retain(s) of a dying descriptor, %d final release(s) not at 1\n",
                atomic_load(&bad_retain), atomic_load(&bad_final));
        failures++;
    }
    {
        /* every charge given back: the uid's count matched the live descriptors throughout (checked below
         * at the end; a lost or doubled uncharge leaves it off) */
        uint32_t n501 = 0;
        for (i = 0; i < RD_BOMAP_OWNERS; i++)
            if (dev.bomap.o[i].used && dev.bomap.o[i].uid == 501)
                n501 = dev.bomap.o[i].n;
        if (live != 0 || n501 != 0 || dev.bomap.n != 0 || dev.bomap.bytes != 0 || the_bo.map_gen != 0 ||
            atomic_load(&quota_refused) || atomic_load(&bad_count)) {
            fprintf(stderr, "test_bo_map_race: %d live, uid 501 holds %u, device %u / %llu bytes, BO gen %llu, %d "
                    "quota refusals, %d count mismatches\n", live, n501, dev.bomap.n, (unsigned long long)dev.bomap.bytes,
                    (unsigned long long)the_bo.map_gen, atomic_load(&quota_refused), atomic_load(&bad_count));
            failures++;
        }
    }
    if (atomic_load(&made) != atomic_load(&freed) || link_ptr) {
        fprintf(stderr, "test_bo_map_race: %d made, %d freed, link %p\n", atomic_load(&made), atomic_load(&freed),
                link_ptr);
        failures++;
    }
    if (atomic_load(&made) < 2) {   /* the last release and a new descriptor did race */
        fprintf(stderr, "test_bo_map_race: only %d descriptor(s) made: the race was never exercised\n",
                atomic_load(&made));
        failures++;
    }
    if (failures)
        return 1;
    printf("test_bo_map_race: ok (%d descriptors over %d lookups)\n", atomic_load(&made), THREADS * ITERS);
    return 0;
}
