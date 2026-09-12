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
#include "../src/plex_fts_join_rewrite.h"
#undef malloc
static void *controlled_malloc(size_t n) { return fail_alloc ? NULL : malloc(n); }

static const char movie_sql[] =
 "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id"
 " join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id"
 " where fts4_tag_titles_icu.tag match 'Th* OR (com* AND rom*)' and unlikely(tag_type=1)"
 " and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1"
 " group by tags.id order by count(*) desc limit 100";
static unsigned state = 0x19a31u;
static unsigned random_word(void) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}
static void exercise(const char *input, size_t n) {
    size_t scan = 0, len = 0, unused = 0;
    char *out = plex_build_fts_join_sql(input, n, n, &scan, &len);
    if (!out) return;
    assert(scan == len && out[len] == 0);
    assert(plex_build_fts_join_sql(out, len, len, &unused, &unused) == NULL);
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
    assert(plex_build_fts_join_sql(sql, n, n, &scan, &len) == NULL);
    fail_alloc = 0;
    out = plex_build_fts_join_sql(sql, n, n, &scan, &len);
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
int main(void) {
    static const char album_sql[] =
      "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id"
      " join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id"
      " where fts4_tag_titles_icu.tag match 'Th* OR (com* AND rom*)' and unlikely(tag_type=1)"
      " and metadata_items.library_section_id in (19) and (metadata_items.metadata_type=9 and (metadata_items.remote is null))"
      " group by tags.id order by count(*) desc limit 100";
    run_case(movie_sql);
    run_case(album_sql);
    puts("PASS: 200000 mutations, movie+album guarded prefixes, allocation failure, idempotence");
    return 0;
}
