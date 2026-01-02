#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sched.h>
#include <numa.h>
#include <numaif.h>
#include <time.h>

#define NUM_OPS 20000
#define REGION_SIZE (8 * 1024 * 1024)  /* 8MB per thread */

static int num_nodes;
static volatile int ready_count = 0;
static volatile int go = 0;

typedef struct {
    int node;
    void *region;
    double elapsed_sec;
    uint64_t ops;
} worker_data_t;

static void pin_to_node(int node) {
    struct bitmask *cpus = numa_allocate_cpumask();
    cpu_set_t cpuset;
    
    if (numa_node_to_cpus(node, cpus) < 0) {
        numa_free_cpumask(cpus);
        return;
    }
    
    CPU_ZERO(&cpuset);
    for (int i = 0; i < numa_num_configured_cpus(); i++) {
        if (numa_bitmask_isbitset(cpus, i)) {
            CPU_SET(i, &cpuset);
            break; /* Just first CPU on node */
        }
    }
    numa_free_cpumask(cpus);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

static void *worker(void *arg) {
    worker_data_t *data = (worker_data_t *)arg;
    struct timespec start, end;
    
    /* Pin to our node */
    pin_to_node(data->node);
    
    /* Allocate and touch memory */
    data->region = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data->region == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    
    /* Touch pages to fault them in on THIS node */
    memset(data->region, 0xAB, REGION_SIZE);
    
    /* Signal ready and wait for go */
    __sync_fetch_and_add(&ready_count, 1);
    while (!go) {
        __asm__ __volatile__("pause" ::: "memory");
    }
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    /* Main loop: mprotect triggers TLB shootdowns */
    for (int i = 0; i < NUM_OPS; i++) {
        mprotect(data->region, REGION_SIZE, PROT_READ);
        mprotect(data->region, REGION_SIZE, PROT_READ | PROT_WRITE);
        data->ops += 2;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    data->elapsed_sec = (end.tv_sec - start.tv_sec) + 
                        (end.tv_nsec - start.tv_nsec) / 1e9;
    
    munmap(data->region, REGION_SIZE);
    return NULL;
}

int main(int argc, char **argv) {
    pthread_t *threads;
    worker_data_t *data;
    uint64_t total_ops = 0;
    double max_time = 0;
    
    if (numa_available() < 0) {
        fprintf(stderr, "NUMA not available\n");
        return 1;
    }
    
    num_nodes = numa_num_configured_nodes();
    
    printf("========================================\n");
    printf("Hydra TLB Shootdown Benchmark\n");
    printf("========================================\n");
    printf("NUMA nodes: %d\n", num_nodes);
    printf("Threads: %d (one per node)\n", num_nodes);
    printf("Ops per thread: %d\n", NUM_OPS * 2);
    printf("Region per thread: %d MB\n", REGION_SIZE / (1024*1024));
    printf("\n");
    
    threads = calloc(num_nodes, sizeof(pthread_t));
    data = calloc(num_nodes, sizeof(worker_data_t));
    
    /* Create workers */
    for (int i = 0; i < num_nodes; i++) {
        data[i].node = i;
        data[i].ops = 0;
        if (pthread_create(&threads[i], NULL, worker, &data[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    
    /* Wait for all threads ready */
    while (ready_count < num_nodes) {
        usleep(1000);
    }
    
    printf("All threads ready. Starting benchmark...\n\n");
    
    /* GO! */
    go = 1;
    __sync_synchronize();
    
    /* Wait for completion */
    for (int i = 0; i < num_nodes; i++) {
        pthread_join(threads[i], NULL);
        printf("Node %d: %.3f sec, %lu ops\n", 
               i, data[i].elapsed_sec, (unsigned long)data[i].ops);
        total_ops += data[i].ops;
        if (data[i].elapsed_sec > max_time)
            max_time = data[i].elapsed_sec;
    }
    
    printf("\n========================================\n");
    printf("RESULTS:\n");
    printf("========================================\n");
    printf("Total mprotect ops: %lu\n", (unsigned long)total_ops);
    printf("Wall time: %.3f sec\n", max_time);
    printf("Throughput: %.0f ops/sec\n", total_ops / max_time);
    printf("\n");
    printf("Without Hydra: each mprotect IPIs all %d nodes\n", num_nodes);
    printf("With Hydra: each mprotect IPIs only 1 node\n");
    printf("Expected IPI reduction: ~%dx\n", num_nodes);
    printf("========================================\n");
    
    free(threads);
    free(data);
    return 0;
}
