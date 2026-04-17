#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* Memory-bound workload: allocate and touch memory */
int main(int argc, char** argv) {
    long duration_sec = 5;
    long mib_to_allocate = 20;  /* Allocate 20 MiB by default */

    if (argc > 1) {
        duration_sec = atol(argv[1]);
    }
    if (argc > 2) {
        mib_to_allocate = atol(argv[2]);
    }

    time_t start = time(NULL);
    long bytes_to_allocate = mib_to_allocate * 1024 * 1024;
    char** blocks;
    long block_count = 0;

    printf("[memory_hog] Starting memory workload: allocate %ld MiB for %ld seconds\n",
           mib_to_allocate, duration_sec);

    /* Allocate blocks of 1 MiB */
    blocks = malloc((bytes_to_allocate / (1024 * 1024)) * sizeof(char*));

    while (time(NULL) - start < duration_sec) {
        if (block_count * 1024 * 1024 >= bytes_to_allocate) {
            break;
        }

        /* Allocate 1 MiB */
        blocks[block_count] = malloc(1024 * 1024);
        if (!blocks[block_count]) {
            printf("[memory_hog] Failed to allocate block %ld\n", block_count);
            break;
        }

        /* Touch every page to ensure it's actually allocated */
        for (long i = 0; i < 1024 * 1024; i += 4096) {
            blocks[block_count][i] = (char)(block_count % 256);
        }

        printf("[memory_hog] Allocated block %ld (total: %ld MiB)\n", block_count + 1, block_count + 1);
        block_count++;

        /* Small sleep */
        usleep(100000);
    }

    printf("[memory_hog] Allocated %ld blocks (%ld MiB total)\n", block_count, block_count);

    /* Keep alive for remaining duration */
    while (time(NULL) - start < duration_sec) {
        sleep(1);
    }

    /* Cleanup */
    for (long i = 0; i < block_count; i++) {
        free(blocks[i]);
    }
    free(blocks);

    printf("[memory_hog] Completed\n");
    return 0;
}
