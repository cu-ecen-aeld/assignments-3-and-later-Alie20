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

#define PORT 9000
#define DEBUG_LOG_PATH "/var/log/aesdsocket_debug.log"


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

int main(int argc, char *argv[])
{
    int run_as_daemon = 0;
    int opt;

    while ((opt = getopt(argc, argv, "d")) != -1) {
        if (opt == 'd') {
            run_as_daemon = 1;
        } else {
            fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    setup_signal_handlers();
    openlog("aesdsocket", LOG_PID, LOG_USER);
    log_debug("=== aesdsocket starting (USE_AESD_CHAR_DEVICE=%d) ===", USE_AESD_CHAR_DEVICE);

    if (run_as_daemon) {
        daemonize();
    }

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

        char recv_buffer[1024];
        size_t total_size = 0;
        char *packet_buffer = NULL;
        int fd = -1; // Lazy open

        while (1) {
            ssize_t bytes_received = recv(client_fd, recv_buffer, sizeof(recv_buffer), 0);
            if (bytes_received <= 0) break;
            log_debug("Received %zd bytes: '%.*s'", bytes_received, (int)bytes_received, recv_buffer);

            char *temp = realloc(packet_buffer, total_size + bytes_received + 1);
            if (!temp) {
                syslog(LOG_ERR, "Memory allocation failed");
                log_debug("Memory allocation failed");
                free(packet_buffer);
                packet_buffer = NULL;
                total_size = 0;
                break;
            }

            packet_buffer = temp;
            memcpy(packet_buffer + total_size, recv_buffer, bytes_received);
            total_size += bytes_received;
            packet_buffer[total_size] = '\0';

            char *newline_ptr;
            while ((newline_ptr = strchr(packet_buffer, '\n')) != NULL) {
                size_t packet_len = newline_ptr - packet_buffer + 1;
                log_debug("Newline detected, writing %zu bytes to %s", packet_len, FILE_PATH);

                if (fd == -1) {
		#if USE_AESD_CHAR_DEVICE
 	   	fd = open(FILE_PATH, O_RDWR);
		#else
		    fd = open(FILE_PATH, O_RDWR | O_CREAT | O_APPEND, 0666);
		#endif
		    if (fd == -1) {
			syslog(LOG_ERR, "Failed to open %s: %s", FILE_PATH, strerror(errno));
			log_debug("Failed to open %s: %s", FILE_PATH, strerror(errno));
			break;
		    }
		}

                ssize_t written = write(fd, packet_buffer, packet_len);
                if (written < 0) {
                    syslog(LOG_ERR, "write() failed: %s", strerror(errno));
                    log_debug("write() failed: %s", strerror(errno));
                    break;
                }

                log_debug("Wrote %zd bytes, sending back data from %s", written, FILE_PATH);

                // Use low-level read for /dev/aesdchar
                lseek(fd, 0, SEEK_SET);
                char file_buf[1024];
                ssize_t bytes;
                while ((bytes = read(fd, file_buf, sizeof(file_buf))) > 0) {
                    send(client_fd, file_buf, bytes, 0);
                    log_debug("Sent %zd bytes to client", bytes);
                }

#if !USE_AESD_CHAR_DEVICE
                lseek(fd, 0, SEEK_SET);
#endif

                size_t remaining = total_size - packet_len;
                memmove(packet_buffer, packet_buffer + packet_len, remaining);
                total_size = remaining;

                char *temp2 = realloc(packet_buffer, total_size + 1);
                if (temp2) packet_buffer = temp2;
                packet_buffer[total_size] = '\0';
            }
        }

        free(packet_buffer);
        if (fd != -1)
            close(fd);
        close(client_fd);
        syslog(LOG_INFO, "Closed connection from %s", client_ip);
        log_debug("Closed connection from %s", client_ip);
    }

    syslog(LOG_INFO, "Caught signal, exiting");
    log_debug("Caught signal, exiting");

    close(server_fd);

#if !USE_AESD_CHAR_DEVICE
    unlink(FILE_PATH);
#endif

    closelog();
    log_debug("=== aesdsocket exited cleanly ===");
    return 0;
}

