// aesdsocket.c

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <time.h>
#include <pthread.h>
#include <stdbool.h>

#define PORT 9000
#define FILE_PATH "/var/tmp/aesdsocketdata"

volatile sig_atomic_t stop = 0;

pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t list_mutex = PTHREAD_MUTEX_INITIALIZER;

struct thread_info {
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    bool finished;
    struct thread_info *next;
};

/* Head of singly-linked list */
static struct thread_info *thread_list = NULL;

void signal_handler(int sig) {
    (void)sig;
    stop = 1;
}

void setup_signal_handlers(void) {
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS); // parent exits
    }
    if (setsid() < 0) {
        perror("setsid failed");
        exit(EXIT_FAILURE);
    }
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_RDWR);
}

/* Add node to head of list */
void add_threadinfo(struct thread_info *info) {
    pthread_mutex_lock(&list_mutex);
    info->next = thread_list;
    thread_list = info;
    pthread_mutex_unlock(&list_mutex);
}

/* Remove a node (by pointer) from list and free it after join.
   Returns true if removed, false otherwise. */
bool remove_threadinfo_and_join(struct thread_info *prev, struct thread_info *curr) {
    if (!curr) return false;
    /* join must be done outside list lock? we will hold lock to remove, then unlock then join */
    pthread_mutex_lock(&list_mutex);
    if (prev) prev->next = curr->next;
    else thread_list = curr->next;
    pthread_mutex_unlock(&list_mutex);

    /* join thread now */
    pthread_join(curr->thread_id, NULL);
    close(curr->client_fd);
    free(curr);
    return true;
}

/* Reap finished threads: iterate list, for nodes with finished==true join and remove them */
void reap_finished_threads(void) {
    pthread_mutex_lock(&list_mutex);
    struct thread_info *prev = NULL;
    struct thread_info *curr = thread_list;
    /* We will build a small array of nodes to join to avoid joining while holding list_mutex
       But for simplicity we mark nodes and remove them first, then join them one by one.
    */
    while (curr) {
        if (curr->finished) {
            struct thread_info *to_remove = curr;
            /* remove from list */
            if (prev) prev->next = curr->next;
            else thread_list = curr->next;
            curr = curr->next;
            /* unlock, join, then relock to continue - to avoid long lock while joining */
            pthread_mutex_unlock(&list_mutex);
            pthread_join(to_remove->thread_id, NULL);
            close(to_remove->client_fd);
            free(to_remove);
            pthread_mutex_lock(&list_mutex);
            /* prev remains the same (because we removed the node after prev) */
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
    pthread_mutex_unlock(&list_mutex);
}

/* Worker thread function */
void *connection_handler(void *arg) {
    struct thread_info *tinfo = (struct thread_info *)arg;
    int client_fd = tinfo->client_fd;
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &tinfo->client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

    const size_t RECV_BUF_SZ = 1024;
    char recv_buf[RECV_BUF_SZ];
    char *packet_buffer = NULL;
    size_t total_size = 0;

    while (!stop) {
        ssize_t bytes = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
        if (bytes < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "recv error from %s: %s", client_ip, strerror(errno));
            break;
        } else if (bytes == 0) {
            /* client closed */
            break;
        }

        /* append to packet_buffer */
        char *tmp = realloc(packet_buffer, total_size + bytes + 1);
        if (!tmp) {
            syslog(LOG_ERR, "realloc failed");
            free(packet_buffer);
            packet_buffer = NULL;
            total_size = 0;
            break;
        }
        packet_buffer = tmp;
        memcpy(packet_buffer + total_size, recv_buf, bytes);
        total_size += bytes;
        packet_buffer[total_size] = '\0';

        /* process complete packets ending with newline */
        char *newline;
        while ((newline = memchr(packet_buffer, '\n', total_size)) != NULL) {
            size_t packet_len = (newline - packet_buffer) + 1;

            /* write atomically to file using file_mutex */
            pthread_mutex_lock(&file_mutex);
            FILE *fp = fopen(FILE_PATH, "a");
            if (!fp) {
                syslog(LOG_ERR, "fopen append failed: %s", strerror(errno));
                pthread_mutex_unlock(&file_mutex);
                /* cannot write; still remove the packet from buffer to avoid infinite loop */
            } else {
                size_t wrote = fwrite(packet_buffer, 1, packet_len, fp);
                if (wrote != packet_len) {
                    syslog(LOG_ERR, "fwrite incomplete");
                }
                fclose(fp);

                /* after append, read the entire file and send to client while holding file_mutex
                   to provide consistent snapshot (prevents interleaving with other writers).
                 */
                fp = fopen(FILE_PATH, "r");
                if (fp) {
                    char sendbuf[1024];
                    size_t r;
                    /* rewind isn't necessary since fopen new */
                    while ((r = fread(sendbuf, 1, sizeof(sendbuf), fp)) > 0) {
                        ssize_t s = send(client_fd, sendbuf, r, 0);
                        if (s < 0) {
                            syslog(LOG_ERR, "send error to %s: %s", client_ip, strerror(errno));
                            break;
                        }
                    }
                    fclose(fp);
                } else {
                    syslog(LOG_ERR, "fopen read failed: %s", strerror(errno));
                }
            }
            pthread_mutex_unlock(&file_mutex);

            /* remove processed packet from packet_buffer */
            size_t remaining = total_size - packet_len;
            if (remaining > 0) memmove(packet_buffer, packet_buffer + packet_len, remaining);
            total_size = remaining;
            char *tmp2 = realloc(packet_buffer, total_size + 1);
            if (tmp2) packet_buffer = tmp2;
            if (packet_buffer) packet_buffer[total_size] = '\0';
        }
    }

    free(packet_buffer);
    /* mark finished: main thread will join it */
    tinfo->finished = true;
    syslog(LOG_INFO, "Thread for %s exiting", client_ip);
    return NULL;
}

/* Timer thread that appends RFC-2822 timestamp every 10 seconds */
void *timer_thread_func(void *arg) {
    (void)arg;
    while (!stop) {
        /* produce RFC 2822-like timestamp using strftime format "%a, %d %b %Y %H:%M:%S %z" */
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char timestr[128];
        /* Example: "Fri, 29 Aug 2025 18:03:12 +0300" */
        if (strftime(timestr, sizeof(timestr), "%a, %d %b %Y %H:%M:%S %z", &tm) == 0) {
            /* fallback */
            snprintf(timestr, sizeof(timestr), "%ld", (long)now);
        }

        char line[256];
        int n = snprintf(line, sizeof(line), "timestamp:%s\n", timestr);
        if (n > 0) {
            pthread_mutex_lock(&file_mutex);
            FILE *fp = fopen(FILE_PATH, "a");
            if (fp) {
                fwrite(line, 1, (size_t)n, fp);
                fclose(fp);
            } else {
                syslog(LOG_ERR, "timer fopen append failed: %s", strerror(errno));
            }
            pthread_mutex_unlock(&file_mutex);
        }

        /* sleep up to 10 seconds but wake early if stop set */
        for (int i = 0; i < 10 && !stop; ++i) {
            sleep(1);
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int run_as_daemon = 0;
    int opt;
    while ((opt = getopt(argc, argv, "d")) != -1) {
        if (opt == 'd') run_as_daemon = 1;
        else {
            fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    setup_signal_handlers();
    openlog("aesdsocket", LOG_PID, LOG_USER);

    if (run_as_daemon) daemonize();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int optval = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    syslog(LOG_INFO, "Server listening on port %d", PORT);

    /* start timer thread */
    pthread_t timer_thread;
    if (pthread_create(&timer_thread, NULL, timer_thread_func, NULL) != 0) {
        syslog(LOG_ERR, "Failed to create timer thread");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    while (!stop) {
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (stop) break; /* likely interrupted by signal */
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        /* create thread_info and spawn worker */
        struct thread_info *tinfo = calloc(1, sizeof(*tinfo));
        if (!tinfo) {
            syslog(LOG_ERR, "calloc failed for thread_info");
            close(client_fd);
            continue;
        }
        tinfo->client_fd = client_fd;
        tinfo->client_addr = client_addr;
        tinfo->finished = false;
        tinfo->next = NULL;

        if (pthread_create(&tinfo->thread_id, NULL, connection_handler, tinfo) != 0) {
            syslog(LOG_ERR, "pthread_create failed");
            close(client_fd);
            free(tinfo);
            continue;
        }

        add_threadinfo(tinfo);

        /* reap any finished threads so we don't accumulate joined threads */
        reap_finished_threads();
    }

    /* stop requested: close listening socket and wait for threads to finish */
    syslog(LOG_INFO, "Caught signal, exiting");
    close(server_fd);

    /* Wait for all worker threads to finish */
    pthread_mutex_lock(&list_mutex);
    struct thread_info *curr = thread_list;
    while (curr) {
        /* Tell client fds to shutdown to encourage threads to exit */
        shutdown(curr->client_fd, SHUT_RDWR);
        curr = curr->next;
    }
    pthread_mutex_unlock(&list_mutex);

    /* Now reap (join and free) all remaining threads */
    reap_finished_threads();

    /* If any still remain (they might not have set finished yet), join them explicitly */
    pthread_mutex_lock(&list_mutex);
    curr = thread_list;
    while (curr) {
        pthread_mutex_unlock(&list_mutex);
        pthread_join(curr->thread_id, NULL);
        close(curr->client_fd);
        struct thread_info *next = curr->next;
        free(curr);
        pthread_mutex_lock(&list_mutex);
        curr = next;
    }
    thread_list = NULL;
    pthread_mutex_unlock(&list_mutex);

    /* stop timer thread */
    stop = 1;
    pthread_join(timer_thread, NULL);

    /* cleanup file and locks */
    unlink(FILE_PATH);
    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&list_mutex);
    closelog();
    return 0;
}

