#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <threads.h>

#define TID_UNKNOWN -1
#define MAX_THREADS 128

typedef struct {
    atomic_uintptr_t    next;
    uintptr_t           key;
} list_node_t;

typedef struct {
    atomic_uintptr_t    head;
    atomic_uintptr_t    tail;
} list_t;

static list_t *list_new() {
    list_t *list = malloc(sizeof(list_t));
    list_node_t *sentry_head = malloc(sizeof(list_node_t));
    list_node_t *sentry_tail = malloc(sizeof(list_node_t));

    atomic_init(&sentry_head->next, (uintptr_t) sentry_tail);
    sentry_head->key = 0;
    sentry_tail->key = UINTPTR_MAX;

    atomic_init(&list->head, (uintptr_t) sentry_head);
    atomic_init(&list->tail, (uintptr_t) sentry_tail);

    return list;
}

static bool __list_find(list_t *list,
                        uintptr_t *key,
                        atomic_uintptr_t **par_prev,
                        list_node_t **par_curr,
                        list_node_t **par_next)
{
    atomic_uintptr_t *prev = NULL;
    list_node_t *curr = NULL, *next = NULL;

try_again:
    prev = &list->head;
    curr = (list_node_t *) atomic_load(prev);

    if (curr != (list_node_t *) atomic_load(prev))
        goto try_again;

    while (true) {
        if (!curr)
            return false;

        next = (list_node_t *) atomic_load(&curr->next);

        if (curr != (list_node_t*) atomic_load(prev))
            goto try_again;

        if (!(curr->key < *key)) {
            *par_curr = curr;
            *par_prev = prev;
            *par_next = next;

            return (curr->key == *key);
        }

        prev = &curr->next;

        curr = next;

        if (next == (list_node_t *) atomic_load(&list->tail))
            break;
    }

    *par_curr = curr;
    *par_prev = prev;
    *par_next = next;

    return false;
}

static bool list_insert(list_t *list, uintptr_t key)
{
    list_node_t *new = malloc(sizeof(list_node_t));
    new->key = key;

    atomic_uintptr_t *prev;
    list_node_t *curr, *next;

    while (true) {
        if (__list_find(list, &key, &prev, &curr, &next)) {
            return false;
        }

        atomic_store_explicit(&new->next, (uintptr_t) curr,
                              memory_order_relaxed);
        uintptr_t tmp = (uintptr_t) curr;
        if (atomic_compare_exchange_strong(prev, &tmp, (uintptr_t) new)) {
            return true;
        }
    }
}

static bool list_delete(list_t *list, uintptr_t key)
{
    atomic_uintptr_t *prev;
    list_node_t *curr, *next;

    while (true) {
        if (!__list_find(list, &key, &prev, &curr, &next)) {
            return false;
        }
        uintptr_t tmp = (uintptr_t) curr;

        atomic_compare_exchange_strong(prev, &tmp, (uintptr_t) next);
        return true;
    }
}

static thread_local int tid_v = TID_UNKNOWN;
static atomic_int_fast32_t tid_v_base = ATOMIC_VAR_INIT(0);

static inline int tid(void)
{
    if (tid_v == TID_UNKNOWN) {
        tid_v = atomic_fetch_add(&tid_v_base, 1);
    }
    return tid_v;
}


#define N_ELEMENTS 128
#define N_THREADS 4

static uintptr_t elements[MAX_THREADS + 1][N_ELEMENTS];

static void *insert_thread(void *arg)
{
    list_t *list = arg;
    // Slight changes to test ordering
    for (int i = N_ELEMENTS - 1; i >= 0; i--)
        list_insert(list, (uintptr_t) &elements[tid()][i]);

    return NULL;
}

static void *delete_thread(void *arg)
{
    list_t *list = arg;
    for (int j = 0; j < 100; j++) {
        for (int i = N_ELEMENTS - 1; i >= 0; i--) {
            list_delete(list, (uintptr_t) &elements[tid()-1][i]);
        }
    }

    return NULL;
}

int main() {
    pthread_t thr[N_THREADS];

    list_t *list = list_new();

    for (size_t i = 0; i < N_THREADS; i++)
        pthread_create(&thr[i], NULL, (i & 1) ? delete_thread : insert_thread,
                       list);

    for (size_t i = 0; i < N_THREADS; i++)
        pthread_join(thr[i], NULL);


    list_node_t *cur = (list_node_t *) atomic_load(&list->head);
    if (cur->key != 0) {
        fprintf(stderr, "EXPECTED HEAD, GOT %lu!\n", cur->key);
        return -1;
    }
    if (!cur->next) {
        fprintf(stderr, "MISSING TAIL!\n");
        return -1;
    }
    cur = (list_node_t *) atomic_load(&cur->next);
    if (cur->key != UINTPTR_MAX) {
        fprintf(stderr, "EXPECTED TAIL, GOT %lu!\n", cur->key);
        return -1;
    }

    fprintf(stderr, "\nTEST OK!\n");
    return 0;
}
