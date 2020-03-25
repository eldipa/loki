#include "queue.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

volatile int exit_now = 0;

struct worker_t {
    pthread_t tid;
    struct lckfree_queue *q;

    // prod only
    uint32_t start_n;
    uint32_t n;

    // cons only
    uint32_t sum;
};

void* produce(void* arg) {
    struct worker_t *ctx = arg;

    uint32_t end = ctx->start_n + ctx->n;
    for (uint32_t i = ctx->start_n; i < end; ++i) {
        //printf("%i\n", i);
        if (lckfree_queue__push(ctx->q, i)) {
            printf("PUSH FAILED\n");
        }
    }

    return NULL;
}

void* consume(void* arg) {
    struct worker_t *ctx = arg;

    while (1) {
        uint32_t i = 0;
        if (lckfree_queue__pop(ctx->q, &i) == 0)
            ctx->sum += i;
        else if (exit_now)
            break;
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 4)
        return -1;

    struct lckfree_queue q;
    int queue_sz = atoi(argv[1]);
    int prod_cnt = atoi(argv[2]);
    int cons_cnt = atoi(argv[3]);

    if (queue_sz < 0 || prod_cnt <= 0 || cons_cnt < 0)
        return -2;

    if (queue_sz % prod_cnt != 0)
        return -3;

    if (lckfree_queue__init(&q, queue_sz))
        return -4;

    struct worker_t producers[prod_cnt];
    struct worker_t consumers[cons_cnt];

    for (int i = 0; i < prod_cnt; ++i) {
        producers[i].q = &q;
        producers[i].start_n = i * (queue_sz / prod_cnt) + ((i==0) ? 1 : 0);
        producers[i].n = (queue_sz / prod_cnt) - ((i==0) ? 1 : 0);

        printf("Producer n=%u starting from %u\n",
                producers[i].n , producers[i].start_n);

        pthread_create(&(producers[i].tid), NULL, produce, &producers[i]);
    }

    for (int i = 0; i < cons_cnt; ++i) {
        consumers[i].q = &q;
        consumers[i].sum = 0;

        pthread_create(&(consumers[i].tid), NULL, consume, &consumers[i]);
    }

    for (int i = 0; i < prod_cnt; ++i) {
        pthread_join(producers[i].tid, NULL);
    }

    // all the iterms were pushed by the producers,
    // when a consumer does a pop and it fails, it must exit
    usleep(0.01);
    exit_now = 1;

    uint32_t sum = 0;
    for (int i = 0; i < cons_cnt; ++i) {
        pthread_join(consumers[i].tid, NULL);
        sum += consumers[i].sum;
    }

    lckfree_queue__destroy(&q);

    uint32_t expected = (queue_sz-1) * (queue_sz / 2);
    if (expected != sum) {
        printf("FAIL: obtained %u, expected %u\n", sum, expected);
        return -5;
    }

    return 0;
}
