#include "loki/queue.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

volatile int exit_now = 0;

struct worker_t {
    pthread_t tid;
    struct loki_queue *q;

    // prod only
    uint32_t start_n;
    uint32_t n;
    uint32_t push_len;
    int single_producer;

    // cons only
    uint32_t sum;
    uint32_t pop_len;
    int single_consumer;
};

void* produce(void* arg) {
    struct worker_t *ctx = arg;

    uint32_t end = ctx->start_n + ctx->n;
    uint32_t block[ctx->push_len];

    int flags = LOKI_SOME_DATA;
    if (ctx->single_producer)
        flags |= LOKI_SINGLE;

    for (uint32_t i = ctx->start_n; i < end;) {
        uint32_t len = 0;
        for (; len < ctx->push_len && len+i < end; ++len) {
            block[len] = i + len;
        }

        uint32_t ret = loki_queue__push(ctx->q, block, len, flags, NULL);
        if (ret == 0) {
            printf("PUSH FAILED\n");
        }
        else {
            i += ret;
        }
    }

    return NULL;
}

void* consume(void* arg) {
    struct worker_t *ctx = arg;

    int flags = LOKI_SOME_DATA;
    if (ctx->single_consumer)
        flags |= LOKI_SINGLE;

    while (1) {
        uint32_t block[ctx->pop_len];
        uint32_t ret = loki_queue__pop(ctx->q, block, ctx->pop_len, flags, NULL);
        if (ret > 0) {
            for (uint32_t i = 0; i < ret; ++i) {
                ctx->sum += block[i];
            }
        }
        else if (exit_now) {
            break;
        }
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    _dbg_warn("Mutex enabled!");
    if (argc != 6)
        return -1;

    struct loki_queue q;
    int queue_sz = atoi(argv[1]);
    int prod_cnt = atoi(argv[2]);
    int cons_cnt = atoi(argv[3]);

    int push_len = atoi(argv[4]);
    int pop_len  = atoi(argv[5]);

    if (queue_sz < 0 || prod_cnt <= 0 || cons_cnt < 0)
        return -2;

    if (queue_sz % prod_cnt != 0)
        return -3;

    if (loki_queue__init(&q, queue_sz))
        return -4;

    struct worker_t producers[prod_cnt];
    struct worker_t consumers[cons_cnt];

    for (int i = 0; i < prod_cnt; ++i) {
        producers[i].q = &q;
        producers[i].start_n = i * (queue_sz / prod_cnt) + ((i==0) ? 1 : 0);
        producers[i].n = (queue_sz / prod_cnt) - ((i==0) ? 1 : 0);
        producers[i].push_len = push_len;
        producers[i].single_producer = (prod_cnt == 1);

        printf("Producer n=%u starting from %u, block of len %u\n",
                producers[i].n , producers[i].start_n, producers[i].push_len);

        if (producers[i].single_producer)
            printf("Single producer\n");

        pthread_create(&(producers[i].tid), NULL, produce, &producers[i]);
    }

    for (int i = 0; i < cons_cnt; ++i) {
        consumers[i].q = &q;
        consumers[i].sum = 0;
        consumers[i].pop_len = pop_len;
        consumers[i].single_consumer = (cons_cnt == 1);

        printf("Consumer, block of len %u\n", consumers[i].pop_len);

        if (consumers[i].single_consumer)
            printf("Single consumer\n");

        pthread_create(&(consumers[i].tid), NULL, consume, &consumers[i]);
    }

    printf("Waiting for the producers\n");
    for (int i = 0; i < prod_cnt; ++i) {
        pthread_join(producers[i].tid, NULL);
        printf("Producer %i done\n", i);
    }

    // all the iterms were pushed by the producers,
    // when a consumer does a pop and it fails, it must exit
    usleep(0.01);
    printf("Signal the consumers to exit\n");
    exit_now = 1;

    printf("Waiting for the consumers\n");
    uint32_t sum = 0;
    for (int i = 0; i < cons_cnt; ++i) {
        pthread_join(consumers[i].tid, NULL);
        sum += consumers[i].sum;
        printf("Consumer %i done\n", i);
    }

    loki_queue__destroy(&q);

    uint32_t expected = (queue_sz-1) * (queue_sz / 2);
    if (expected != sum) {
        printf("FAIL: obtained %u, expected %u\n", sum, expected);
        return -5;
    }

    printf("OK\n");
    return 0;
}
