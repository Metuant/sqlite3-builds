#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static int fail_alloc;
static void *controlled_malloc(size_t n);
#define malloc controlled_malloc
#include "../src/plex_recent_episode_rewrite.h"
#undef malloc
static char *collection_build(const char*s,size_t n,size_t scan,size_t*a,size_t*b){return plex_build_recent_episode_sql(s,n,scan,a,b);}
static void *controlled_malloc(size_t n) { return fail_alloc ? NULL : malloc(n); }

static const char movie_sql[] = "select metadata_items.id from metadata_items join metadata_items as children on children.parent_id=metadata_items.id join metadata_items as grandchildren on grandchildren.parent_id=children.id where metadata_items.library_section_id in (?1) and (metadata_items.title NOT LIKE ?2 and metadata_items.id not in (select taggings.metadata_item_id from taggings join tags on tags.id=taggings.tag_id where tags.id=?3) and grandchildren.added_at>?4 and (metadata_items.metadata_type=?5 or metadata_items.metadata_type=?6)) order by metadata_items.title_sort collate icu_root limit ?7 offset ?8";
static unsigned state = 0x19a31u;
static unsigned random_word(void) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}
static void exercise(const char *input, size_t n) {
    size_t scan = 0, len = 0, unused = 0;
    char *out = collection_build(input, n, n, &scan, &len);
    if (!out) return;
    assert(scan == len && out[len] == 0);
    assert(collection_build(out, len, len, &unused, &unused) == NULL);
    free(out);
}
static void run_case(const char *sql) {
    size_t n = strlen(sql), i, j, scan, len;
    char *out, *page, *end;
    long size = sysconf(_SC_PAGESIZE);
    assert(size > 0 && n < (size_t)size);
    /* End the non-NUL-terminated SQL exactly at an unreadable guard page. */
    page = mmap(NULL, (size_t)size*2, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED);
    assert(mprotect(page+size, (size_t)size, PROT_NONE) == 0);
    end = page+size;
    for (i = 0; i <= n; ++i) {
        memcpy(end-i, sql, i);
        exercise(end-i, i);
    }
    assert(munmap(page, (size_t)size*2) == 0);
    fail_alloc = 1;
    assert(collection_build(sql, n, n, &scan, &len) == NULL);
    fail_alloc = 0;
    out = collection_build(sql, n, n, &scan, &len);
    assert(out != NULL); free(out);
    for (i = 0; i < 100000; ++i) {
        char input[4096];
        assert(n < sizeof(input));
        memcpy(input, sql, n);
        for (j = 0; j < 1+(i%8); ++j)
            input[random_word()%n] = (char)(random_word()%256);
        exercise(input, n);
    }
}
int main(void){run_case(movie_sql);puts("PASS: 100000 mutations, guarded prefixes, allocation failure, idempotence");return 0;}
