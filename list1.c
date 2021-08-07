#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
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

static pthread_mutex_t mutex;

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

    pthread_mutex_lock(&mutex);
    list_node_t **prev, *curr, *next;
    if (__list_find(list, &key, &prev, &curr, &next)) {
        return false;
    }

    new->next = curr;
    *prev = new;
    pthread_mutex_unlock(&mutex);

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

#define N_ELEMENTS 1280

static uintptr_t elements[MAX_THREADS + 1][N_ELEMENTS];

static void *insert_thread(void *arg)
{
    list_t *list = arg;
    // Slight changes to test ordering
    for (int i = N_ELEMENTS - 1; i >= 0; i--)
        list_insert(list, (uintptr_t) &elements[tid()][i]);

    return NULL;
}

void print_list(list_t *list)
{
    puts("==================================");
    list_node_t *cur = list->head;
    while (cur->next != list->tail) {
        list_node_t *next = cur->next;
        printf("%lu\n", next->key);
        cur = next;
    }
    puts("==================================");
}

#define N_THREADS 128

int main() {
    pthread_t thr[N_THREADS];
    pthread_mutex_init(&mutex, NULL);

    list_t *list = list_new();

    for (size_t i = 0; i < N_THREADS; i++)
        pthread_create(&thr[i], NULL, insert_thread, list);

    for (size_t i = 0; i < N_THREADS; i++)
        pthread_join(thr[i], NULL);

    list_node_t *cur = list->head;

    for (size_t tid = 0; tid < tid_v_base; tid++) {
        for (size_t i = 0; i < N_ELEMENTS; i++) {
            list_node_t *next = cur->next;
            if (!next) {
                fprintf(stderr, "PREMATURE END OF LIST!\n");
                return -1;
            }

            if (next->key != (uintptr_t) &elements[tid][i]) {
                fprintf(stderr, "UNEXPECTED ORDERING, EXPECTED %lu GOT %lu\n",
                        (uintptr_t) &elements[tid][i], next->key);
                return -1;
            }
            cur = next;
        }
    }
    fprintf(stderr, "TEST OK!\n");
    return 0;
}
