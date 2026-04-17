#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* CPU-bound workload: perform intensive calculations */
int main(int argc, char** argv) {
    long duration_sec = 5;
    if (argc > 1) {
        duration_sec = atol(argv[1]);
    }

    time_t start = time(NULL);
    unsigned long counter = 0;
    double result = 0.0;

    printf("[cpu_hog] Starting CPU-bound workload for %ld seconds\n", duration_sec);

    while (time(NULL) - start < duration_sec) {
        /* Perform expensive calculations */
        for (int i = 0; i < 1000000; i++) {
            result += (double)i * 1.5 / (i + 1);
        }
        counter++;
    }

    printf("[cpu_hog] Completed %lu iterations, result=%f\n", counter, result);
    return 0;
}
