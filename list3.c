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
    void            *next;
    uintptr_t       key;
} list_node_t;

typedef struct {
    list_node_t     *head;
    list_node_t     *tail;
} list_t;

static list_t *list_new() {
    list_t *list = malloc(sizeof(list_t));
    list_node_t *sentry_head = malloc(sizeof(list_node_t));
    list_node_t *sentry_tail = malloc(sizeof(list_node_t));
    sentry_head->next = sentry_tail;
    sentry_head->key = 0;
    sentry_tail->key = UINTPTR_MAX;

    list->head = sentry_head;
    list->tail = sentry_tail;

    return list;
}

static bool __list_find(list_t *list,
                        uintptr_t *key,
                        list_node_t ***par_prev,
                        list_node_t **par_curr,
                        list_node_t **par_next)
{
    list_node_t **prev = &list->head;
    list_node_t *curr = *prev;
    list_node_t *next;

    while (true) {
        next = curr->next;

        if (!(curr->key < *key)) {
            *par_curr = curr;
            *par_prev = prev;
            *par_next = next;

            return (curr->key == *key);
        }

        prev = (list_node_t **) &curr->next;
        curr = next;

        if (next == list->tail)
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

    list_node_t **prev, *curr, *next;
    if (__list_find(list, &key, &prev, &curr, &next)) {
        return false;
    }

    new->next = curr;
    *prev = new;

    return true;
}

static bool list_delete(list_t *list, uintptr_t key)
{
    list_node_t **prev, *curr, *next;
    if (!__list_find(list, &key, &prev, &curr, &next)) {
        return false;
    }

    *prev = next;
    free(curr);
    return true;
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

static uintptr_t elements[MAX_THREADS + 1][N_ELEMENTS];

static void *insert_thread(void *arg)
{
    list_t *list = arg;
    // Slight changes to test ordering
    for (int i = N_ELEMENTS - 1; i >= 0; i--)
        list_insert(list, (uintptr_t) &elements[tid()][i]);

    return NULL;
}

#define N_THREADS 1

int main() {
    pthread_t thr[N_THREADS];

    list_t *list = list_new();

    for (size_t i = 0; i < N_THREADS; i++)
        pthread_create(&thr[i], NULL, insert_thread, list);

    for (size_t i = 0; i < N_THREADS; i++)
        pthread_join(thr[i], NULL);

    for (size_t tid = 0; tid < tid_v_base; tid++) {
        for (size_t i = 0; i < N_ELEMENTS; i++) {
            list_delete(list, (uintptr_t) &elements[tid][i]);
        }
    }

    list_node_t *cur = list->head;
    if (cur->key != 0) {
        fprintf(stderr, "EXPECTED HEAD, GOT %lu!\n", cur->key);
        return -1;
    }
    if (!cur->next) {
        fprintf(stderr, "MISSING TAIL!\n");
        return -1;
    }
    cur = cur->next;
    if (cur->key != UINTPTR_MAX) {
        fprintf(stderr, "EXPECTED TAIL, GOT %lu!\n", cur->key);
        return -1;
    }

    fprintf(stderr, "TEST OK!\n");
    return 0;
}
