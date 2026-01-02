/*
 * microbenchmark3.c - Spinning Thread Interference Benchmark
 * 
 * Measures how spinning threads on remote NUMA nodes impact mprotect performance.
 * Reproduces the key experiment from Hydra paper (Figure 1).
 *
 * Compile: gcc -O2 -o microbenchmark3 microbenchmark3.c -lpthread -lnuma
 * Run:     numactl -r all ./microbenchmark3 -s <spinners_per_node>
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

#define NUM_OPS 20000
#define REGION_SIZE (64 * 1024)  /* 64KB - optimal from microbenchmark2 */
#define WORKER_NODE 0

static int num_nodes;
static int spinners_per_node;
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
    
    /* Spin until told to stop */
    while (!stop_spinners) {
        data->spin_count++;
        __asm__ __volatile__("pause" ::: "memory");
    }
    
    return NULL;
}

static void *worker(void *arg) {
    worker_data_t *data = (worker_data_t *)arg;
    struct timespec start, end;
    
    pin_to_node_first_cpu(WORKER_NODE);
    
    data->region = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data->region == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    
    /* Touch pages to fault them in on worker's node */
    memset(data->region, 0xAB, REGION_SIZE);
    
    __sync_fetch_and_add(&ready_count, 1);
    while (!go) {
        __asm__ __volatile__("pause" ::: "memory");
    }
    
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
    return NULL;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s -s <spinners_per_node>\n", prog);
    fprintf(stderr, "  -s, --spinners  Number of spinner threads per remote node (default: 0)\n");
    fprintf(stderr, "  -h, --help      Show this help\n");
    fprintf(stderr, "\nExample: %s -s 4  (4 spinners on each of nodes 1-7)\n", prog);
    fprintf(stderr, "\nRun with Hydra: numactl -r all %s -s <n>\n", prog);
}

int main(int argc, char **argv) {
    pthread_t worker_thread;
    pthread_t *spinner_threads = NULL;
    spinner_data_t *spinner_data = NULL;
    worker_data_t worker_data = {0};
    int total_spinners;
    int expected_ready;
    
    spinners_per_node = 0;
    
    static struct option long_opts[] = {
        {"spinners", required_argument, 0, 's'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "s:h", long_opts, NULL)) != -1) {
        switch (opt) {
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
    
    /* Spinners on all nodes except WORKER_NODE */
    total_spinners = spinners_per_node * (num_nodes - 1);
    expected_ready = total_spinners + 1; /* spinners + worker */
    
    printf("========================================\n");
    printf("Microbenchmark 3: Spinning Thread Interference\n");
    printf("========================================\n");
    printf("NUMA nodes: %d\n", num_nodes);
    printf("Worker node: %d\n", WORKER_NODE);
    printf("Spinners per remote node: %d\n", spinners_per_node);
    printf("Total spinner threads: %d\n", total_spinners);
    printf("Region size: %d KB\n", REGION_SIZE / 1024);
    printf("Ops (mprotect pairs): %d\n", NUM_OPS);
    printf("\n");
    
    /* Allocate spinner resources */
    if (total_spinners > 0) {
        spinner_threads = calloc(total_spinners, sizeof(pthread_t));
        spinner_data = calloc(total_spinners, sizeof(spinner_data_t));
        
        /* Create spinner threads on remote nodes */
        int idx = 0;
        for (int node = 0; node < num_nodes; node++) {
            if (node == WORKER_NODE) continue;
            
            for (int s = 0; s < spinners_per_node; s++) {
                int cpu = get_cpu_for_node(node, s);
                if (cpu < 0) {
                    fprintf(stderr, "Warning: cannot get CPU %d on node %d\n", s, node);
                    continue;
                }
                
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
        total_spinners = idx; /* Adjust for any failures */
        expected_ready = total_spinners + 1;
    }
    
    /* Create worker thread */
    if (pthread_create(&worker_thread, NULL, worker, &worker_data) != 0) {
        perror("pthread_create worker");
        return 1;
    }
    
    /* Wait for all threads ready */
    while (ready_count < expected_ready) {
        usleep(1000);
    }
    
    printf("All threads ready (%d spinners + 1 worker). Starting benchmark...\n\n", total_spinners);
    
    /* GO! */
    go = 1;
    __sync_synchronize();
    
    /* Wait for worker to complete */
    pthread_join(worker_thread, NULL);
    
    /* Stop spinners */
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
    printf("RESULTS (spinners_per_node=%d):\n", spinners_per_node);
    printf("========================================\n");
    printf("Total mprotect ops: %lu\n", (unsigned long)worker_data.ops);
    printf("Wall time: %.3f sec\n", worker_data.elapsed_sec);
    printf("Throughput: %.0f ops/sec\n", worker_data.ops / worker_data.elapsed_sec);
    printf("Latency per op: %.2f us\n", (worker_data.elapsed_sec * 1e6) / worker_data.ops);
    printf("========================================\n");
    
    if (spinner_threads) free(spinner_threads);
    if (spinner_data) free(spinner_data);
    
    return 0;
}
