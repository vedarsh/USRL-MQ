#define _GNU_SOURCE
#include "usrl_core.h"
#include "usrl_ring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>

#define SHM_PATH "/usrl_core"
#define UPDATE_INTERVAL_MS 500

/* --------------------------------------------------------------------------
 * ANSI Colors
 * -------------------------------------------------------------------------- */
#define CLR_CLS     "\033[2J\033[H"
#define CLR_BOLD    "\033[1m"
#define CLR_RST     "\033[0m"
#define CLR_GREEN   "\033[32m"
#define CLR_YELLOW  "\033[33m"
#define CLR_RED     "\033[31m"
#define CLR_GREY    "\033[90m"

/* --------------------------------------------------------------------------
 * STATE TRACKING
 * -------------------------------------------------------------------------- */
typedef struct {
    uint64_t last_head;
    uint64_t current_head;
    double   rate_hz;
    double   bw_kbs;
    int      fill_pct;
} TopicStats;

/* --------------------------------------------------------------------------
 * UTILS
 * -------------------------------------------------------------------------- */

static uint64_t time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void* map_system(void) {
    int fd = shm_open(SHM_PATH, O_RDWR, 0666);
    if (fd < 0) return NULL;

    CoreHeader hdr;
    if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) { close(fd); return NULL; }

    void *base = mmap(NULL, hdr.mmap_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    return (base == MAP_FAILED) ? NULL : base;
}

static void draw_bar(int pct) {
    int bars = pct / 5; // 20 bars total
    printf("[");
    for (int i=0; i<20; i++) {
        if (i < bars) {
            if (pct > 90) printf(CLR_RED "|" CLR_RST);
            else if (pct > 70) printf(CLR_YELLOW "|" CLR_RST);
            else printf(CLR_GREEN "|" CLR_RST);
        } else {
            printf(" ");
        }
    }
    printf("]");
}

/* --------------------------------------------------------------------------
 * MAIN LOOP
 * -------------------------------------------------------------------------- */
int main(void) {
    void *base = map_system();
    if (!base) {
        fprintf(stderr, "Error: Could not open USRL SHM.\n");
        return 1;
    }

    CoreHeader *hdr = (CoreHeader*)base;
    TopicEntry *topics = (TopicEntry*)((uint8_t*)base + hdr->topic_table_offset);

    // Alloc Stats
    TopicStats *stats = calloc(hdr->topic_count, sizeof(TopicStats));

    // Initialize heads
    for (uint32_t i=0; i < hdr->topic_count; i++) {
        RingDesc *r = (RingDesc*)((uint8_t*)base + topics[i].ring_desc_offset);
        stats[i].last_head = atomic_load(&r->w_head);
    }

    uint64_t last_time = time_ms();

    while (1) {
        usleep(UPDATE_INTERVAL_MS * 1000);

        uint64_t now = time_ms();
        double dt = (now - last_time) / 1000.0;
        if (dt <= 0) dt = 0.001;

        // 1. Update Stats
        for (uint32_t i=0; i < hdr->topic_count; i++) {
            TopicEntry *t = &topics[i];
            RingDesc *r = (RingDesc*)((uint8_t*)base + t->ring_desc_offset);

            uint64_t head = atomic_load(&r->w_head);
            uint64_t diff = head - stats[i].last_head;

            stats[i].rate_hz = (double)diff / dt;
            stats[i].bw_kbs  = (stats[i].rate_hz * t->slot_size) / 1024.0;

            // Calc fill (approximation based on unconsumed vs capacity is hard without subscriber info)
            // Instead, we show "Activity" bar based on rate vs capacity?
            // Better: Just show visual rate indicator

            stats[i].last_head = head;
            stats[i].current_head = head;
        }
        last_time = now;

        // 2. Draw UI
        printf(CLR_CLS);
        printf(CLR_BOLD "USRL SYSTEM MONITOR" CLR_RST " | %.1fs uptime\n", (double)clock()/CLOCKS_PER_SEC);
        printf("System Memory: %lu MB | Topics: %u\n\n", hdr->mmap_size/(1024*1024), hdr->topic_count);

        printf(CLR_BOLD "%-20s %-6s %-8s %-10s %-10s %-12s\n" CLR_RST,
               "TOPIC", "TYPE", "SIZE", "RATE", "BW", "TOTAL");
        printf("--------------------------------------------------------------------------\n");

        for (uint32_t i=0; i < hdr->topic_count; i++) {
            TopicEntry *t = &topics[i];
            TopicStats *s = &stats[i];

            char rate_str[32];
            char bw_str[32];

            // Colorize based on activity
            const char *clr = (s->rate_hz > 0) ? CLR_GREEN : CLR_GREY;
            if (s->rate_hz == 0) clr = CLR_GREY;

            snprintf(rate_str, 32, "%.1f Hz", s->rate_hz);
            snprintf(bw_str, 32, "%.1f KB/s", s->bw_kbs);

            printf("%-20s %-6s %-8u %s%-10s %-10s" CLR_RST " %-12lu\n",
                   t->name,
                   (t->type == 0) ? "SWMR" : "MWMR",
                   t->slot_size,
                   clr,
                   rate_str,
                   bw_str,
                   s->current_head);
        }

        printf("\n" CLR_GREY "Press Ctrl+C to exit" CLR_RST "\n");
        fflush(stdout);
    }

    return 0;
}
