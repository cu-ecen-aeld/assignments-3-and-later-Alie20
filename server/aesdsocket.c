#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <errno.h>

#if USE_AESD_CHAR_DEVICE
#include "../aesd-char-driver/aesd_ioctl.h"
#endif

#define PORT 9000
#define FILE_PATH "/dev/aesdchar"
#define BUFFER_SIZE 1024
#define MAX_RETRY_ATTEMPTS 10
#define RETRY_DELAY_US 500000  // 500ms

volatile sig_atomic_t stop = 0;

void handle_signal(int sig) {
    (void)sig;
    stop = 1;
}

void setup_signal_handlers() {
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

void daemonize() {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    
    if (setsid() < 0) exit(EXIT_FAILURE);
    
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    
    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    
    umask(0);
    chdir("/");
    
    for (int x = sysconf(_SC_OPEN_MAX); x >= 0; x--) {
        close(x);
    }
}

int open_device_file() {
    int fd = -1;
    int attempts = 0;
    
    while (fd < 0 && attempts < MAX_RETRY_ATTEMPTS && !stop) {
        fd = open(FILE_PATH, O_RDWR);
        if (fd < 0) {
            if (errno == ENOENT) {
                // Device file doesn't exist yet, wait and retry
                syslog(LOG_WARNING, "Device file %s not found, retrying... (%d/%d)", 
                       FILE_PATH, attempts + 1, MAX_RETRY_ATTEMPTS);
                usleep(RETRY_DELAY_US);
                attempts++;
            } else {
                // Other error, break immediately
                break;
            }
        }
    }
    
    if (fd < 0) {
        syslog(LOG_ERR, "Failed to open device file %s after %d attempts: %s", 
               FILE_PATH, attempts, strerror(errno));
    } else {
        syslog(LOG_INFO, "Successfully opened device file %s", FILE_PATH);
    }
    
    return fd;
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

    // Create socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int optval = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT)
    };

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

    while (!stop) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (stop) break;
            perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        // Open the device file with retry logic
        int fd = open_device_file();
        if (fd < 0) {
            // Send error message to client
            const char *error_msg = "ERROR: Device not available\n";
            send(client_fd, error_msg, strlen(error_msg), 0);
            close(client_fd);
            continue;
        }

        char buffer[BUFFER_SIZE];
        ssize_t bytes_received;
        char *complete_packet = NULL;
        size_t packet_size = 0;
        
        while (!stop && (bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0)) > 0) {
            buffer[bytes_received] = '\0';
            
            // Append to packet buffer
            char *new_packet = realloc(complete_packet, packet_size + bytes_received + 1);
            if (!new_packet) {
                free(complete_packet);
                break;
            }
            complete_packet = new_packet;
            memcpy(complete_packet + packet_size, buffer, bytes_received);
            packet_size += bytes_received;
            complete_packet[packet_size] = '\0';
            
            // Process complete lines (ending with newline)
            char *line_start = complete_packet;
            char *newline_ptr;
            
            while ((newline_ptr = strchr(line_start, '\n')) != NULL) {
                size_t line_length = newline_ptr - line_start + 1;
                
                // Check if this is an IOCTL command
                if (strncmp(line_start, "AESDCHAR_IOCSEEKTO:", 19) == 0) {
#if USE_AESD_CHAR_DEVICE
                    struct aesd_seekto seekto;
                    uint32_t write_cmd, write_cmd_offset;
                    
                    if (sscanf(line_start + 19, "%u,%u", &write_cmd, &write_cmd_offset) == 2) {
                        seekto.write_cmd = write_cmd;
                        seekto.write_cmd_offset = write_cmd_offset;
                        
                        syslog(LOG_DEBUG, "Processing IOCTL: cmd=%u, offset=%u", write_cmd, write_cmd_offset);
                        
                        if (ioctl(fd, AESDCHAR_IOCSEEKTO, &seekto) == 0) {
                            // Read and send all data from current position
                            char read_buf[BUFFER_SIZE];
                            ssize_t bytes_read;
                            
                            while ((bytes_read = read(fd, read_buf, sizeof(read_buf))) > 0) {
                                if (send(client_fd, read_buf, bytes_read, 0) < 0) {
                                    perror("send after ioctl");
                                    break;
                                }
                            }
                        } else {
                            perror("ioctl failed");
                        }
                    }
#endif
                } else {
                    // Regular data - write to file
                    ssize_t bytes_written = write(fd, line_start, line_length);
                    if (bytes_written > 0) {
                        // Read back entire file content and send to client
                        char read_buf[BUFFER_SIZE];
                        ssize_t bytes_read;
                        
                        // Save current position
                        off_t current_pos = lseek(fd, 0, SEEK_CUR);
                        
                        // Read from beginning
                        lseek(fd, 0, SEEK_SET);
                        while ((bytes_read = read(fd, read_buf, sizeof(read_buf))) > 0) {
                            if (send(client_fd, read_buf, bytes_read, 0) < 0) {
                                perror("send after write");
                                break;
                            }
                        }
                        
                        // Restore position for next write
                        lseek(fd, current_pos, SEEK_SET);
                    }
                }
                
                // Move to next line
                line_start = newline_ptr + 1;
            }
            
            // Keep only incomplete data for next recv
            if (line_start > complete_packet) {
                size_t remaining = packet_size - (line_start - complete_packet);
                if (remaining > 0) {
                    memmove(complete_packet, line_start, remaining);
                }
                packet_size = remaining;
                complete_packet[packet_size] = '\0';
            }
        }

        free(complete_packet);
        close(fd);
        close(client_fd);
        syslog(LOG_INFO, "Closed connection from %s", client_ip);
    }

    close(server_fd);
    syslog(LOG_INFO, "Server shutting down");
    closelog();
    return 0;
}
