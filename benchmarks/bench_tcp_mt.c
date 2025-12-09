/* =============================================================================
 * USRL TCP MULTI-THREADED BENCHMARK
 * =============================================================================
 */
#define _GNU_SOURCE
#include "usrl_core.h"
#include "usrl_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#define PAYLOAD_SIZE 4096
#define BATCH_SIZE 1000000
#define DEFAULT_THREADS 4

struct ThreadStats
{
    long count;
    double elapsed;
};

struct ThreadArgs
{
    const char *host;
    int port;
    int id;
    struct ThreadStats *stats;
};

/* Worker Thread Function */
void *client_thread(void *arg)
{
    struct ThreadArgs *args = (struct ThreadArgs *)arg;
    uint8_t *payload = malloc(PAYLOAD_SIZE);
    memset(payload, 0xCC, PAYLOAD_SIZE);

    // Each thread needs its OWN connection
    usrl_transport_t *client = usrl_trans_create(
        USRL_TRANS_TCP, args->host, args->port, 0, USRL_SWMR, false);

    if (!client)
    {
        fprintf(stderr, "[Thread %d] Connection failed\n", args->id);
        free(payload);
        return NULL;
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    long count = 0;
    // Pure blast mode: We just want to saturate bw
    for (long i = 0; i < BATCH_SIZE; i++)
    {
        // Send Request
        if (usrl_trans_send(client, payload, PAYLOAD_SIZE) != PAYLOAD_SIZE)
            break;
        // Wait for Response (Ping-Pong)
        if (usrl_trans_recv(client, payload, PAYLOAD_SIZE) != PAYLOAD_SIZE)
            break;
        count++;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    args->stats->count = count;
    args->stats->elapsed = (end.tv_sec - start.tv_sec) +
                           (end.tv_nsec - start.tv_nsec) / 1e9;

    usrl_trans_destroy(client);
    free(payload);
    return NULL;
}

int main(int argc, char *argv[])
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 8080;
    int num_threads = argc > 3 ? atoi(argv[3]) : DEFAULT_THREADS;

    printf("[MT-BENCH] Starting %d threads on %s:%d (Payload: %d)\n",
           num_threads, host, port, PAYLOAD_SIZE);

    pthread_t threads[num_threads];
    struct ThreadArgs args[num_threads];
    struct ThreadStats stats[num_threads];

    // Launch Threads
    for (int i = 0; i < num_threads; i++)
    {
        args[i].host = host;
        args[i].port = port;
        args[i].id = i;
        args[i].stats = &stats[i];

        if (pthread_create(&threads[i], NULL, client_thread, &args[i]) != 0)
        {
            perror("pthread_create");
            return 1;
        }
    }

    // Join Threads
    for (int i = 0; i < num_threads; i++)
    {
        pthread_join(threads[i], NULL);
    }

    // Aggregation
    long total_req = 0;
    double total_bw = 0;
    double max_time = 0;

    for (int i = 0; i < num_threads; i++)
    {
        double rate = stats[i].count / stats[i].elapsed;
        double mbps = (stats[i].count * PAYLOAD_SIZE * 8.0) / (stats[i].elapsed * 1e6);

        total_req += stats[i].count;
        total_bw += mbps;
        if (stats[i].elapsed > max_time)
            max_time = stats[i].elapsed;

        // printf("[Thread %d] %.2f Mbps\n", i, mbps);
    }

    // Calculate aggregate throughput based on the slowest thread (wall time)
    // Real Aggregated Bandwidth = Total Bits / Max Time
    double real_agg_bw = (total_req * PAYLOAD_SIZE * 8.0) / (max_time * 1e6);
    double real_req_rate = total_req / max_time;

    printf("[MT-BENCH] FINAL RESULT (%d Threads):\n", num_threads);
    printf("   Total Requests: %ld\n", total_req);
    printf("   Aggregate Rate: %.2f M req/sec\n", real_req_rate / 1e6);
    printf("   Aggregate BW:   %.2f Mbps (%.2f GB/s)\n", real_agg_bw, real_agg_bw / 8000.0);

    return 0;
}
