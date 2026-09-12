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
#include "../src/plex_title_scope_rewrite.h"
#undef malloc
static void *controlled_malloc(size_t n) { return fail_alloc ? NULL : malloc(n); }

static const char movie_sql[] = "select metadata_items.id from metadata_items  join fts4_metadata_titles_icu on metadata_items.id=fts4_metadata_titles_icu.rowid  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where fts4_metadata_titles_icu.title match 'Th*'  and metadata_items.metadata_type=1 and tags.id=8353788  order by  metadata_items.title_sort collate icu_root asc, metadata_items.originally_available_at asc, metadata_items.id asc limit 101 offset 0";
static unsigned state = 0x19a31u;
static unsigned random_word(void) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}
static void exercise(const char *input, size_t n) {
    size_t scan = 0, len = 0, unused = 0;
    char *out = plex_build_title_scope_sql(input, n, n, &scan, &len);
    if (!out) return;
    assert(scan == len && out[len] == 0);
    assert(plex_build_title_scope_sql(out, len, len, &unused, &unused) == NULL);
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
    assert(plex_build_title_scope_sql(sql, n, n, &scan, &len) == NULL);
    fail_alloc = 0;
    out = plex_build_title_scope_sql(sql, n, n, &scan, &len);
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
    static const char album_sql[] = "select metadata_items.id from metadata_items   join metadata_items as parents on parents.id=metadata_items.parent_id  join metadata_items as grandparents on grandparents.id=parents.parent_id  join fts4_metadata_titles_icu on metadata_items.id=fts4_metadata_titles_icu.rowid  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where fts4_metadata_titles_icu.title match 'Th*'  and metadata_items.metadata_type=4 and tags.id=8353788  order by  grandparents.title_sort collate icu_root asc, parents.`index` IS NULL,parents.`index` asc, metadata_items.`index` IS NULL,metadata_items.`index` asc, metadata_items.originally_available_at IS NULL,metadata_items.originally_available_at asc, metadata_items.title_sort collate icu_root asc, metadata_items.id asc limit 101 offset 0";
    run_case(movie_sql);
    run_case(album_sql);
    puts("PASS: 200000 mutations, movie+episode guarded prefixes, allocation failure, idempotence");
    return 0;
}
