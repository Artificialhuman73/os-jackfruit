/* CHIRANTH.R (PES1UG24AM354) | Deeptanshu Kumar (PES1UG24AM357)
 * I/O-bound workload: write Fibonacci sequences to temp files,
 * read them back computing a running XOR checksum, then delete. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

int main(int argc, char **argv) {
    long duration_sec = 5;
    if (argc > 1) duration_sec = atol(argv[1]);

    printf("[io_pulse] CHIRANTH.R (PES1UG24AM354) | Deeptanshu Kumar (PES1UG24AM357)\n");
    printf("[io_pulse] Fibonacci I/O workload for %ld s\n", duration_sec);
    fflush(stdout);

    time_t start = time(NULL);
    int iteration = 0;
    char filename[64];

    while (time(NULL) - start < duration_sec) {
        /* Reuse a small pool of 8 temp files to avoid filling /tmp */
        snprintf(filename, sizeof(filename),
                 "/tmp/io_pulse_%d.tmp", iteration % 8);

        /* Write: Fibonacci sequence up to term 200 */
        FILE *f = fopen(filename, "w");
        if (f) {
            unsigned long a = 0, b = 1;
            for (int i = 0; i < 200; i++) {
                fprintf(f, "fib[%03d] = %lu\n", i, a);
                unsigned long c = a + b;
                a = b;
                b = c;
            }
            fclose(f);
        }

        /* Read back: accumulate XOR checksum over every byte */
        f = fopen(filename, "r");
        if (f) {
            unsigned char checksum = 0;
            int ch;
            while ((ch = fgetc(f)) != EOF)
                checksum ^= (unsigned char)ch;
            fclose(f);
            (void)checksum;
        }

        unlink(filename);
        iteration++;
        usleep(10000);
    }

    printf("[io_pulse] Done — %d iterations\n", iteration);
    return 0;
}
