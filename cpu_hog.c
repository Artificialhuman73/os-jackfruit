/* CHIRANTH.R (PES1UG24AM354) | Deeptanshu Kumar (PES1UG24AM357)
 * CPU-bound workload: Sieve of Eratosthenes
 * Repeatedly runs the sieve to find all primes <= SIEVE_N,
 * keeping the CPU fully occupied. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SIEVE_N 500000

int main(int argc, char **argv) {
    long duration_sec = 5;
    if (argc > 1) duration_sec = atol(argv[1]);

    printf("[cpu_hog] CHIRANTH.R (PES1UG24AM354) | Deeptanshu Kumar (PES1UG24AM357)\n");
    printf("[cpu_hog] Sieve of Eratosthenes (N=%d) for %ld s\n",
           SIEVE_N, duration_sec);
    fflush(stdout);

    char *sieve = malloc(SIEVE_N + 1);
    if (!sieve) { perror("malloc"); return 1; }

    time_t start = time(NULL);
    unsigned long runs = 0;

    while (time(NULL) - start < duration_sec) {
        /* Mark all as prime, then cross out composites */
        memset(sieve, 1, SIEVE_N + 1);
        sieve[0] = sieve[1] = 0;

        for (int i = 2; (long long)i * i <= SIEVE_N; i++) {
            if (sieve[i]) {
                for (int j = i * i; j <= SIEVE_N; j += i)
                    sieve[j] = 0;
            }
        }

        /* Count primes */
        long prime_count = 0;
        for (int i = 2; i <= SIEVE_N; i++)
            if (sieve[i]) prime_count++;

        runs++;
        if (runs % 20 == 0) {
            printf("[cpu_hog] Run %lu: %ld primes <= %d\n",
                   runs, prime_count, SIEVE_N);
            fflush(stdout);
        }
    }

    free(sieve);
    printf("[cpu_hog] Done — %lu sieve runs completed\n", runs);
    return 0;
}
