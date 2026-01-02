/*
 * microbenchmark2.c - Region Size Scaling Benchmark
 * 
 * Tests Hydra's TLB shootdown optimization across different memory region sizes.
 * Measures how IPI reduction scales with region size.
 *
 * Compile: gcc -O2 -o microbenchmark2 microbenchmark2.c -lpthread -lnuma
 * Run:     numactl -r all ./microbenchmark2 -s <size_in_kb>
 */

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
#include <getopt.h>

#define NUM_OPS 10000

static int num_nodes;
static size_t region_size;
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
            break;
        }
    }
    numa_free_cpumask(cpus);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

static void *worker(void *arg) {
    worker_data_t *data = (worker_data_t *)arg;
    struct timespec start, end;
    
    pin_to_node(data->node);
    
    data->region = mmap(NULL, region_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data->region == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    
    /* Touch pages to fault them in on THIS node */
    memset(data->region, 0xAB, region_size);
    
    __sync_fetch_and_add(&ready_count, 1);
    while (!go) {
        __asm__ __volatile__("pause" ::: "memory");
    }
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < NUM_OPS; i++) {
        mprotect(data->region, region_size, PROT_READ);
        mprotect(data->region, region_size, PROT_READ | PROT_WRITE);
        data->ops += 2;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    data->elapsed_sec = (end.tv_sec - start.tv_sec) + 
                        (end.tv_nsec - start.tv_nsec) / 1e9;
    
    munmap(data->region, region_size);
    return NULL;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s -s <size_in_kb>\n", prog);
    fprintf(stderr, "  -s, --size    Region size in KB (default: 8192)\n");
    fprintf(stderr, "  -h, --help    Show this help\n");
    fprintf(stderr, "\nExample sizes: 4, 64, 512, 2048, 8192, 32768, 131072\n");
    fprintf(stderr, "\nRun with Hydra: numactl -r all %s -s <size>\n", prog);
}

int main(int argc, char **argv) {
    pthread_t *threads;
    worker_data_t *data;
    uint64_t total_ops = 0;
    double max_time = 0;
    size_t size_kb = 8192; /* Default 8MB */
    
    static struct option long_opts[] = {
        {"size", required_argument, 0, 's'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "s:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's':
            size_kb = (size_t)atol(optarg);
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }
    
    region_size = size_kb * 1024;
    
    if (numa_available() < 0) {
        fprintf(stderr, "NUMA not available\n");
        return 1;
    }
    
    num_nodes = numa_num_configured_nodes();
    
    printf("========================================\n");
    printf("Microbenchmark 2: Region Size Scaling\n");
    printf("========================================\n");
    printf("NUMA nodes: %d\n", num_nodes);
    printf("Threads: %d (one per node)\n", num_nodes);
    printf("Region size: %zu KB (%zu MB)\n", size_kb, size_kb / 1024);
    printf("Pages in region: %zu\n", region_size / 4096);
    printf("Page-tables covered: %zu\n", (region_size + (512 * 4096 - 1)) / (512 * 4096));
    printf("Ops per thread: %d\n", NUM_OPS * 2);
    printf("\n");
    
    threads = calloc(num_nodes, sizeof(pthread_t));
    data = calloc(num_nodes, sizeof(worker_data_t));
    
    for (int i = 0; i < num_nodes; i++) {
        data[i].node = i;
        data[i].ops = 0;
        if (pthread_create(&threads[i], NULL, worker, &data[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    
    while (ready_count < num_nodes) {
        usleep(1000);
    }
    
    printf("All threads ready. Starting benchmark...\n\n");
    
    go = 1;
    __sync_synchronize();
    
    for (int i = 0; i < num_nodes; i++) {
        pthread_join(threads[i], NULL);
        printf("Node %d: %.3f sec, %lu ops\n", 
               i, data[i].elapsed_sec, (unsigned long)data[i].ops);
        total_ops += data[i].ops;
        if (data[i].elapsed_sec > max_time)
            max_time = data[i].elapsed_sec;
    }
    
    printf("\n========================================\n");
    printf("RESULTS (region_size=%zuKB):\n", size_kb);
    printf("========================================\n");
    printf("Total mprotect ops: %lu\n", (unsigned long)total_ops);
    printf("Wall time: %.3f sec\n", max_time);
    printf("Throughput: %.0f ops/sec\n", total_ops / max_time);
    printf("Latency per op: %.2f us\n", (max_time * 1e6) / (total_ops / num_nodes));
    printf("========================================\n");
    
    free(threads);
    free(data);
    return 0;
}
