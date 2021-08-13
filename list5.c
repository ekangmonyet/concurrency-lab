#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <threads.h>

static atomic_int_fast32_t deleted = ATOMIC_VAR_INIT(0);

#define TID_UNKNOWN -1
#define MAX_THREADS 128

#define is_marked(p)            (bool) ((uintptr_t)(p) &0x01)
#define get_marked(p)           ((uintptr_t)(p) | (0x01))
#define get_marked_node(p)      ((list_node_t *) get_marked(p))
#define get_unmarked(p)         ((uintptr_t)(p) & (~0x01))
#define get_unmarked_node(p)    ((list_node_t *) get_unmarked(p))

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

    if (atomic_load(prev) != get_unmarked(curr)) {
        goto try_again;
    }

    while (true) {
        next = (list_node_t *) atomic_load(&get_unmarked_node(curr)->next);

        if (atomic_load(&get_unmarked_node(curr)->next) != (uintptr_t) next) {
            goto try_again;
        }
        if (atomic_load(prev) != get_unmarked(curr)) {
            goto try_again;
        }

        if (get_unmarked_node(next) == next) {
            if (!(get_unmarked_node(curr)->key < *key)) {
                *par_curr = curr;
                *par_prev = prev;
                *par_next = next;
                return (get_unmarked_node(curr)->key == *key);
            }
            prev = &get_unmarked_node(curr)->next;

        } else {
            //TODO: what if we don't do this?
            uintptr_t tmp = get_unmarked(curr);
            if (!atomic_compare_exchange_strong(prev, &tmp,
                                                get_unmarked(next))) {
                goto try_again;
            }

        }
        curr = next;
    }
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
        uintptr_t tmp = get_unmarked(curr);
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

        uintptr_t tmp = get_unmarked(next);

        if (!atomic_compare_exchange_strong(&curr->next, &tmp,
                                            get_marked(next))) {
            continue;
        }

        tmp = get_unmarked(curr);

        atomic_compare_exchange_strong(prev, &tmp, get_unmarked(next));
        atomic_fetch_add(&deleted, 1);
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

    int deleted = 0;
    for (int j = 0; j < 1000000; j++) {
        for (size_t i = 0; i < N_ELEMENTS; i++)
            deleted += list_delete(list, (uintptr_t) &elements[tid()-1][i]);
        if (deleted == N_ELEMENTS) {
            printf("\t\t break at %d\n", j);
            break;
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

    printf("insert %d delete %ld\n", (N_THREADS >> 1) * N_ELEMENTS, deleted);

    return 0;
}
