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
#include <stdarg.h>
#include <sys/ioctl.h>

#include "aesd_ioctl.h"     // MUST match struct aesd_seekto + ioctl definition

#define PORT 9000
#define DEBUG_LOG_PATH "/var/log/aesdsocket_debug.log"

// Buildroot will define USE_AESD_CHAR_DEVICE=1
#if USE_AESD_CHAR_DEVICE
#define FILE_PATH "/dev/aesdchar"
#else
#define FILE_PATH "/var/tmp/aesdsocketdata"
#endif

volatile sig_atomic_t stop = 0;

static void log_debug(const char *fmt, ...)
{
    va_list args;
    FILE *f = fopen(DEBUG_LOG_PATH, "a");
    if (f) {
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        fprintf(f, "\n");
        va_end(args);
        fclose(f);
    }
}

/* robust send helper: handles partial writes and EINTR */
static ssize_t robust_send(int sock, const void *buf, size_t len)
{
    const char *p = buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t s = send(sock, p, remaining, 0);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += s;
        remaining -= s;
    }
    return (ssize_t)len;
}

void signal_handler(int sig)
{
    (void)sig;
    stop = 1;
}

void setup_signal_handlers()
{
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

void daemonize()
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        exit(EXIT_FAILURE);
    }

    if (pid > 0) {
        exit(EXIT_SUCCESS);
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

int main(int argc, char *argv[])
{
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
    log_debug("=== aesdsocket starting (USE_AESD_CHAR_DEVICE=%d) ===", USE_AESD_CHAR_DEVICE);

    if (run_as_daemon) daemonize();

    /* Create socket */
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
    log_debug("Server listening on port %d", PORT);

    while (!stop) {

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (stop) break;
            perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);
        log_debug("Accepted connection from %s", client_ip);

        /* For char device, open once per client so ioctl/read use same fd */
        int device_fd = -1;
#if USE_AESD_CHAR_DEVICE
        device_fd = open(FILE_PATH, O_RDWR);
        if (device_fd < 0) {
            log_debug("open(%s) failed: %s", FILE_PATH, strerror(errno));
            /* we still serve but any device ops will fail */
        }
#endif

        char recv_buffer[1024];
        size_t total_size = 0;
        char *packet_buffer = NULL;

        while (!stop) {

            ssize_t bytes_received = recv(client_fd, recv_buffer, sizeof(recv_buffer), 0);
            if (bytes_received < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (bytes_received == 0) break;

            char *tmp = realloc(packet_buffer, total_size + bytes_received + 1);
            if (!tmp) { log_debug("realloc failed"); free(packet_buffer); packet_buffer = NULL; total_size = 0; break; }
            packet_buffer = tmp;
            memcpy(packet_buffer + total_size, recv_buffer, bytes_received);
            total_size += bytes_received;
            packet_buffer[total_size] = '\0';

            char *newline_ptr;
            while ((newline_ptr = strchr(packet_buffer, '\n')) != NULL) {

                size_t packet_len = newline_ptr - packet_buffer + 1;

                /* Check for ioctl command */
                if (!strncmp(packet_buffer, "AESDCHAR_IOCSEEKTO:", 20)) {

#if USE_AESD_CHAR_DEVICE
                    struct aesd_seekto seekto;
                    if (sscanf(packet_buffer + 20, "%u,%u",
                               &seekto.write_cmd, &seekto.write_cmd_offset) == 2) {

                        if (device_fd >= 0) {
                            log_debug("ioctl: write_cmd=%u offset=%u", seekto.write_cmd, seekto.write_cmd_offset);
                            if (ioctl(device_fd, AESDCHAR_IOCSEEKTO, &seekto) < 0) {
                                log_debug("ioctl failed: %s", strerror(errno));
                            }
                            /* read from device (do NOT lseek) */
                            char file_buf[1024];
                            ssize_t r;
                            while ((r = read(device_fd, file_buf, sizeof(file_buf))) > 0) {
                                if (robust_send(client_fd, file_buf, r) < 0) {
                                    log_debug("send() failed: %s", strerror(errno));
                                    break;
                                }
                            }
                            /* After reading, the device file position remains as set by ioctl */
                        } else {
                            log_debug("device fd not available for ioctl");
                        }
                    } else {
                        log_debug("Malformed AESDCHAR_IOCSEEKTO packet");
                    }
#else
                    /* File-backend: compute seek offset from file contents then read */
                    struct aesd_seekto seekto;
                    if (sscanf(packet_buffer + 20, "%u,%u",
                               &seekto.write_cmd, &seekto.write_cmd_offset) == 2) {
                        int fd = open(FILE_PATH, O_RDONLY);
                        if (fd < 0) {
                            log_debug("open(%s) failed: %s", FILE_PATH, strerror(errno));
                        } else {
                            /* compute absolute offset by reading file sequentially */
                            off_t target = 0;
                            unsigned cmd_i = 0;
                            ssize_t rv;
                            char readbuf[1024];
                            size_t accum = 0;
                            off_t found = -1;
                            /* naive scan: read file and count newlines */
                            while ((rv = read(fd, readbuf, sizeof(readbuf))) > 0) {
                                for (ssize_t i = 0; i < rv; ++i) {
                                    if (cmd_i == seekto.write_cmd && (accum + i) >= 0) {
                                        found = accum + i;
                                        break;
                                    }
                                    if (readbuf[i] == '\n') cmd_i++;
                                }
                                if (found >= 0) break;
                                accum += rv;
                            }
                            if (found >= 0) {
                                off_t final_target = found + seekto.write_cmd_offset;
                                if (lseek(fd, final_target, SEEK_SET) >= 0) {
                                    while ((rv = read(fd, readbuf, sizeof(readbuf))) > 0) {
                                        if (robust_send(client_fd, readbuf, rv) < 0) break;
                                    }
                                }
                            } else {
                                log_debug("could not compute target offset for ioctl");
                            }
                            close(fd);
                        }
                    } else {
                        log_debug("Malformed AESDCHAR_IOCSEEKTO packet");
                    }
#endif

                } else {
                    /* Normal write packet: write then return full contents */
#if USE_AESD_CHAR_DEVICE
                    if (device_fd >= 0) {
                        ssize_t w = write(device_fd, packet_buffer, packet_len);
                        if (w < 0) {
                            log_debug("write to %s failed: %s", FILE_PATH, strerror(errno));
                        } else {
                            /* set device read position to beginning by using ioctl write_cmd=0,offset=0 */
                            struct aesd_seekto seekto0 = { .write_cmd = 0, .write_cmd_offset = 0 };
                            if (ioctl(device_fd, AESDCHAR_IOCSEEKTO, &seekto0) < 0) {
                                log_debug("ioctl(setpos 0) failed: %s", strerror(errno));
                            }
                            /* read and send full contents */
                            char file_buf[1024];
                            ssize_t r;
                            while ((r = read(device_fd, file_buf, sizeof(file_buf))) > 0) {
                                if (robust_send(client_fd, file_buf, r) < 0) {
                                    log_debug("send() failed after write: %s", strerror(errno));
                                    break;
                                }
                            }
                        }
                    } else {
                        log_debug("device_fd not available to write");
                    }
#else
                    /* file backend: append then read from start */
                    int wfd = open(FILE_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
                    if (wfd >= 0) {
                        ssize_t w = write(wfd, packet_buffer, packet_len);
                        if (w < 0) log_debug("append write failed: %s", strerror(errno));
                        fsync(wfd);
                        close(wfd);
                    } else {
                        log_debug("open for append failed: %s", strerror(errno));
                    }
                    int rfd = open(FILE_PATH, O_RDONLY);
                    if (rfd >= 0) {
                        if (lseek(rfd, 0, SEEK_SET) < 0) log_debug("lseek failed: %s", strerror(errno));
                        char file_buf[1024];
                        ssize_t r;
                        while ((r = read(rfd, file_buf, sizeof(file_buf))) > 0) {
                            if (robust_send(client_fd, file_buf, r) < 0) break;
                        }
                        close(rfd);
                    } else {
                        log_debug("open for read failed: %s", strerror(errno));
                    }
#endif
                } /* end normal vs ioctl */

                /* remove processed packet from packet_buffer */
                size_t remaining = total_size - packet_len;
                if (remaining > 0)
                    memmove(packet_buffer, packet_buffer + packet_len, remaining);
                total_size = remaining;
                char *tmp2 = realloc(packet_buffer, total_size + 1);
                if (tmp2 || total_size == 0) packet_buffer = tmp2;
                if (packet_buffer) packet_buffer[total_size] = '\0';
            } /* end while newline found */
        } /* end per-connection loop */

        if (packet_buffer) free(packet_buffer);
#if USE_AESD_CHAR_DEVICE
        if (device_fd >= 0) close(device_fd);
#endif
        close(client_fd);
        log_debug("Closed connection from %s", client_ip);
    } /* main accept loop */

    close(server_fd);

#if !USE_AESD_CHAR_DEVICE
    unlink(FILE_PATH);
#endif

    closelog();
    log_debug("=== aesdsocket exited cleanly ===");
    return 0;
}

