#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>



typedef struct {
    alignas(128) atomic_uintptr_t   next;
    uintptr_t                       key;
} list_node_t;

typedef struct {
    atomic_uintptr_t head;
    atomic_uintptr_t tail;
} list_t;

#define is_marked(p)            (bool) ((uintptr_t)(p) &0x01)
#define get_marked(p)           ((uintptr_t)(p) | (0x01))
#define get_marked_node(p)      ((list_node_t *) get_marked(p))
#define get_unmarked(p)         ((uintptr_t)(p) & (~0x01))
#define get_unmarked_node(p)    ((list_node_t *) get_unmarked(p))

static list_t *list_new() {
    list_t *list = malloc(sizeof(list_t));
    list_node_t *sentinel_head = malloc(sizeof(list_node_t));
    list_node_t *sentinel_tail = malloc(sizeof(list_node_t));

    atomic_init(&sentinel_head->next, (uintptr_t) sentinel_tail);
    sentinel_head->key = 0;
    sentinel_tail->key = UINTPTR_MAX;

    atomic_init(&list->head, (uintptr_t) sentinel_head);
    atomic_init(&list->tail, (uintptr_t) sentinel_tail);
    return list;
}

#define STOP_IF_WEAK() {if(weak) { *weak=false; return false; }}

static bool list_find(list_t              *list,
                      uintptr_t           *key,
                      atomic_uintptr_t    **par_prev,
                      list_node_t         **par_curr,
                      list_node_t         **par_next,
                      bool                *weak)
{
    atomic_uintptr_t *prev = NULL;
    list_node_t *curr = NULL, *next = NULL;

try_again:
    prev = &list->head;
    curr = (list_node_t *) atomic_load(prev);

    while (true) {
        next = (list_node_t *) atomic_load(&get_unmarked_node(curr)->next);

        if (!(get_unmarked_node(curr)->key < *key)) {
            if (is_marked(atomic_load(&curr->next)) ||
                is_marked(&((list_node_t *) atomic_load(prev))->next)) {
                STOP_IF_WEAK();
                // TEXTBOOK
                goto try_again;
            }

            *par_curr = curr;
            *par_prev = prev;
            *par_next = next;
            return get_unmarked_node(curr)->key == *key;
        }
        // traverse
        prev = &get_unmarked_node(curr)->next;
        curr = next;
    }
}


/*
 * Insert key assuming no-possible contention
 */
static void list_insert_unsafe(list_t *list, uintptr_t key)
{
    atomic_uintptr_t *prev = &list->head;
    list_node_t *cur = (list_node_t *) atomic_load(prev);

    while (cur->key < key) {
        prev = &cur->next;
        cur = (list_node_t *) atomic_load(prev);
    }

    if (cur->key == key)
        return;

    list_node_t *new = malloc(sizeof(list_node_t));
    new->key = key;
    atomic_init(&new->next, (uintptr_t) cur);
    atomic_store(prev, (uintptr_t) new);
}

static void list_print(list_t *list, const char *title)
{
    list_node_t *cur = (list_node_t *) atomic_load(&list->head);

    printf("==================== %-20s ====================\n", title);
    while (cur) {
        printf("LIST %lu ", cur->key);
        if (is_marked(atomic_load(&cur->next)))
            printf("[M]");
        putchar('\n');
        cur = get_unmarked_node(atomic_load(&cur->next));
    }
    printf("==============================================================\n");
}

typedef struct {
    const char          *t;
    list_t              *list;
    atomic_uintptr_t    *prev;
    list_node_t         *curr;
    list_node_t         *next;
    uintptr_t           key;
    uintptr_t           tmp;
    int                 step;
} state_t;

static state_t *_list_delete_step1(list_t *list, uintptr_t key, const char *t)
{
    state_t *state = malloc(sizeof(*state));
    state->t = t;
    state->list = list;
    state->key = key;
    state->step = 0;
    bool weak = true;
    if (list_find(list, &state->key, &state->prev, &state->curr,
                  &state->next, &weak))
        printf("%s FIND OK\n", state->t);
    else {
        if (!weak)
            printf("%s FIND -ETRYAGAIN\n", state->t);
        else
            printf("%s FIND FAIL\n", state->t);
        return state;
    }
    state->step = 1;
    return state;
}

static void _list_delete_step2(state_t *state)
{
    if (!(state->step == 1))
        return;

    state->tmp = get_unmarked(state->next);

    atomic_compare_exchange_strong(&state->curr->next, &state->tmp,
                                   get_marked(state->next));
    state->step = 2;
}

static void _list_delete_step3(state_t *state)
{
    if (!(state->step == 2))
        return;

    state->tmp = get_unmarked(state->curr);
    atomic_compare_exchange_strong(state->prev, &state->tmp,
                                   get_unmarked(state->next));

    state->step = 3;
}

int main () {

    list_t *list = list_new();

    list_insert_unsafe(list, 300);
    list_insert_unsafe(list, 100);
    list_insert_unsafe(list, 200);
    list_insert_unsafe(list, 500);
    list_insert_unsafe(list, 400);

    /*
     * CASE 1: Distinct delete (200, 400)
     *  Expected: both can be deleted concurrently
     *
     * CASE 2: Consecutive delete (200, 300)
     *  Expected: t2 should retry find until 200 is deleted
     */
    list_print(list, "init");

    state_t *t1 = _list_delete_step1(list, 200, "t1");
    _list_delete_step2(t1);
    state_t *t2 = _list_delete_step1(list, 300, "t2");

    list_print(list, "marked");
    _list_delete_step3(t1);
    _list_delete_step2(t2);
    _list_delete_step2(t2);
    _list_delete_step3(t2);

    list_print(list, "deleted");
}
