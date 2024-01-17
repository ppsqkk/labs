/*
 * Cache simulator: given a valgrind memory trace, report cache hits, misses,
 * and evictions.
 *
 * Solves Cache Lab Part A from https://csapp.cs.cmu.edu/3e/labs.html.
 */

#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// #include "cachelab.h"

/*
 * A CacheLine is a node in a doubly linked list. A CacheLine contains a tag
 * which uniquely identifies it within its CacheSet.
 */
typedef struct CacheLine {
    uint64_t tag;
    struct CacheLine *prev;
    struct CacheLine *next;
} CacheLine;

/*
 * A CacheSet contains pointers to the least recently used (lru) and most
 * recently used (mru) CacheLine. The lru and mru are the tail and head,
 * respectively, of a doubly linked list which represents all CacheLines
 * currently in the CacheSet. The doubly linked list is always kept in a sorted
 * order, with the less recently used CacheLines towards the tail.
 *
 * A CacheSet keeps track of its size in order to determine when it has become
 * full.
 */
typedef struct CacheSet {
    int size;
    CacheLine *lru;
    CacheLine *mru;
} CacheSet;

int convert_hex_digit(char digit);
int initialize();
uint64_t convert_hex_string(char *string);
void cleanup();
void simulate_single_access(CacheSet *curr_set, uint64_t tag);
void simulate_operation(char *trace_line);
CacheLine *find(CacheSet *set, uint64_t tag);
CacheLine *evict(CacheSet *set, CacheLine *line);
CacheLine *push(CacheSet *set, CacheLine *line);

#define BUFFER_SIZE 100

int set_bit_count = 0;
int lines_per_set = 0;
int offset_bit_count = 0;

/*
 * A cache is an array of set_count CacheSets.
 */
CacheSet *cache;
uint64_t set_count;

int hits = 0;
int misses = 0;
int evictions = 0;

/*
 * You must specify the following as command-line arguments: number of bits used
 * for the set index, number of lines per set, number of bits used for the
 * offset, path to a valgrind memory trace.
 */
int main(int argc, char* argv[])
{
    int opt;
    char *t = NULL;

    while ((opt = getopt(argc, argv, "s:E:b:t:")) != -1) {
        switch (opt) {
        case 's':
            set_bit_count = atoi(optarg);
            break;
        case 'E':
            lines_per_set = atoi(optarg);
            break;
        case 'b':
            offset_bit_count = atoi(optarg);
            break;
        case 't':
            t = malloc(strlen(optarg));
            memcpy(t, optarg, strlen(optarg)+1);
            break;
        default:
            break;
        }
    }
    if (set_bit_count <= 0 || lines_per_set <= 0 || offset_bit_count <= 0 || t == NULL) {
        printf("Bad arguments\n");
        return 1;
    }
    if (set_bit_count >= 64 || offset_bit_count >= 64) {
        printf("Bad arguments\n");
        return 1;
    }
    // Guaranteed no overflow
    set_count = (uint64_t) 1 << set_bit_count;

    FILE *file;
    char trace_line[BUFFER_SIZE + 1];
    if ((file = fopen(t, "r")) == NULL) {
        printf("Bad file\n");
        return 1;
    }

    if (initialize() == 1) {
        printf("Bad initialize\n");
        return 1;
    }
    while (fgets(trace_line, BUFFER_SIZE, file) != NULL) {
        if (strlen(trace_line) == 0 || trace_line[strlen(trace_line)-1] != '\n') {
            printf("Bad input\n");
            cleanup();
            return 1;
        }
        simulate_operation(trace_line);
    }
    cleanup();
    fclose(file);

    // printSummary(hits, misses, evictions);
    printf("hits:%d misses:%d evictions:%d\n", hits, misses, evictions);

    return 0;
}

int initialize()
{
    uint64_t bytes_to_allocate = sizeof(CacheSet) * set_count;
    if (bytes_to_allocate / set_count != sizeof(CacheSet)) {
        return 1; // Overflow
    }
    cache = malloc(bytes_to_allocate);
    if (cache == NULL) {
        return 1;
    }
    memset(cache, 0, bytes_to_allocate);
    return 0;
}

void cleanup()
{
    for (uint64_t i = 0; i < set_count; i++) {
        CacheLine *cl = cache[i].lru;
        while (cl != NULL) {
            CacheLine *tmp = cl->next;
            free(cl);
            cl = tmp;
        }
    }
    free(cache);
}

/*
 * A line of a valgrind memory trace may begin with characters 'L', 'S', 'M', or
 * 'I'. These characters represents data load, data store, data modify, and
 * instruction load, respectively. We only consider 'L', 'S', and 'M'.
 *
 * For the purposes of this simulator, 'M' is equivalent to 'L' and then 'S',
 * and 'L' and 'S' are equivalent.
 *
 * A line of a valgrind memory trace also contains an address. This address is
 * parsed in order to determine which CacheSet it corresponds to, and which tag
 * should be searched for in that CacheSet.
 */
void simulate_operation(char *trace_line)
{
    char tmp1[2];
    char tmp2[20];
    sscanf(trace_line, "%s %[^,]s,%*s", tmp1, tmp2);

    char operation = tmp1[0];
    uint64_t address = convert_hex_string(tmp2);
    address >>= offset_bit_count; // Don't care about offset
    
    // printf("%" PRIx64 "\n", ~0 << (uint64_t) 32); // Raises warning
    uint64_t mask = (uint64_t) ~0 << set_bit_count;
    uint64_t tag = address & mask;
    uint64_t i = address & ~mask;
    CacheSet *curr_set = cache + i;

    switch (operation) {
    case 'L':
    case 'S':
        simulate_single_access(curr_set, tag);
        break;
    case 'M':
        simulate_single_access(curr_set, tag);
        simulate_single_access(curr_set, tag);
        break;
    default:
        break;
    }
}

void simulate_single_access(CacheSet *curr_set, uint64_t tag)
{
    CacheLine *match = find(curr_set, tag);
    if (match == NULL) {
        // Miss
        misses++;
        CacheLine *new_line = malloc(sizeof(CacheLine));
        new_line->tag = tag;
        if (curr_set->size >= lines_per_set) {
            // Eviction
            evictions++;
            CacheLine *evicted = evict(curr_set, curr_set->lru);
            free(evicted);
        }
        push(curr_set, new_line);
    }
    else {
        // Hit
        hits++;
        evict(curr_set, match);
        push(curr_set, match);
    }
}

/*
 * This is O(N), where N = lines per set. This can be made faster by using a
 * hashmap that maps from tag to CacheLine.
 */
CacheLine *find(CacheSet *set, uint64_t tag)
{
    CacheLine *l = set->lru;
    while (l != NULL) {
        if (l->tag == tag) {
            return l;
        }
        l = l->next;
    }
    return NULL;
}

// Does not free the line!
CacheLine *evict(CacheSet *set, CacheLine *line)
{
    if (set->lru == line) {
        set->lru = line->next;
    }
    if (set->mru == line) {
        set->mru = line->prev;
    }
    if (line->prev != NULL) {
        line->prev->next = line->next;
    }
    if (line->next != NULL) {
        line->next->prev = line->prev;
    }
    set->size--;
    return line;
}

/*
 * Makes line into the most recently used CacheLine in set.
 *
 * Note: overwrites line->prev and line->next.
 */
CacheLine *push(CacheSet *set, CacheLine *line)
{
    if (set->mru == NULL) {
        // First time
        line->prev = NULL;
        line->next = NULL;
        set->lru = line;
        set->mru = line;
    }
    else {
        set->mru->next = line;
        line->prev = set->mru;
        line->next = NULL;
        set->mru = line;
    }
    set->size++;
    return line;
}

uint64_t convert_hex_string(char *string)
{
    uint64_t res = 0;
    for (int i = 0; i < strlen(string); i++) {
        uint64_t d = convert_hex_digit(string[strlen(string)-i-1]);
        res += d * 1<<(4*i);
    }
    return res;
}

int convert_hex_digit(char digit)
{
    if (digit >= '0' && digit <= '9') {
        return digit-'0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit-'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit-'A' + 10;
    }
    return -1; // BAD
}
