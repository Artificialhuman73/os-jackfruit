/* CHIRANTH.R (PES1UG24AM354) | Deeptanshu Kumar (PES1UG24AM357)
 * Memory workload: allocate 1-MiB blocks and touch every page.
 * Each page is filled with an XOR pattern derived from the SRN bytes,
 * ensuring the kernel actually backs every page with physical RAM. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#define SRN      "PES1UG24AM354"
#define SRN_LEN  13

/* Fill one 4096-byte page with XOR(SRN[i % SRN_LEN], block_index). */
static void fill_page(char *page, int block_idx) {
    for (int i = 0; i < 4096; i++)
        page[i] = SRN[i % SRN_LEN] ^ (char)(block_idx & 0xFF);
}

int main(int argc, char **argv) {
    long duration_sec    = 5;
    long mib_to_allocate = 20;

    if (argc > 1) duration_sec    = atol(argv[1]);
    if (argc > 2) mib_to_allocate = atol(argv[2]);

    printf("[memory_hog] CHIRANTH.R (PES1UG24AM354) | Deeptanshu Kumar (PES1UG24AM357)\n");
    printf("[memory_hog] Allocating %ld MiB for %ld s (SRN-XOR page fill)\n",
           mib_to_allocate, duration_sec);
    fflush(stdout);

    char **blocks = malloc(mib_to_allocate * sizeof(char *));
    if (!blocks) { perror("malloc"); return 1; }

    time_t start = time(NULL);
    long block_count = 0;

    while (time(NULL) - start < duration_sec) {
        if (block_count >= mib_to_allocate) break;

        blocks[block_count] = malloc(1024 * 1024);
        if (!blocks[block_count]) {
            printf("[memory_hog] Allocation failed at block %ld\n", block_count);
            break;
        }

        /* Touch every 4096-byte page so the kernel maps physical frames */
        for (int p = 0; p < 256; p++)   /* 256 pages * 4096 = 1 MiB */
            fill_page(blocks[block_count] + (long)p * 4096, (int)block_count);

        printf("[memory_hog] Block %ld allocated (%ld MiB total)\n",
               block_count + 1, block_count + 1);
        fflush(stdout);
        block_count++;
        usleep(100000);
    }

    printf("[memory_hog] Holding %ld MiB...\n", block_count);
    fflush(stdout);
    while (time(NULL) - start < duration_sec)
        sleep(1);

    for (long i = 0; i < block_count; i++)
        free(blocks[i]);
    free(blocks);

    printf("[memory_hog] Done\n");
    return 0;
}
