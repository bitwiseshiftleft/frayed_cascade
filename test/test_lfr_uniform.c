/** @file test_lfr_uniform.c
 * @author Mike Hamburg
 * @copyright 2020-2021 Rambus Inc.
 * @brief Test and bench uniform maps.
 */
#include <stdio.h>
#include "lfr_uniform.h"
#include <sys/time.h>
#include <string.h>
#include <sodium.h>
#include <math.h> // For INFINITY
#include <assert.h>
#include "util.h" // for le2ui

static double now() {
    struct timeval tv;
    if (gettimeofday(&tv, NULL)) return 0;
    return tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static void record(double *start, double *book) {
    double cur = now();
    if (cur > *start) {
        *book += cur - *start;
    }
    *start = cur;
}

void randomize(uint8_t *x, uint64_t seed, uint64_t nonce, size_t length) {
    uint8_t keybytes[crypto_stream_chacha20_KEYBYTES] = {0};
    uint8_t noncebytes[crypto_stream_chacha20_NONCEBYTES] = {0};
    memcpy(keybytes, &seed, sizeof(seed));
    memcpy(noncebytes, &nonce, sizeof(nonce));

    int ret = crypto_stream_chacha20(x, length, noncebytes, keybytes);
    if (ret) abort();
}

void usage(const char *fail, const char *me, int exitcode) {
    if (fail) fprintf(stderr, "Unknown argument: %s\n", fail);
    fprintf(stderr,"Usage: %s [--deficit 8] [--threads 0] [--augmented 8] [--blocks 2||--rows 32] [--blocks-max 0]\n", me);
    fprintf(stderr,"  [--blocks-step 10] [--exp 1.1] [--ntrials 100] [--verbose] [--seed 2] [--bail 3]\n");
    exit(exitcode);
}

static inline size_t min(size_t a, size_t b) {
    return (a<b) ? a : b;
}

int main(int argc, const char **argv) {
    long long blocks_min=2, blocks_max=-1, blocks_step=10, augmented=8, ntrials=100;
    uint64_t seed = 2;
    double ratio = 1.1;
    int is_exponential = 0, ret, verbose=0, bail=3, nthreads=0;
        
    for (int i=1; i<argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg,"--augmented") && i<argc-1) {
            augmented = atoll(argv[++i]);
        } else if (!strcmp(arg,"--blocks") && i<argc-1) {
            blocks_min = atoll(argv[++i]);
	} else if (!strcmp(arg,"--bail") && i<argc-1) {
            bail = atoll(argv[++i]);
        } else if (!strcmp(arg,"--blocks-max") && i<argc-1) {
            blocks_max = atoll(argv[++i]);
        } else if (!strcmp(arg,"--rows") && i<argc-1) {
            blocks_min = _lfr_uniform_provision_columns(atoll(argv[++i])) / LFR_BLOCKSIZE / 8;
            if (blocks_min < 2) blocks_min = 2;
        } else if (!strcmp(arg,"--rows-max") && i<argc-1) {
            blocks_max = atoll(argv[++i]) / LFR_BLOCKSIZE / 8;
        } else if (!strcmp(arg,"--blocks-step") && i<argc-1) {
            blocks_step = atoll(argv[++i]);
            is_exponential = 0;
        } else if (!strcmp(arg,"--rows-step") && i<argc-1) {
            blocks_step = atoll(argv[++i]) / LFR_BLOCKSIZE / 8;
            is_exponential = 0;
        } else if (!strcmp(arg,"--exp")) {
            is_exponential = 1;
            if (i <argc-1) ratio = atof(argv[++i]);
        } else if (!strcmp(arg,"--ntrials") && i<argc-1) {
            ntrials = atoll(argv[++i]);
        } else if (!strcmp(arg,"--threads") && i<argc-1) {
            nthreads = atoll(argv[++i]);
        } else if (!strcmp(arg,"--seed") && i<argc-1) {
            seed = atoll(argv[++i]);
        } else if (!strcmp(arg,"--verbose")) {
            verbose = 1;
        } else {
            usage(argv[i], argv[0],1);
        }
    }
    (void)nthreads;
    
    if (blocks_max <= 0) blocks_max = blocks_min;
    
    if (augmented > 64) {
        printf("We don't support augmented > 64\n");
        return 1;
    }
    unsigned rows_max = _lfr_uniform_provision_max_rows(LFR_BLOCKSIZE*8*blocks_max);
    
    size_t keylen = 8;

    const size_t ARENA_SIZE = 1<<20;
    unsigned arena_max = min(rows_max, ARENA_SIZE);
    uint8_t *keys    = malloc(arena_max*keylen);
    uint64_t *values = calloc(arena_max, sizeof(*values));
    if (keys == NULL || values == NULL) {
        printf("Can't allocate %lld key value pairs\n", (long long)rows_max);
        return 1;
    }
    
    if (blocks_min <= 1) {
        fprintf(stderr, "Must have at least 2 blocks\n");
        return 1;
    }
    
    if (blocks_min > blocks_max) {
        fprintf(stderr, "No blocks\n");
        return 1;
    }
    
    lfr_uniform_map_t map;
    lfr_uniform_builder_t matrix;
    
    int successive_fails = 0;
    for (long long blocks=blocks_min; blocks <= blocks_max && (bail <= 0 || successive_fails < bail); ) {

        size_t rows = _lfr_uniform_provision_max_rows(LFR_BLOCKSIZE*8*blocks);
        if (rows == 0) goto norows;

        size_t row_deficit = LFR_BLOCKSIZE*8*blocks - rows;
        lfr_uniform_salt_t salt;
        uint8_t salt_as_bytes[sizeof(salt)];
        randomize(salt_as_bytes, seed, blocks<<32 ^ 0xFFFFFFFF, sizeof(salt_as_bytes));
        salt = le2ui(salt_as_bytes, sizeof(salt_as_bytes));
        if (( ret=lfr_uniform_builder_init(matrix, rows, augmented, salt) )) {
            fprintf(stderr, "Init  error: %s\n", strerror(-ret));
            return ret;
        }
        assert(matrix->blocks <= (unsigned long long) blocks);
    
        double start, tot_construct=0, tot_rand=0, tot_query=0, tot_sample=0;
        size_t passes=0;
        uint64_t dist=0;
        for (unsigned t=0; t<ntrials; t++) {
            start = now();

            lfr_uniform_builder_reset(matrix);
            for (int j=0; j*ARENA_SIZE < rows; j++) {
                size_t rows_todo = min(ARENA_SIZE, rows-j*ARENA_SIZE);
                randomize(keys, seed,  blocks<<32 ^ t<<20 ^ j<<1,    rows_todo*keylen);
                randomize((uint8_t*)values,seed,blocks<<32 ^ t<<20 ^ j<<1 ^ 1,rows_todo*sizeof(*values));
                for (unsigned i=0; i<rows_todo; i++) {
                    lfr_uniform_insert(matrix,&keys[keylen*i],keylen,values[i]);
                }
            }
            record(&start, &tot_sample);

            ret=lfr_uniform_build_threaded(map, matrix, nthreads);
            record(&start, &tot_construct);
            if (ret) {
                if (verbose) printf("Solve error: %d\n", ret);
                continue;
            }
        
            uint64_t mask = (augmented==64) ? -(uint64_t)1 : ((uint64_t)1 << augmented)-1;
            int allpass = 1;
            for (int j=0; j*ARENA_SIZE < rows; j++) {
                size_t rows_todo = min(ARENA_SIZE, rows-j*ARENA_SIZE);
                randomize(keys, seed,  blocks<<32 ^ t<<20 ^ j<<1, rows_todo*keylen);
                randomize((uint8_t*)values, seed, blocks<<32 ^ t<<20 ^ j<<1 ^ 1, rows_todo*sizeof(*values));
                record(&start, &tot_rand);
                for (unsigned i=0; i<rows_todo; i++) {
                    uint64_t ret = lfr_uniform_query(map, &keys[i*keylen], keylen);
                    if (ret != (values[i] & mask)) {
                        if (verbose) printf("  Fail in row %lld: should be 0x%llx, actually 0x%llx\n",
                            (long long)(i+j*ARENA_SIZE), (long long)(values[i] & mask), (long long)ret
                        );
                        allpass = 0;
                    }
                }
                record(&start,&tot_query);
            }
            if (allpass && verbose) printf("  Pass!\n");
            passes += allpass;
            record(&start, &tot_query);

            for (size_t r=0; r<rows; r++) {
                uint64_t a_dist = matrix->row_meta[r].blocks[1] - matrix->row_meta[r].blocks[0];
                if (a_dist > 128) a_dist = 128;
                dist += a_dist;
            }
            lfr_uniform_map_destroy(map);
        }

        double us_per_query = INFINITY, sps = INFINITY, us_per_build = INFINITY, ns_per_sample = INFINITY, distrate = INFINITY;
        if (passes) {
            us_per_query = tot_query * 1e6 / passes / rows;
            ns_per_sample = tot_sample * 1e9 / passes / rows;
            us_per_build = tot_construct * 1e6 / passes / rows;
            distrate = 1.0 * dist / passes / rows;
	        successive_fails = 0;
        } else {
  	        successive_fails ++;
	    }
        if (tot_construct > 0) sps = passes / tot_construct;
        printf("Size %6d*%d*8 - %d x +%d pass rate = %4d / %4d = %5.1f%%, rand/trial=%0.5f s, time/trial=%0.5f s, sample/row=%0.5f ns, avgdist=%0.3f, build/row=%0.5f us, query/row=%0.5f us,  SPS=%0.3f\n",
            (int)blocks, (int)LFR_BLOCKSIZE, (int)row_deficit, (int)augmented, (int)passes,
            (int)ntrials, 100.0*passes/ntrials,
            tot_rand/ntrials, tot_construct/ntrials, ns_per_sample, distrate, us_per_build, us_per_query,
            sps);
        fflush(stdout);
        
        lfr_uniform_builder_destroy(matrix);
norows:
        if (is_exponential) {
            long long blocks2 = blocks * ratio;
            if (blocks2 == blocks) blocks2++;
            blocks = blocks2;
        } else {
            blocks += blocks_step;
        }
    }
    
    free(keys);
    free(values);
    
    return 0;
}
