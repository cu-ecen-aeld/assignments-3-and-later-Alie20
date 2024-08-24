#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <unistd.h> // For access()

// Function to create a directory if it does not exist
int create_directory(const char *dir) {
    // Check if directory exists
    if (access(dir, F_OK) != 0) {
        // Try to create the directory
        if (mkdir(dir, 0755) != 0) {
            syslog(LOG_ERR, "Failed to create directory %s: %s", dir, strerror(errno));
            fprintf(stderr, "Failed to create directory %s: %s\n", dir, strerror(errno));
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        // Log usage error
        syslog(LOG_ERR, "Usage: %s <file> <string>", argv[0]);
        fprintf(stderr, "Usage: %s <file> <string>\n", argv[0]);
        closelog(); // Close syslog
        return EXIT_FAILURE;
    }

    const char *file = argv[1];
    const char *string = argv[2];

    // Extract the directory path from the file path
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", file);
    char *last_slash = strrchr(dir, '/');
    if (last_slash != NULL) {
        *last_slash = '\0'; // Remove file name, leaving directory path
        // Create directory if it doesn't exist
        if (create_directory(dir) != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
    }

    // Open file for appending
    FILE *fp = fopen(file, "a");
    if (fp == NULL) {
        // Log file opening error
        syslog(LOG_ERR, "Error opening file %s: %s", file, strerror(errno));
        fprintf(stderr, "Error opening file %s: %s\n", file, strerror(errno));
        closelog(); // Close syslog
        return EXIT_FAILURE;
    }

    // Write string to file
    if (fprintf(fp, "%s\n", string) < 0) {
        // Log file writing error
        syslog(LOG_ERR, "Error writing to file %s: %s", file, strerror(errno));
        fprintf(stderr, "Error writing to file %s: %s\n", file, strerror(errno));
        fclose(fp);
        closelog(); // Close syslog
        return EXIT_FAILURE;
    }

    // Log success message
    syslog(LOG_DEBUG, "Successfully wrote '%s' to %s", string, file);
    fclose(fp);
    closelog(); // Close syslog

    return EXIT_SUCCESS;
}

