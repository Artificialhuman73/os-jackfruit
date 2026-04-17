#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

/* I/O-bound workload: perform frequent I/O operations */
int main(int argc, char** argv) {
    long duration_sec = 5;
    if (argc > 1) {
        duration_sec = atol(argv[1]);
    }

    time_t start = time(NULL);
    int iteration = 0;
    char filename[64];
    FILE* f;

    printf("[io_pulse] Starting I/O-bound workload for %ld seconds\n", duration_sec);

    while (time(NULL) - start < duration_sec) {
        /* Write to a temporary file */
        snprintf(filename, sizeof(filename), "/tmp/io_pulse_%d.tmp", iteration);
        f = fopen(filename, "w");
        if (f) {
            for (int i = 0; i < 100; i++) {
                fprintf(f, "Line %d: This is test data for I/O bound workload\n", i);
            }
            fclose(f);
        }

        /* Read it back */
        f = fopen(filename, "r");
        if (f) {
            char buf[256];
            while (fgets(buf, sizeof(buf), f)) {
                /* Just read */
            }
            fclose(f);
        }

        /* Delete file */
        unlink(filename);

        iteration++;

        /* Small sleep to simulate realistic I/O pattern */
        usleep(10000);
    }

    printf("[io_pulse] Completed %d iterations\n", iteration);
    return 0;
}
