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
 * Many threads look up, retain and release one BO's descriptor at once, so
 * lookups race the last release and two holders release together. A lookup
 * that retains a freed descriptor, or a final release that does not find
 * the count at 1, is a failure; ASan sees a use after free.
 *
 * SPDX-License-Identifier: MIT
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "rdna3_bo_map.h"

typedef struct obj {
    atomic_int refs;
    atomic_int dead;
} obj;

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
