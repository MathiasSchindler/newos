#include "concurrency.h"

static RtTaskPool test_pool;
static unsigned int test_stage;
static unsigned int test_parked;
static unsigned int test_stale_context;
static unsigned int test_completed;
static unsigned int test_violation;
static void *test_expected_address;

static void taskclaimtest_before_compare(void *object, void *expected_address, unsigned long long desired);

/* Schedule the ABA interleaving in the real worker loop, without production hooks.
   The self-reference expands to the compiler builtin, not another macro call. */
#define __atomic_compare_exchange_n(object, expected, desired, weak, success, failure) \
    (taskclaimtest_before_compare((void *)(object), (void *)(expected), (unsigned long long)(desired)), \
     __atomic_compare_exchange_n(object, expected, desired, weak, success, failure))
#include "../../../src/shared/runtime/concurrency.c"
#undef __atomic_compare_exchange_n

static void taskclaimtest_wait_until(unsigned int *word, unsigned int target) {
    for (;;) {
        unsigned int current = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        if (current >= target) {
            return;
        }
        platform_wait_word(word, current);
    }
}

static void taskclaimtest_publish(unsigned int *word, unsigned int value) {
    __atomic_store_n(word, value, __ATOMIC_RELEASE);
    platform_wake_word_all(word);
}

static void taskclaimtest_before_compare(void *object, void *expected_address, unsigned long long desired) {
    unsigned int generation;
    unsigned int expected = 0U;

    /* The dispatcher's all-ones closing CAS must not enter the worker gates. */
    if (object != &test_pool.claimed_workers || desired == 0xffffffffU) {
        return;
    }
    generation = __atomic_load_n(&test_pool.generation, __ATOMIC_ACQUIRE);
    if (generation == 1U &&
        __atomic_compare_exchange_n(&test_parked, &expected, 1U, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        __atomic_store_n(&test_expected_address, expected_address, __ATOMIC_RELEASE);
        platform_wake_word_all(&test_parked);
        /* Another native worker must finish generation 1 while this one is parked. */
        platform_wake_word_all(&test_pool.generation);
        taskclaimtest_wait_until(&test_stage, 2U);
        __atomic_store_n(&test_stale_context, 1U, __ATOMIC_RELEASE);
        return;
    }
    if (generation >= 2U) {
        if (expected_address == __atomic_load_n(&test_expected_address, __ATOMIC_ACQUIRE)) {
            /* A subsequent claim by the parked thread belongs to its fresh observation. */
            __atomic_store_n(&test_stale_context, 0U, __ATOMIC_RELEASE);
        } else {
            /* Keep the next counter at 1 until the delayed CAS has been exercised. */
            taskclaimtest_wait_until(&test_stage, 3U);
        }
    }
}

static int taskclaimtest_body(size_t begin, size_t end, unsigned int worker_index, void *arg) {
    unsigned int phase = *(unsigned int *)arg;
    (void)end;

    if (phase == 1U) {
        if (worker_index == 0U) {
            taskclaimtest_wait_until(&test_parked, 1U);
        }
    } else if (worker_index == 0U && begin == 0U) {
        taskclaimtest_publish(&test_stage, 2U);
        taskclaimtest_wait_until(&test_completed, 1U);
    } else if (worker_index != 0U) {
        if (__atomic_load_n(&test_stale_context, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&test_violation, 1U, __ATOMIC_RELEASE);
        }
        taskclaimtest_publish(&test_completed, 1U);
    }
    return 0;
}

typedef struct {
    unsigned int visits[32];
    size_t count;
} TaskClaimTestVisits;

static int taskclaimtest_visit(size_t begin, size_t end, unsigned int worker_index, void *arg) {
    TaskClaimTestVisits *visits = (TaskClaimTestVisits *)arg;
    size_t index;
    (void)worker_index;
    for (index = begin; index < end; ++index) {
        __atomic_fetch_add(&visits->visits[index], 1U, __ATOMIC_RELAXED);
    }
    return 0;
}

static int taskclaimtest_task(unsigned int worker_index, void *arg) {
    (void)worker_index;
    __atomic_fetch_add((unsigned int *)arg, 1U, __ATOMIC_RELAXED);
    return 0;
}

static int taskclaimtest_transitions(void) {
    RtTaskPool pool;
    unsigned int round;
    if (rt_task_pool_init(&pool, 4U) != 0) {
        return -1;
    }
    for (round = 0U; round < 1000U; ++round) {
        TaskClaimTestVisits visits;
        RtTaskGroup group;
        size_t index;
        int result;

        rt_memset(&visits, 0, sizeof(visits));
        visits.count = round % 3U == 0U ? 1U : (round % 3U == 1U ? 2U : 32U);
        result = rt_parallel_for(&pool, visits.count, 1U, taskclaimtest_visit, &visits);
        if (result != 0 || rt_task_group_begin(&pool, &group) != 0) {
            rt_task_pool_destroy(&pool);
            return -1;
        }
        for (index = 0U; index < visits.count; ++index) {
            if (rt_task_group_submit(&group, taskclaimtest_task, &visits.visits[index]) != 0) {
                rt_task_group_wait(&group);
                rt_task_pool_destroy(&pool);
                return -1;
            }
        }
        result = rt_task_group_wait(&group);
        for (index = 0U; index < 32U; ++index) {
            if (visits.visits[index] != (index < visits.count ? 2U : 0U)) {
                result = -1;
            }
        }
        if (result != 0) {
            rt_task_pool_destroy(&pool);
            return -1;
        }
    }
    rt_task_pool_destroy(&pool);
    return 0;
}

int main(void) {
    unsigned int phase = 1U;
    int result;

    if (rt_task_pool_init(&test_pool, 3U) != 0) {
        rt_write_cstr(2, "taskclaimtest: pool initialization failed\n");
        return 1;
    }
    if (test_pool.native_width >= 3U) {
        result = rt_parallel_for(&test_pool, 2U, 1U, taskclaimtest_body, &phase);
        phase = 2U;
        if (result == 0) {
            result = rt_parallel_for(&test_pool, 2U, 1U, taskclaimtest_body, &phase);
        }
        taskclaimtest_publish(&test_stage, 3U);
        rt_task_pool_destroy(&test_pool);
        if (result != 0 || __atomic_load_n(&test_violation, __ATOMIC_ACQUIRE)) {
            rt_write_cstr(2, "taskclaimtest: stale generation executed the next callback\n");
            return 1;
        }
        rt_write_cstr(1, "scheduled_generation_reuse: ok\n");
    } else {
        rt_task_pool_destroy(&test_pool);
        rt_write_cstr(1, "scheduled_generation_reuse: unavailable (needs three native participants)\n");
    }
    if (taskclaimtest_transitions() != 0) {
        rt_write_cstr(2, "taskclaimtest: parallel/group/serial transition mismatch\n");
        return 1;
    }
    rt_write_cstr(1, "status: ok\n");
    return 0;
}
