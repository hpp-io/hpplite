/*
** A stand-in for libFuzzer where the toolchain has none (Apple's clang): linked with a target and run under
** AddressSanitizer, it feeds the target each file named on the command line, then for HPPLITE_FUZZ_SECONDS (default
** 30) mutations of those files - bytes flipped, set, inserted, removed, cut - as libFuzzer's simplest mutators do.
** CI builds the targets with -fsanitize=fuzzer instead (#343).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static unsigned char *slurp(const char *path, size_t *pn) {
    FILE *f = fopen(path, "rb");
    unsigned char *p;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    p = malloc((size_t)n + 1);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    *pn = (size_t)n;
    return p;
}

int main(int argc, char **argv) {
    const char *z = getenv("HPPLITE_FUZZ_SECONDS");
    double seconds = z ? atof(z) : 30;
    unsigned seed = (unsigned)time(NULL);
    unsigned char *seeds[256];
    size_t lens[256];
    int n = 0;
    long runs = 0;
    for (int i = 1; i < argc && n < 256; i++) {
        if ((seeds[n] = slurp(argv[i], &lens[n])) != NULL) { LLVMFuzzerTestOneInput(seeds[n], lens[n]); n++; runs++; }
    }
    if (n == 0) { seeds[0] = calloc(1, 1); lens[0] = 0; n = 1; }
    srand(seed);
    printf("fuzz driver: %d seeds, seed %u, %.0f s\n", n, seed, seconds);
    clock_t end = clock() + (clock_t)(seconds * CLOCKS_PER_SEC);
    unsigned char *buf = malloc(1 << 20);
    while (clock() < end) {
        int s = rand() % n;
        size_t len = lens[s] < (1 << 20) - 64 ? lens[s] : (1 << 20) - 64;
        memcpy(buf, seeds[s], len);
        for (int k = 1 + rand() % 8; k > 0; k--) {
            size_t at = len ? (size_t)rand() % len : 0;
            switch (rand() % 5) {
                case 0: if (len) buf[at] ^= (unsigned char)(1u << (rand() % 8)); break;
                case 1: if (len) buf[at] = (unsigned char)rand(); break;
                case 2: if (len < (1 << 20) - 64) { memmove(buf + at + 1, buf + at, len - at); buf[at] = (unsigned char)rand(); len++; } break;
                case 3: if (len) { memmove(buf + at, buf + at + 1, len - at - 1); len--; } break;
                case 4: len = at; break;
            }
        }
        {
            /* in an allocation of exactly its length, as libFuzzer hands it: a read past the end traps (ASan, or guard malloc) */
            unsigned char *exact = malloc(len ? len : 1);
            memcpy(exact, buf, len);
            LLVMFuzzerTestOneInput(exact, len);
            free(exact);
        }
        runs++;
    }
    printf("fuzz driver: %ld runs, no crash\n", runs);
    return 0;
}
