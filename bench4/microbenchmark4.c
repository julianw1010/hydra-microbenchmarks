/*
 * microbenchmark4.c - Memory Operation Comparison Benchmark
 * 
 * Compares Hydra's effectiveness across mprotect, munmap, and mmap operations.
 * Based on Hydra paper Figure 9.
 *
 * Compile: gcc -O2 -o microbenchmark4 microbenchmark4.c -lpthread -lnuma
 * Run:     numactl -r all ./microbenchmark4 -o <operation> -s <spinners_per_node>
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
#define REGION_SIZE (64 * 1024)  /* 64KB */
#define WORKER_NODE 0

typedef enum {
    OP_MPROTECT,    /* mprotect toggle (baseline) */
    OP_MUNMAP,      /* munmap + mmap cycle */
    OP_MMAP_FULL    /* mmap + touch + munmap cycle */
} op_type_t;

static int num_nodes;
static int spinners_per_node;
static op_type_t operation;
static volatile int ready_count = 0;
static volatile int go = 0;
static volatile int stop_spinners = 0;

typedef struct {
    int id;
    int node;
    int cpu;
    uint64_t spin_count;
} spinner_data_t;

typedef struct {
    void *region;
    double elapsed_sec;
    uint64_t ops;
} worker_data_t;

static const char *op_name(op_type_t op) {
    switch (op) {
    case OP_MPROTECT: return "mprotect";
    case OP_MUNMAP:   return "munmap";
    case OP_MMAP_FULL: return "mmap_full";
    default: return "unknown";
    }
}

static int get_cpu_for_node(int node, int index) {
    struct bitmask *cpus = numa_allocate_cpumask();
    int cpu = -1;
    int count = 0;
    
    if (numa_node_to_cpus(node, cpus) < 0) {
        numa_free_cpumask(cpus);
        return -1;
    }
    
    for (int i = 0; i < numa_num_configured_cpus(); i++) {
        if (numa_bitmask_isbitset(cpus, i)) {
            if (count == index) {
                cpu = i;
                break;
            }
            count++;
        }
    }
    numa_free_cpumask(cpus);
    return cpu;
}

static void pin_to_cpu(int cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

static void pin_to_node_first_cpu(int node) {
    int cpu = get_cpu_for_node(node, 0);
    if (cpu >= 0) {
        pin_to_cpu(cpu);
    }
}

static void *spinner(void *arg) {
    spinner_data_t *data = (spinner_data_t *)arg;
    
    pin_to_cpu(data->cpu);
    
    __sync_fetch_and_add(&ready_count, 1);
    while (!go) {
        __asm__ __volatile__("pause" ::: "memory");
    }
    
    while (!stop_spinners) {
        data->spin_count++;
        __asm__ __volatile__("pause" ::: "memory");
    }
    
    return NULL;
}

static void do_mprotect_workload(worker_data_t *data) {
    /* Pre-allocate region */
    data->region = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data->region == MAP_FAILED) {
        perror("mmap");
        return;
    }
    memset(data->region, 0xAB, REGION_SIZE);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < NUM_OPS; i++) {
        mprotect(data->region, REGION_SIZE, PROT_READ);
        mprotect(data->region, REGION_SIZE, PROT_READ | PROT_WRITE);
        data->ops += 2;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    data->elapsed_sec = (end.tv_sec - start.tv_sec) + 
                        (end.tv_nsec - start.tv_nsec) / 1e9;
    
    munmap(data->region, REGION_SIZE);
}

static void do_munmap_workload(worker_data_t *data) {
    /* Pre-allocate region */
    data->region = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data->region == MAP_FAILED) {
        perror("mmap");
        return;
    }
    memset(data->region, 0xAB, REGION_SIZE);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < NUM_OPS; i++) {
        /* Unmap then immediately remap at same address hint */
        void *addr = data->region;
        munmap(data->region, REGION_SIZE);
        data->region = mmap(addr, REGION_SIZE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (data->region == MAP_FAILED) {
            perror("mmap in loop");
            break;
        }
        data->ops += 1;  /* Count munmap as the operation */
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    data->elapsed_sec = (end.tv_sec - start.tv_sec) + 
                        (end.tv_nsec - start.tv_nsec) / 1e9;
    
    if (data->region != MAP_FAILED)
        munmap(data->region, REGION_SIZE);
}

static void do_mmap_full_workload(worker_data_t *data) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < NUM_OPS; i++) {
        /* Full cycle: mmap, touch, munmap */
        data->region = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (data->region == MAP_FAILED) {
            perror("mmap in loop");
            break;
        }
        /* Touch first and last page to fault them in */
        ((volatile char *)data->region)[0] = 0xAB;
        ((volatile char *)data->region)[REGION_SIZE - 1] = 0xCD;
        
        munmap(data->region, REGION_SIZE);
        data->ops += 1;  /* Count full cycle as one operation */
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    data->elapsed_sec = (end.tv_sec - start.tv_sec) + 
                        (end.tv_nsec - start.tv_nsec) / 1e9;
}

static void *worker(void *arg) {
    worker_data_t *data = (worker_data_t *)arg;
    
    pin_to_node_first_cpu(WORKER_NODE);
    
    __sync_fetch_and_add(&ready_count, 1);
    while (!go) {
        __asm__ __volatile__("pause" ::: "memory");
    }
    
    switch (operation) {
    case OP_MPROTECT:
        do_mprotect_workload(data);
        break;
    case OP_MUNMAP:
        do_munmap_workload(data);
        break;
    case OP_MMAP_FULL:
        do_mmap_full_workload(data);
        break;
    }
    
    return NULL;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s -o <operation> [-s <spinners_per_node>]\n", prog);
    fprintf(stderr, "  -o, --operation  Operation type: mprotect, munmap, mmap_full\n");
    fprintf(stderr, "  -s, --spinners   Spinners per remote node (default: 8)\n");
    fprintf(stderr, "  -h, --help       Show this help\n");
    fprintf(stderr, "\nOperations:\n");
    fprintf(stderr, "  mprotect  - Toggle protection flags (baseline)\n");
    fprintf(stderr, "  munmap    - Unmap + remap cycle\n");
    fprintf(stderr, "  mmap_full - Full mmap + touch + munmap cycle\n");
}

int main(int argc, char **argv) {
    pthread_t worker_thread;
    pthread_t *spinner_threads = NULL;
    spinner_data_t *spinner_data = NULL;
    worker_data_t worker_data = {0};
    int total_spinners;
    int expected_ready;
    
    spinners_per_node = 8;  /* Default */
    operation = OP_MPROTECT;
    
    static struct option long_opts[] = {
        {"operation", required_argument, 0, 'o'},
        {"spinners", required_argument, 0, 's'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "o:s:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'o':
            if (strcmp(optarg, "mprotect") == 0)
                operation = OP_MPROTECT;
            else if (strcmp(optarg, "munmap") == 0)
                operation = OP_MUNMAP;
            else if (strcmp(optarg, "mmap_full") == 0)
                operation = OP_MMAP_FULL;
            else {
                fprintf(stderr, "Unknown operation: %s\n", optarg);
                print_usage(argv[0]);
                return 1;
            }
            break;
        case 's':
            spinners_per_node = atoi(optarg);
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }
    
    if (numa_available() < 0) {
        fprintf(stderr, "NUMA not available\n");
        return 1;
    }
    
    num_nodes = numa_num_configured_nodes();
    total_spinners = spinners_per_node * (num_nodes - 1);
    expected_ready = total_spinners + 1;
    
    printf("========================================\n");
    printf("Microbenchmark 4: Memory Operation Comparison\n");
    printf("========================================\n");
    printf("NUMA nodes: %d\n", num_nodes);
    printf("Worker node: %d\n", WORKER_NODE);
    printf("Operation: %s\n", op_name(operation));
    printf("Spinners per remote node: %d\n", spinners_per_node);
    printf("Total spinner threads: %d\n", total_spinners);
    printf("Region size: %d KB\n", REGION_SIZE / 1024);
    printf("Iterations: %d\n", NUM_OPS);
    printf("\n");
    
    /* Allocate spinner resources */
    if (total_spinners > 0) {
        spinner_threads = calloc(total_spinners, sizeof(pthread_t));
        spinner_data = calloc(total_spinners, sizeof(spinner_data_t));
        
        int idx = 0;
        for (int node = 0; node < num_nodes; node++) {
            if (node == WORKER_NODE) continue;
            
            for (int s = 0; s < spinners_per_node; s++) {
                int cpu = get_cpu_for_node(node, s);
                if (cpu < 0) continue;
                
                spinner_data[idx].id = idx;
                spinner_data[idx].node = node;
                spinner_data[idx].cpu = cpu;
                spinner_data[idx].spin_count = 0;
                
                if (pthread_create(&spinner_threads[idx], NULL, spinner, &spinner_data[idx]) != 0) {
                    perror("pthread_create spinner");
                    return 1;
                }
                idx++;
            }
        }
        total_spinners = idx;
        expected_ready = total_spinners + 1;
    }
    
    if (pthread_create(&worker_thread, NULL, worker, &worker_data) != 0) {
        perror("pthread_create worker");
        return 1;
    }
    
    while (ready_count < expected_ready) {
        usleep(1000);
    }
    
    printf("All threads ready (%d spinners + 1 worker). Starting benchmark...\n\n", total_spinners);
    
    go = 1;
    __sync_synchronize();
    
    pthread_join(worker_thread, NULL);
    
    stop_spinners = 1;
    __sync_synchronize();
    
    if (total_spinners > 0) {
        for (int i = 0; i < total_spinners; i++) {
            pthread_join(spinner_threads[i], NULL);
        }
    }
    
    printf("Worker completed: %.3f sec, %lu ops\n", 
           worker_data.elapsed_sec, (unsigned long)worker_data.ops);
    
    printf("\n========================================\n");
    printf("RESULTS (%s, %d spinners/node):\n", op_name(operation), spinners_per_node);
    printf("========================================\n");
    printf("Total ops: %lu\n", (unsigned long)worker_data.ops);
    printf("Wall time: %.3f sec\n", worker_data.elapsed_sec);
    printf("Throughput: %.0f ops/sec\n", worker_data.ops / worker_data.elapsed_sec);
    printf("Latency per op: %.2f us\n", (worker_data.elapsed_sec * 1e6) / worker_data.ops);
    printf("========================================\n");
    
    if (spinner_threads) free(spinner_threads);
    if (spinner_data) free(spinner_data);
    
    return 0;
}
