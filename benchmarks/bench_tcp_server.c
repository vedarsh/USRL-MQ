/* =============================================================================
 * USRL TCP CONCURRENT SERVER (FINAL)
 * =============================================================================
 */

#define _GNU_SOURCE
#include "usrl_core.h"
#include "usrl_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>

#define PAYLOAD_SIZE 4096
#define DEFAULT_PORT 8080

volatile sig_atomic_t running = 1;

/* Signal handlers */
void sighandler(int sig)
{
    (void)sig;
    running = 0;
}

void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    // Reap all dead children without blocking
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    errno = saved_errno;
}

/*
 * Child Process Logic:
 * Handles one client connection until EOF or error.
 */
void handle_client(usrl_transport_t *client)
{
    uint8_t *payload = malloc(PAYLOAD_SIZE);
    memset(payload, 0xBB, PAYLOAD_SIZE);

    while (1)
    {
        // 1. Recv Request
        // usrl_trans_recv now handles EINTR loop internally
        ssize_t n = usrl_trans_recv(client, payload, PAYLOAD_SIZE);

        if (n == 0)
            break; // Clean EOF
        if (n != PAYLOAD_SIZE)
        {
            // Partial read (unexpected) or Error (-1)
            // if (n < 0) perror("Child recv error");
            break;
        }

        // 2. Send Response
        n = usrl_trans_send(client, payload, PAYLOAD_SIZE);
        if (n != PAYLOAD_SIZE)
            break;
    }

    free(payload);
    usrl_trans_destroy(client);
    exit(0);
}

int main(int argc, char *argv[])
{
    int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;

    // Setup signals
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    // Shutdown on INT/TERM
    sa.sa_handler = sighandler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Auto-reap children
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART; // Critical: restart accept() if interrupted
    sigaction(SIGCHLD, &sa, NULL);

    printf("[BENCH] TCP Concurrent Server listening on port %d...\n", port);

    usrl_transport_t *server = usrl_trans_create(
        USRL_TRANS_TCP, NULL, port, 0, USRL_SWMR, true);

    if (!server)
        return 1;

    while (running)
    {
        usrl_transport_t *client = NULL;
        int rc = usrl_trans_accept(server, &client);

        if (rc == 0 && client != NULL)
        {
            pid_t pid = fork();

            if (pid == 0)
            {
                // Child: Handle client and exit
                handle_client(client);
            }
            else if (pid > 0)
            {
                // Parent: Close client fd copy and continue accepting
                usrl_trans_destroy(client);
            }
            else
            {
                perror("fork failed");
            }
        }
    }

    printf("[BENCH] TCP Server shutting down.\n");
    usrl_trans_destroy(server);
    kill(0, SIGTERM); // Kill any remaining children
    return 0;
}
