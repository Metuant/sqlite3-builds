#include "sqlite3.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define RW_FLAGS (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)

static const char *MATCH_SQL_INT =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id  join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6  and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1   group by tags.id order by count(*) desc limit 100";
static const char *MATCH_SQL_INT_REWRITTEN =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id  join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and unlikely(tag_type=6)  and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1   group by tags.id order by count(*) desc limit 100";
static const char *MATCH_SQL_LEAN =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6";
static const char *MATCH_SQL_LEAN_REWRITTEN =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and unlikely(tag_type=6)";
static const char *MATCH_SQL_QUOTED =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type='6' and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 group by tags.id order by count(*) desc limit 100";
static const char *MATCH_SQL_PARAM =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=? and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 group by tags.id order by count(*) desc limit 100";
static const char *MATCH_SQL_NAMED_MATCH_PARAM =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match @SearchTerm and tag_type=6";
static const char *MATCH_SQL_NUMBERED_MATCH_PARAM =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match ?1 and tag_type=6";
static const char *PROJECTED_TAG_TYPE_SQL =
    "select tag_type, tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6 order by tag_type, tags.id";
static const char *PROJECTED_TAG_TYPE_SQL_REWRITTEN =
    "select tag_type, tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and unlikely(tag_type=6) order by tag_type, tags.id";
static const char *BOUNDARY_PLUS_INT_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6+1 order by tags.id";
static const char *BOUNDARY_PLUS_PARAM_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=?+1 order by tags.id";
static const char *BOUNDARY_HEX_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=0x1f order by tags.id";
static const char *NONMATCH_SQL = "select 1";
static const char *NO_FTS_SQL =
    "select tags.id from tags where tag_type=6";
static const char *NO_TARGET_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tags.id=10";
static const char *DUPLICATE_TARGET_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6 and tag_type=1";
static const char *CROSS_SCOPE_CTE_SQL =
    "with matched as (select rowid from fts4_tag_titles_icu where fts4_tag_titles_icu.tag match 'Django*') select tags.id from tags where tag_type=6 and tags.id in (select rowid from matched)";
static const char *CROSS_SCOPE_SUBQUERY_SQL =
    "select tags.id from tags where tag_type=6 and exists (select 1 from fts4_tag_titles_icu where fts4_tag_titles_icu.rowid=tags.id and fts4_tag_titles_icu.tag match 'Django*')";
static const char *PROJECTION_ONLY_TAG_TYPE_EQ_SQL =
    "select (tag_type=6) from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*'";
static const char *ORDER_ONLY_TAG_TYPE_EQ_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' order by tag_type=6";
static const char *LEFT_BOUND_TAG_TYPE_EQ_SQL =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and 1 + tag_type=4";

#define TAG_MEMBERSHIP_10 " AND metadata_items.id IN (SELECT metadata_item_id FROM taggings WHERE tag_id=10)"

static const char *TAG_BROWSE_SQL =
    "select metadata_items.id from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10)  order by taggings.`index` IS NULL,taggings.`index` asc, metadata_items.title_sort asc, metadata_items.originally_available_at asc, metadata_items.id asc limit 2 offset 0";
static const char *TAG_BROWSE_SQL_REWRITTEN =
    "select metadata_items.id from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10" TAG_MEMBERSHIP_10 ")  order by taggings.`index` IS NULL,taggings.`index` asc, metadata_items.title_sort asc, metadata_items.originally_available_at asc, metadata_items.id asc limit 2 offset 0";
static const char *TAG_COUNT_SQL =
    "select count(*) from (select distinct(metadata_items.id) from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10) )";
static const char *TAG_COUNT_SQL_REWRITTEN =
    "select count(*) from (select distinct(metadata_items.id) from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10" TAG_MEMBERSHIP_10 ") )";
static const char *TAG_LIMIT_SQL =
    "select metadata_items.id from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10)  order by taggings.`index` IS NULL,taggings.`index` asc, metadata_items.title_sort asc, metadata_items.originally_available_at asc, metadata_items.id asc limit 27 offset 0";
static const char *TAG_LIMIT_SQL_REWRITTEN =
    "select metadata_items.id from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10" TAG_MEMBERSHIP_10 ")  order by taggings.`index` IS NULL,taggings.`index` asc, metadata_items.title_sort asc, metadata_items.originally_available_at asc, metadata_items.id asc limit 27 offset 0";
static const char *TAG_MISSING_JOIN_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.tag_id=10 left join tags on taggings.tag_id=tags.id where tags.id=10";
static const char *TAG_NESTED_JOIN_SQL =
    "select metadata_items.id from metadata_items left join tags on tags.id=10 where tags.id=10 and exists (select 1 from taggings where taggings.metadata_item_id=metadata_items.id and taggings.tag_id=tags.id)";
static const char *TAG_JOIN_IN_STRING_SQL =
    "select metadata_items.id from metadata_items left join tags on tags.id=10 where tags.id=10 and 'taggings.metadata_item_id=metadata_items.id taggings.tag_id=tags.id' != ''";
static const char *TAG_JOIN_IN_COMMENT_SQL =
    "select metadata_items.id from metadata_items left join tags on tags.id=10 /* taggings.metadata_item_id=metadata_items.id and taggings.tag_id=tags.id */ where tags.id=10";
static const char *TAG_DUPLICATE_ID_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where tags.id=10 and tags.id=11";
static const char *TAG_BOUND_ID_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where tags.id=?";
static const char *TAG_NAMED_ID_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where tags.id=:tag_id";
static const char *TAG_STRING_ID_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where tags.id='10'";
static const char *TAG_METADATA_FTS_SQL =
    "select metadata_items.id from metadata_items join fts4_metadata_titles_icu on metadata_items.id=fts4_metadata_titles_icu.rowid left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where fts4_metadata_titles_icu.title match 'Django*' and tags.id=10";
static const char *TAG_FLIPPED_JOIN_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on tags.id=taggings.tag_id where metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 and tags.id=10 order by metadata_items.id";
static const char *TAG_FLIPPED_JOIN_SQL_REWRITTEN =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on tags.id=taggings.tag_id where metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 and tags.id=10" TAG_MEMBERSHIP_10 " order by metadata_items.id";
static const char *TAG_OR_ID_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 or tags.id=10)";
static const char *TAG_NOT_ID_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where metadata_items.library_section_id in (1) and not (tags.id=10)";
static const char *TAG_VALUE_COMPARED_ID_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where metadata_items.library_section_id in (1) and (tags.id=10)=1";
static const char *TAG_COLUMN_RHS_ONLY_SQL =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on tags.id=taggings.tag_id where metadata_items.library_section_id in (1) and metadata_items.metadata_type=1";

#define ONDECK_HEAD \
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid and metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id="
#define ONDECK_AFTER_SECTION " and grandparents.id in ("
#define ONDECK_AFTER_IDS " and metadata_item_settings.view_count>0  and metadata_item_views.account_id="
#define ONDECK_AFTER_ACCOUNT " group by grandparents.id order by viewed_at desc"
#define ONDECK_IDS "101,101,102"
#define ONDECK_SQL_BODY ONDECK_HEAD "2" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT

static const char *ONDECK_SQL = ONDECK_SQL_BODY;
static const char *ONDECK_SQL_REWRITTEN =
    "SELECT grandparents_id AS id,\n"
    "       originally_available_at AS originally_available_at,\n"
    "       parent_index AS parent_index,\n"
    "       metadata_item_views_index AS \"index\",\n"
    "       viewed_at AS \"max(viewed_at)\",\n"
    "       library_section_id AS library_section_id,\n"
    "       grandparents_extra_data AS extra_data\n"
    "FROM (\n"
    "  SELECT grandparents.id AS grandparents_id,\n"
    "         metadata_item_views.originally_available_at AS originally_available_at,\n"
    "         metadata_item_views.parent_index AS parent_index,\n"
    "         metadata_item_views.`index` AS metadata_item_views_index,\n"
    "         metadata_item_views.viewed_at AS viewed_at,\n"
    "         grandparents.library_section_id AS library_section_id,\n"
    "         grandparentsSettings.extra_data AS grandparents_extra_data,\n"
    "         row_number() OVER (PARTITION BY grandparents.id ORDER BY metadata_item_views.viewed_at DESC, metadata_item_views.id DESC, grandparentsSettings.id DESC, metadata_item_settings.id DESC) AS dshadow_on_deck_rank\n"
    "  FROM metadata_items AS grandparents\n"
    "  CROSS JOIN metadata_item_views\n"
    "  JOIN metadata_item_settings\n"
    "  JOIN metadata_item_settings AS grandparentsSettings\n"
    "  WHERE grandparents.guid=metadata_item_views.grandparent_guid\n"
    "    AND metadata_item_settings.guid=metadata_item_views.guid\n"
    "    AND metadata_item_views.account_id=metadata_item_settings.account_id\n"
    "    AND grandparentsSettings.guid=metadata_item_views.grandparent_guid\n"
    "    AND metadata_item_views.account_id=grandparentsSettings.account_id\n"
    "    AND metadata_item_views.library_section_id=2\n"
    "    AND grandparents.id IN (" ONDECK_IDS ")\n"
    "    AND metadata_item_settings.view_count>0\n"
    "    AND metadata_item_views.account_id=42\n"
    ") AS dshadow_on_deck_ranked\n"
    "WHERE dshadow_on_deck_rank=1\n"
    "ORDER BY viewed_at DESC, grandparents_id DESC;";
static const char *ONDECK_LEFT_JOIN_SQL =
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid left join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid and metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=2 and grandparents.id in (" ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_PARAM_LIST_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_SECTION "101,?" ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_PARAM_ACCOUNT_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "?" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_DUP_ACCOUNT_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "42 and metadata_item_views.account_id=43" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_MISSING_VIEWCOUNT_SQL =
    ONDECK_HEAD "2" ONDECK_AFTER_SECTION ONDECK_IDS ")" "  and metadata_item_views.account_id=42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_MISSING_SETTINGS_GUID_SQL =
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=2 and grandparents.id in (" ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_MISSING_SETTINGS_ACCOUNT_SQL =
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=2 and grandparents.id in (" ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char *ONDECK_PROJECTION_DRIFT_SQL =
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(metadata_item_views.viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid and metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=2 and grandparents.id in (" ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;

typedef struct auth_probe {
    int unlikely_calls;
    int deny_unlikely;
} auth_probe;

typedef struct digest_result {
    int rows;
    uint64_t hash;
} digest_result;

static const char *g_program_path;

static void failf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void require_int(const char *label, int got, int want) {
    if (got != want) failf("FAIL [%s]: got=%d want=%d", label, got, want);
}

static void require_str_eq(const char *label, const char *got, const char *want) {
    if (!got || strcmp(got, want) != 0) {
        failf("FAIL [%s]: got=\"%s\" want=\"%s\"", label, got ? got : "(null)", want);
    }
}

static void require_contains(const char *label, const char *got, const char *needle) {
    if (!got || !strstr(got, needle)) {
        failf("FAIL [%s]: got=\"%s\" missing=\"%s\"", label, got ? got : "(null)", needle);
    }
}

static void require_absent(const char *label, const char *got, const char *needle) {
    if (got && strstr(got, needle)) {
        failf("FAIL [%s]: got=\"%s\" unexpected=\"%s\"", label, got, needle);
    }
}

static int count_occurrences(const char *haystack, const char *needle) {
    int count = 0;
    size_t needle_len = strlen(needle);
    const char *p = haystack;

    if (needle_len == 0) return 0;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    return count;
}

static void require_occurrences(const char *label, const char *got, const char *needle, int want) {
    int got_count = got ? count_occurrences(got, needle) : 0;
    if (got_count != want) {
        failf("FAIL [%s]: occurrences=%d want=%d needle=\"%s\" text=\"%s\"",
              label, got_count, want, needle, got ? got : "(null)");
    }
}

static char *read_text_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    long size;
    char *buf;

    if (!fp) failf("FATAL: open %s failed: %s", path, strerror(errno));
    if (fseek(fp, 0, SEEK_END) != 0) failf("FATAL: seek end %s failed", path);
    size = ftell(fp);
    if (size < 0) failf("FATAL: tell %s failed", path);
    if (fseek(fp, 0, SEEK_SET) != 0) failf("FATAL: seek start %s failed", path);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) failf("FATAL: malloc file buffer failed");
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        failf("FATAL: read %s failed", path);
    }
    if (fclose(fp) != 0) failf("FATAL: close %s failed", path);
    buf[size] = 0;
    return buf;
}

static int safe_setenv(const char *name, const char *value) {
    if (setenv(name, value, 1) != 0) {
        fprintf(stderr, "setenv(%s=%s) failed: %s\n", name, value, strerror(errno));
        return 0;
    }
    return 1;
}

static int safe_unsetenv(const char *name) {
    if (unsetenv(name) != 0) {
        fprintf(stderr, "unsetenv(%s) failed: %s\n", name, strerror(errno));
        return 0;
    }
    return 1;
}

static void configure_env_all(const char *fts_value, const char *tag_value, const char *ondeck_value) {
    if (!safe_setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1")) exit(1);
    if (!safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1")) exit(1);
    if (!safe_setenv("SQLITE3_DISABLE_OBSERVABILITY", "1")) exit(1);
    if (fts_value) {
        if (!safe_setenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE", fts_value)) exit(1);
    } else {
        if (!safe_unsetenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE")) exit(1);
    }
    if (tag_value) {
        if (!safe_setenv("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", tag_value)) exit(1);
    } else {
        if (!safe_unsetenv("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE")) exit(1);
    }
    if (ondeck_value) {
        if (!safe_setenv("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", ondeck_value)) exit(1);
    } else {
        if (!safe_unsetenv("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE")) exit(1);
    }
}

static void configure_env(const char *rewrite_value) {
    configure_env_all(rewrite_value, NULL, NULL);
}

static int configure_obs_enabled_env(void) {
    return safe_setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1") &&
           safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1") &&
           safe_setenv("SQLITE3_DISABLE_STMT_TRACE", "1") &&
           safe_unsetenv("SQLITE3_DISABLE_OBSERVABILITY") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "0") &&
           safe_unsetenv("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE") &&
           safe_unsetenv("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE");
}

static int configure_obs_ondeck_enabled_env(void) {
    return safe_setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1") &&
           safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1") &&
           safe_setenv("SQLITE3_DISABLE_STMT_TRACE", "1") &&
           safe_unsetenv("SQLITE3_DISABLE_OBSERVABILITY") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "1") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", "0");
}

static int configure_obs_tag_enabled_env(void) {
    return safe_setenv("SQLITE3_DISABLE_AUTOPRAGMA", "1") &&
           safe_setenv("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1") &&
           safe_setenv("SQLITE3_DISABLE_STMT_TRACE", "1") &&
           safe_unsetenv("SQLITE3_DISABLE_OBSERVABILITY") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "0") &&
           safe_setenv("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", "1");
}

static void temp_path(char *buf, size_t n, const char *basename) {
    int rc = snprintf(buf, n, "/tmp/plex-fts-rewrite-smoke-%ld/%s", (long)getpid(), basename);
    if (rc < 0 || (size_t)rc >= n) failf("FATAL: temp path too long for %s", basename);
}

static void make_temp_dir(void) {
    char dir[256];
    int rc = snprintf(dir, sizeof(dir), "/tmp/plex-fts-rewrite-smoke-%ld", (long)getpid());
    if (rc < 0 || (size_t)rc >= sizeof(dir)) failf("FATAL: temp dir too long");
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        failf("FATAL: mkdir(%s) failed: %s", dir, strerror(errno));
    }
}

static void cleanup_temp_dir(void) {
    char dir[256];
    char path[512];
    const char *names[] = {
        "com.plexapp.plugins.library.db", "library.db", "jellyfin.db", "not-target.db"
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        temp_path(path, sizeof(path), names[i]);
        unlink(path);
        snprintf(path, sizeof(path), "/tmp/plex-fts-rewrite-smoke-%ld/%s-wal", (long)getpid(), names[i]);
        unlink(path);
        snprintf(path, sizeof(path), "/tmp/plex-fts-rewrite-smoke-%ld/%s-shm", (long)getpid(), names[i]);
        unlink(path);
    }
    snprintf(dir, sizeof(dir), "/tmp/plex-fts-rewrite-smoke-%ld", (long)getpid());
    rmdir(dir);
}

static void exec_sql(sqlite3 *db, const char *label, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        failf("FAIL [%s]: sqlite3_exec rc=%d err=%s", label, rc, err ? err : sqlite3_errmsg(db));
    }
    sqlite3_free(err);
}

static sqlite3 *open_db(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, RW_FLAGS, NULL);
    if (rc != SQLITE_OK) {
        failf("FAIL [open]: path=%s rc=%d err=%s", path, rc, db ? sqlite3_errmsg(db) : "(null)");
    }
    return db;
}

static void setup_schema(sqlite3 *db) {
    exec_sql(db, "schema",
        "CREATE TABLE metadata_items("
        "id INTEGER PRIMARY KEY,"
        "library_section_id INTEGER NOT NULL,"
        "metadata_type INTEGER NOT NULL,"
        "guid TEXT,"
        "parent_id INTEGER,"
        "title_sort TEXT,"
        "originally_available_at INTEGER"
        ");"
        "CREATE INDEX index_metadata_items_on_guid ON metadata_items(guid);"
        "CREATE TABLE tags(id INTEGER PRIMARY KEY, tag_type INTEGER NOT NULL);"
        "CREATE TABLE taggings("
        "id INTEGER PRIMARY KEY,"
        "metadata_item_id INTEGER NOT NULL,"
        "tag_id INTEGER NOT NULL,"
        "`index` INTEGER"
        ");"
        "CREATE VIRTUAL TABLE fts4_tag_titles_icu USING fts4(tag);"
        "CREATE VIRTUAL TABLE fts4_metadata_titles_icu USING fts4(title);"
        "CREATE TABLE metadata_item_views("
        "id INTEGER PRIMARY KEY,"
        "guid TEXT NOT NULL,"
        "grandparent_guid TEXT NOT NULL,"
        "account_id INTEGER NOT NULL,"
        "library_section_id INTEGER NOT NULL,"
        "originally_available_at INTEGER,"
        "parent_index INTEGER,"
        "`index` INTEGER,"
        "viewed_at INTEGER"
        ");"
        "CREATE INDEX index_metadata_item_views_on_guid ON metadata_item_views(guid);"
        "CREATE TABLE metadata_item_settings("
        "id INTEGER PRIMARY KEY,"
        "account_id INTEGER NOT NULL,"
        "guid TEXT NOT NULL,"
        "view_count INTEGER NOT NULL,"
        "extra_data TEXT"
        ");"
        "CREATE INDEX index_metadata_item_settings_on_account_id ON metadata_item_settings(account_id);"
        "CREATE INDEX index_metadata_item_settings_on_guid ON metadata_item_settings(guid);"
        "INSERT INTO metadata_items(id,library_section_id,metadata_type,guid,parent_id,title_sort,originally_available_at) VALUES"
        "(1,1,1,'plex://item/1',NULL,'Item 1',100),"
        "(2,1,1,'plex://item/2',NULL,'Item 2',200),"
        "(3,1,1,'plex://item/3',NULL,'Item 3',300),"
        "(4,2,1,'plex://item/4',NULL,'Item 4',400),"
        "(5,1,1,'plex://item/5',NULL,'Item 5',500),"
        "(6,1,1,'plex://item/6',NULL,'Item 6',600),"
        "(101,2,2,'plex://grandparent/101',NULL,'Grandparent 101',1000),"
        "(102,2,2,'plex://grandparent/102',NULL,'Grandparent 102',1001);"
        "INSERT INTO tags VALUES(10,6),(11,6),(12,1),(13,6),(14,7),(15,31);"
        "INSERT INTO taggings(id,metadata_item_id,tag_id,`index`) VALUES"
        "(1,1,10,1),(2,2,11,2),(3,3,12,3),(4,4,13,4),(5,5,14,5),(6,6,15,6);"
        "INSERT INTO fts4_tag_titles_icu(rowid, tag) VALUES"
        "(10,'Django Alpha'),(11,'Django Beta'),(12,'Django Other'),(13,'Django Section Two'),"
        "(14,'Django Seven'),(15,'Django Hex');"
        "INSERT INTO fts4_metadata_titles_icu(rowid, title) VALUES(1,'Django Metadata');"
        "INSERT INTO metadata_item_views"
        "(id,guid,grandparent_guid,account_id,library_section_id,originally_available_at,parent_index,`index`,viewed_at) VALUES"
        "(1001,'plex://episode/1','plex://grandparent/101',42,2,101,1,1,1000),"
        "(1002,'plex://episode/2','plex://grandparent/101',42,2,102,2,2,2000),"
        "(1003,'plex://episode/3','plex://grandparent/101',42,2,103,3,3,2000),"
        "(1004,'plex://episode/4','plex://grandparent/102',42,2,104,4,4,1500);"
        "INSERT INTO metadata_item_settings(id,account_id,guid,view_count,extra_data) VALUES"
        "(201,42,'plex://episode/1',1,'episode-1'),"
        "(202,42,'plex://episode/2',1,'episode-2'),"
        "(203,42,'plex://episode/3',1,'episode-3'),"
        "(204,42,'plex://episode/4',1,'episode-4'),"
        "(301,42,'plex://grandparent/101',1,'gp101-a'),"
        "(302,42,'plex://grandparent/101',1,'gp101-b'),"
        "(303,42,'plex://grandparent/102',1,'gp102');"
    );
}

static void create_tag_membership_index(sqlite3 *db) {
    exec_sql(db, "tag-membership-index",
             "CREATE INDEX idx_dshadow_taggings_tag_id_metadata_item_id "
             "ON taggings(tag_id, metadata_item_id);");
}

static void create_ondeck_index(sqlite3 *db) {
    exec_sql(db, "ondeck-index",
             "CREATE INDEX idx_dshadow_metadata_item_views_account_grandparent_guid "
             "ON metadata_item_views(account_id, grandparent_guid);");
}

static sqlite3_stmt *prepare_entry(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int nbyte,
    int entry,
    const char **tail
);

static void expect_ondeck_rows(sqlite3 *db) {
    sqlite3_stmt *stmt = prepare_entry(db, "ondeck-rows", ONDECK_SQL, -1, 2, NULL);
    int rc;

    require_str_eq("ondeck-rows/sql", sqlite3_sql(stmt), ONDECK_SQL_REWRITTEN);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) failf("FAIL [ondeck-rows/first]: rc=%d want=SQLITE_ROW", rc);
    require_int("ondeck-rows/first-id", sqlite3_column_int(stmt, 0), 101);
    require_int("ondeck-rows/first-index", sqlite3_column_int(stmt, 3), 3);
    require_int("ondeck-rows/first-viewed", sqlite3_column_int(stmt, 4), 2000);
    require_str_eq("ondeck-rows/first-extra",
                   (const char *)sqlite3_column_text(stmt, 6), "gp101-b");
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) failf("FAIL [ondeck-rows/second]: rc=%d want=SQLITE_ROW", rc);
    require_int("ondeck-rows/second-id", sqlite3_column_int(stmt, 0), 102);
    require_int("ondeck-rows/second-viewed", sqlite3_column_int(stmt, 4), 1500);
    require_str_eq("ondeck-rows/second-extra",
                   (const char *)sqlite3_column_text(stmt, 6), "gp102");
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) failf("FAIL [ondeck-rows/done]: rc=%d want=SQLITE_DONE", rc);
    require_int("ondeck-rows/finalize", sqlite3_finalize(stmt), SQLITE_OK);
}

static int authorizer_cb(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
) {
    auth_probe *probe = (auth_probe *)ctx;
    (void)db;
    (void)trigger;
    if (action == SQLITE_FUNCTION &&
        ((p1 && strcmp(p1, "unlikely") == 0) || (p2 && strcmp(p2, "unlikely") == 0))) {
        probe->unlikely_calls++;
        if (probe->deny_unlikely) return SQLITE_DENY;
    }
    return SQLITE_OK;
}

static sqlite3_stmt *prepare_entry(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int nbyte,
    int entry,
    const char **tail
) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (entry == 3) {
        rc = sqlite3_prepare_v3(db, sql, nbyte, SQLITE_PREPARE_PERSISTENT, &stmt, tail);
    } else if (entry == 2) {
        rc = sqlite3_prepare_v2(db, sql, nbyte, &stmt, tail);
    } else {
        rc = sqlite3_prepare(db, sql, nbyte, &stmt, tail);
    }
    if (rc != SQLITE_OK) {
        failf("FAIL [%s]: prepare entry=%d rc=%d err=%s", label, entry, rc, sqlite3_errmsg(db));
    }
    return stmt;
}

static void expect_saved_sql(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int nbyte,
    int entry,
    const char *want_sql
) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, nbyte, entry, &tail);
    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_saved_sql_contains(
    sqlite3 *db,
    const char *label,
    const char *sql,
    const char *needle
) {
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, -1, 2, NULL);
    require_contains(label, sqlite3_sql(stmt), needle);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_rewritten_prepare_failed_skip_log(sqlite3 *db, const char *log_path) {
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int saved_stderr;
    int log_fd;
    int old_limit;
    int limit;
    int rc;

    if (strlen(MATCH_SQL_LEAN) + 5 > (size_t)INT_MAX) {
        failf("FATAL: skip-log SQL length exceeds int range");
    }
    limit = (int)strlen(MATCH_SQL_LEAN) + 5;

    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) failf("FATAL: dup(stderr) failed: %s", strerror(errno));
    log_fd = open(log_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (log_fd < 0) {
        close(saved_stderr);
        failf("FATAL: open %s failed: %s", log_path, strerror(errno));
    }
    if (dup2(log_fd, STDERR_FILENO) < 0) {
        close(log_fd);
        close(saved_stderr);
        failf("FATAL: dup2(stderr) failed: %s", strerror(errno));
    }
    close(log_fd);

    /* Emby's over-limit lever maps to build_failed; Plex maps it to
       rewritten_prepare_failed, so this Q7 assertion depends on DIV-E keeping
       Plex wrapper SQL-length handling unchanged. */
    old_limit = sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, limit);
    rc = sqlite3_prepare_v2(db, MATCH_SQL_LEAN, -1, &stmt, &tail);
    sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, old_limit);
    fflush(stderr);
    if (dup2(saved_stderr, STDERR_FILENO) < 0) _exit(125);
    close(saved_stderr);

    if (rc != SQLITE_OK) {
        failf("FAIL [skip-log/prepare]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    }
    require_str_eq("skip-log/sql", sqlite3_sql(stmt), MATCH_SQL_LEAN);
    if (tail != MATCH_SQL_LEAN + strlen(MATCH_SQL_LEAN)) {
        failf("FAIL [skip-log/tail]: tail_offset=%ld want=%ld",
              (long)(tail - MATCH_SQL_LEAN), (long)strlen(MATCH_SQL_LEAN));
    }
    require_int("skip-log/finalize", sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_ondeck_capture_miss_skip_log(sqlite3 *db, const char *log_path) {
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int saved_stderr;
    int log_fd;
    int rc;

    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) failf("FATAL: dup(stderr) failed: %s", strerror(errno));
    log_fd = open(log_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (log_fd < 0) {
        close(saved_stderr);
        failf("FATAL: open %s failed: %s", log_path, strerror(errno));
    }
    if (dup2(log_fd, STDERR_FILENO) < 0) {
        close(log_fd);
        close(saved_stderr);
        failf("FATAL: dup2(stderr) failed: %s", strerror(errno));
    }
    close(log_fd);

    rc = sqlite3_prepare_v2(db, ONDECK_PARAM_LIST_SQL, -1, &stmt, &tail);
    fflush(stderr);
    if (dup2(saved_stderr, STDERR_FILENO) < 0) _exit(125);
    close(saved_stderr);

    if (rc != SQLITE_OK) {
        failf("FAIL [ondeck-capture-miss-log/prepare]: rc=%d err=%s",
              rc, sqlite3_errmsg(db));
    }
    require_str_eq("ondeck-capture-miss-log/sql", sqlite3_sql(stmt), ONDECK_PARAM_LIST_SQL);
    if (tail != ONDECK_PARAM_LIST_SQL + strlen(ONDECK_PARAM_LIST_SQL)) {
        failf("FAIL [ondeck-capture-miss-log/tail]: tail_offset=%ld want=%ld",
              (long)(tail - ONDECK_PARAM_LIST_SQL), (long)strlen(ONDECK_PARAM_LIST_SQL));
    }
    require_int("ondeck-capture-miss-log/finalize", sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_tag_applied_log(sqlite3 *db, const char *log_path) {
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int saved_stderr;
    int log_fd;
    int rc;

    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) failf("FATAL: dup(stderr) failed: %s", strerror(errno));
    log_fd = open(log_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (log_fd < 0) {
        close(saved_stderr);
        failf("FATAL: open %s failed: %s", log_path, strerror(errno));
    }
    if (dup2(log_fd, STDERR_FILENO) < 0) {
        close(log_fd);
        close(saved_stderr);
        failf("FATAL: dup2(stderr) failed: %s", strerror(errno));
    }
    close(log_fd);

    rc = sqlite3_prepare_v2(db, TAG_BROWSE_SQL, -1, &stmt, &tail);
    fflush(stderr);
    if (dup2(saved_stderr, STDERR_FILENO) < 0) _exit(125);
    close(saved_stderr);

    if (rc != SQLITE_OK) {
        failf("FAIL [tag-applied-log/prepare]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    }
    require_contains("tag-applied-log/sql", sqlite3_sql(stmt), TAG_MEMBERSHIP_10);
    if (tail != TAG_BROWSE_SQL + strlen(TAG_BROWSE_SQL)) {
        failf("FAIL [tag-applied-log/tail]: tail_offset=%ld want=%ld",
              (long)(tail - TAG_BROWSE_SQL), (long)strlen(TAG_BROWSE_SQL));
    }
    require_int("tag-applied-log/finalize", sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_legacy_authorizer(sqlite3 *db, int expect_rewrite) {
    auth_probe probe = {0, 0};
    sqlite3_stmt *stmt = NULL;

    require_int("legacy-authorizer/set", sqlite3_set_authorizer(db, authorizer_cb, &probe), SQLITE_OK);
    stmt = prepare_entry(db, "legacy-authorizer", MATCH_SQL_INT, -1, 1, NULL);
    require_int("legacy-authorizer/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    require_int("legacy-authorizer/clear", sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    if (expect_rewrite && probe.unlikely_calls < 1) {
        failf("FAIL [legacy-authorizer]: expected unlikely authorizer call, got=%d", probe.unlikely_calls);
    }
    if (!expect_rewrite && probe.unlikely_calls != 0) {
        failf("FAIL [legacy-authorizer]: unexpected unlikely authorizer calls=%d", probe.unlikely_calls);
    }
}

static void expect_tail_null_ok(sqlite3 *db, const char *label, const char *want_sql) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, MATCH_SQL_INT, -1, &stmt, NULL);
    if (rc != SQLITE_OK) failf("FAIL [%s]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    require_str_eq(label, sqlite3_sql(stmt), want_sql);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void expect_clean_original_single_id(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int bind_param,
    int bind_value,
    int want_id
) {
    auth_probe probe = {0, 1};
    sqlite3_stmt *stmt = NULL;
    int rc;
    int got_id;

    require_int("clean-original/set-authorizer", sqlite3_set_authorizer(db, authorizer_cb, &probe), SQLITE_OK);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    require_int("clean-original/clear-authorizer", sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    if (rc != SQLITE_OK) failf("FAIL [%s]: prepare rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    require_str_eq(label, sqlite3_sql(stmt), sql);
    if (probe.unlikely_calls != 0) {
        failf("FAIL [%s]: rewrite attempt count=%d want=0", label, probe.unlikely_calls);
    }
    if (bind_param) {
        require_int("clean-original/bind", sqlite3_bind_int(stmt, 1, bind_value), SQLITE_OK);
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        failf("FAIL [%s]: first step rc=%d want=SQLITE_ROW id=%d err=%s",
              label, rc, want_id, sqlite3_errmsg(db));
    }
    got_id = sqlite3_column_int(stmt, 0);
    if (got_id != want_id) failf("FAIL [%s]: got_id=%d want_id=%d", label, got_id, want_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) failf("FAIL [%s]: second step rc=%d want=SQLITE_DONE", label, rc);
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void collect_first_int_ids(
    sqlite3 *db,
    const char *label,
    const char *sql,
    int nbyte,
    int expect_membership,
    char *out,
    size_t out_n
) {
    const char *tail = NULL;
    sqlite3_stmt *stmt = prepare_entry(db, label, sql, nbyte, 2, &tail);
    int rc;
    int rows = 0;
    size_t used = 0;

    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s/tail]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    if (expect_membership) {
        require_contains(label, sqlite3_sql(stmt), TAG_MEMBERSHIP_10);
    } else {
        require_absent(label, sqlite3_sql(stmt), TAG_MEMBERSHIP_10);
    }
    if (out_n == 0) failf("FAIL [%s]: output buffer empty", label);
    out[0] = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int n = snprintf(out + used, out_n - used, "%d,", sqlite3_column_int(stmt, 0));
        if (n < 0 || (size_t)n >= out_n - used) {
            failf("FAIL [%s]: row-id buffer too small", label);
        }
        used += (size_t)n;
        rows++;
    }
    if (rc != SQLITE_DONE) {
        failf("FAIL [%s/step]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
    if (rows == 0) failf("FAIL [%s]: expected rows", label);
    require_int("first-int-ids/finalize", sqlite3_finalize(stmt), SQLITE_OK);
}

static uint64_t fnv1a_u64(uint64_t h, uint64_t v) {
    int i;
    for (i = 0; i < 8; i++) {
        h ^= (unsigned char)(v & 0xffu);
        h *= 1099511628211ULL;
        v >>= 8;
    }
    return h;
}

static digest_result digest_grouped(sqlite3 *db, const char *sql, int bind_mode) {
    sqlite3_stmt *stmt = NULL;
    digest_result result = {0, 1469598103934665603ULL};
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) failf("FAIL [digest/prepare]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    sqlite3_bind_text(stmt, 1, "Django*", -1, SQLITE_STATIC);
    if (bind_mode == 0) sqlite3_bind_int(stmt, 2, 6);
    else if (bind_mode == 1) sqlite3_bind_text(stmt, 2, "6", -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int(stmt, 3, 1);
    sqlite3_bind_int(stmt, 4, 1);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        result.rows++;
        result.hash = fnv1a_u64(result.hash, (uint64_t)sqlite3_column_int64(stmt, 0));
        result.hash = fnv1a_u64(result.hash, (uint64_t)sqlite3_column_int64(stmt, 1));
    }
    if (rc != SQLITE_DONE) failf("FAIL [digest/step]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    require_int("digest/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    return result;
}

static void expect_grouped_digest_identity(sqlite3 *db) {
    static const char *original =
        "select tags.id, count(*) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match ? and tag_type=? and metadata_items.library_section_id in (?) and metadata_items.metadata_type=? group by tags.id order by tags.id, count(*)";
    static const char *rewritten =
        "select tags.id, count(*) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match ? and unlikely(tag_type=?) and metadata_items.library_section_id in (?) and metadata_items.metadata_type=? group by tags.id order by tags.id, count(*)";
    int mode;
    for (mode = 0; mode < 3; mode++) {
        digest_result a = digest_grouped(db, original, mode);
        digest_result b = digest_grouped(db, rewritten, mode);
        if (a.rows != b.rows || a.hash != b.hash) {
            failf("FAIL [grouped-digest mode=%d]: original rows=%d hash=%llu rewritten rows=%d hash=%llu",
                  mode, a.rows, (unsigned long long)a.hash, b.rows, (unsigned long long)b.hash);
        }
    }
}

static void expect_fail_open(sqlite3 *db, int expect_rewrite_attempt) {
    auth_probe probe = {0, 1};
    sqlite3_stmt *stmt = NULL;
    int rc;

    require_int("fail-open/set-authorizer", sqlite3_set_authorizer(db, authorizer_cb, &probe), SQLITE_OK);
    rc = sqlite3_prepare_v2(db, MATCH_SQL_INT, -1, &stmt, NULL);
    if (rc != SQLITE_OK) failf("FAIL [fail-open]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    require_str_eq("fail-open/sql", sqlite3_sql(stmt), MATCH_SQL_INT);
    require_int("fail-open/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    require_int("fail-open/clear-authorizer", sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    if (expect_rewrite_attempt && probe.unlikely_calls < 1) {
        failf("FAIL [fail-open]: expected denied unlikely attempt, got=%d", probe.unlikely_calls);
    }
    if (!expect_rewrite_attempt && probe.unlikely_calls != 0) {
        failf("FAIL [fail-open]: unexpected denied unlikely attempts=%d", probe.unlikely_calls);
    }
}

static void expect_original_without_unlikely(sqlite3 *db, const char *label, const char *sql) {
    auth_probe probe = {0, 1};

    require_int("original-no-unlikely/set-authorizer",
                sqlite3_set_authorizer(db, authorizer_cb, &probe), SQLITE_OK);
    expect_saved_sql(db, label, sql, -1, 2, sql);
    require_int("original-no-unlikely/clear-authorizer",
                sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    if (probe.unlikely_calls != 0) {
        failf("FAIL [%s]: rewrite attempt count=%d want=0", label, probe.unlikely_calls);
    }
}

static sqlite3 *open_seeded_temp(const char *basename) {
    char path[512];
    temp_path(path, sizeof(path), basename);
    unlink(path);
    snprintf(path, sizeof(path), "/tmp/plex-fts-rewrite-smoke-%ld/%s-wal", (long)getpid(), basename);
    unlink(path);
    snprintf(path, sizeof(path), "/tmp/plex-fts-rewrite-smoke-%ld/%s-shm", (long)getpid(), basename);
    unlink(path);
    temp_path(path, sizeof(path), basename);
    sqlite3 *db = open_db(path);
    setup_schema(db);
    return db;
}

static int child_positive(void) {
    int expect_rewrite;
    sqlite3 *db;

    configure_env("0");
    make_temp_dir();
    expect_rewrite = sqlite3_compileoption_used("ENABLE_ICU") != 0;
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_saved_sql(db, "v2-int", MATCH_SQL_INT, -1, 2,
                     expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    expect_saved_sql(db, "v3-int", MATCH_SQL_INT, -1, 3,
                     expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    expect_saved_sql(db, "lean-no-metadata-group-limit", MATCH_SQL_LEAN, -1, 2,
                     expect_rewrite ? MATCH_SQL_LEAN_REWRITTEN : MATCH_SQL_LEAN);
    expect_saved_sql(db, "projected-tag-type", PROJECTED_TAG_TYPE_SQL, -1, 2,
                     expect_rewrite ? PROJECTED_TAG_TYPE_SQL_REWRITTEN : PROJECTED_TAG_TYPE_SQL);
    expect_saved_sql(db, "nbyte-positive-nul", MATCH_SQL_INT, (int)strlen(MATCH_SQL_INT) + 1, 2,
                     expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    expect_saved_sql(db, "nbyte-positive-no-nul", MATCH_SQL_INT, (int)strlen(MATCH_SQL_INT), 2,
                     expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    expect_tail_null_ok(db, "pztail-null", expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    if (expect_rewrite) {
        expect_saved_sql_contains(db, "quoted", MATCH_SQL_QUOTED, "unlikely(tag_type='6')");
        expect_saved_sql_contains(db, "param", MATCH_SQL_PARAM, "unlikely(tag_type=?)");
    } else {
        expect_saved_sql(db, "quoted", MATCH_SQL_QUOTED, -1, 2, MATCH_SQL_QUOTED);
        expect_saved_sql(db, "param", MATCH_SQL_PARAM, -1, 2, MATCH_SQL_PARAM);
    }
    expect_legacy_authorizer(db, expect_rewrite);
    expect_grouped_digest_identity(db);
    exec_sql(db, "exec-miss", "PRAGMA user_version;");
    require_int("positive/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [positive]: expect_rewrite=%d\n", expect_rewrite);
    return 0;
}

static int child_env_case(const char *value, const char *label, int env_enables_rewrite) {
    int expect_rewrite;
    sqlite3 *db;
    configure_env(value);
    make_temp_dir();
    expect_rewrite = env_enables_rewrite && sqlite3_compileoption_used("ENABLE_ICU") != 0;
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_saved_sql(db, label, MATCH_SQL_INT, -1, 2,
                     expect_rewrite ? MATCH_SQL_INT_REWRITTEN : MATCH_SQL_INT);
    expect_legacy_authorizer(db, expect_rewrite);
    require_int("env-case/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [%s]: expect_rewrite=%d\n", label, expect_rewrite);
    return 0;
}

static int child_path_negative(void) {
    const char *names[] = {"library.db", "jellyfin.db", "not-target.db"};
    char non_ascii_dir[512];
    char non_ascii_path[768];
    size_t i;
    configure_env("0");
    make_temp_dir();
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        sqlite3 *db = open_seeded_temp(names[i]);
        expect_saved_sql(db, names[i], MATCH_SQL_INT, -1, 2, MATCH_SQL_INT);
        require_int("path-negative/close", sqlite3_close(db), SQLITE_OK);
    }
    {
        sqlite3 *db = open_db(":memory:");
        setup_schema(db);
        expect_saved_sql(db, "memory", MATCH_SQL_INT, -1, 2, MATCH_SQL_INT);
        require_int("path-negative/memory-close", sqlite3_close(db), SQLITE_OK);
    }
    {
        auth_probe probe = {0, 1};
        int rc = snprintf(non_ascii_dir, sizeof(non_ascii_dir),
                          "/tmp/plex-fts-rewrite-smoke-%ld/pl\303\251x",
                          (long)getpid());
        sqlite3 *db;
        if (rc < 0 || (size_t)rc >= sizeof(non_ascii_dir)) {
            failf("FATAL: non-ASCII temp dir too long");
        }
        if (mkdir(non_ascii_dir, 0700) != 0 && errno != EEXIST) {
            failf("FATAL: mkdir(%s) failed: %s", non_ascii_dir, strerror(errno));
        }
        rc = snprintf(non_ascii_path, sizeof(non_ascii_path),
                      "%s/com.plexapp.plugins.library.db", non_ascii_dir);
        if (rc < 0 || (size_t)rc >= sizeof(non_ascii_path)) {
            failf("FATAL: non-ASCII temp path too long");
        }
        unlink(non_ascii_path);
        db = open_db(non_ascii_path);
        setup_schema(db);
        require_int("path-negative/non-ascii-set-authorizer",
                    sqlite3_set_authorizer(db, authorizer_cb, &probe), SQLITE_OK);
        expect_saved_sql(db, "non-ascii-path", MATCH_SQL_INT, -1, 2, MATCH_SQL_INT);
        require_int("path-negative/non-ascii-clear-authorizer",
                    sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
        if (probe.unlikely_calls != 0) {
            failf("FAIL [non-ascii-path]: rewrite attempt count=%d want=0",
                  probe.unlikely_calls);
        }
        require_int("path-negative/non-ascii-close", sqlite3_close(db), SQLITE_OK);
        unlink(non_ascii_path);
        rc = snprintf(non_ascii_path, sizeof(non_ascii_path),
                      "%s/com.plexapp.plugins.library.db-wal", non_ascii_dir);
        if (rc >= 0 && (size_t)rc < sizeof(non_ascii_path)) unlink(non_ascii_path);
        rc = snprintf(non_ascii_path, sizeof(non_ascii_path),
                      "%s/com.plexapp.plugins.library.db-shm", non_ascii_dir);
        if (rc >= 0 && (size_t)rc < sizeof(non_ascii_path)) unlink(non_ascii_path);
        rmdir(non_ascii_dir);
    }
    cleanup_temp_dir();
    printf("PASS [path-negative]\n");
    return 0;
}

static int child_nonmatch(void) {
    sqlite3 *db;
    configure_env("0");
    make_temp_dir();
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_saved_sql(db, "nonmatch-select", NONMATCH_SQL, -1, 2, NONMATCH_SQL);
    expect_saved_sql(db, "no-fts-table", NO_FTS_SQL, -1, 2, NO_FTS_SQL);
    expect_saved_sql(db, "no-target-column", NO_TARGET_SQL, -1, 2, NO_TARGET_SQL);
    expect_original_without_unlikely(db, "match-named-param", MATCH_SQL_NAMED_MATCH_PARAM);
    expect_original_without_unlikely(db, "match-numbered-param", MATCH_SQL_NUMBERED_MATCH_PARAM);
    expect_saved_sql(db, "duplicate-target-column", DUPLICATE_TARGET_SQL, -1, 2,
                     DUPLICATE_TARGET_SQL);
    expect_saved_sql(db, "cross-scope-cte", CROSS_SCOPE_CTE_SQL, -1, 2, CROSS_SCOPE_CTE_SQL);
    expect_saved_sql(db, "cross-scope-subquery", CROSS_SCOPE_SUBQUERY_SQL, -1, 2,
                     CROSS_SCOPE_SUBQUERY_SQL);
    expect_saved_sql(db, "projection-only-target-column", PROJECTION_ONLY_TAG_TYPE_EQ_SQL, -1,
                     2, PROJECTION_ONLY_TAG_TYPE_EQ_SQL);
    expect_saved_sql(db, "order-only-target-column", ORDER_ONLY_TAG_TYPE_EQ_SQL, -1, 2,
                     ORDER_ONLY_TAG_TYPE_EQ_SQL);
    expect_original_without_unlikely(db, "left-bound-target-column", LEFT_BOUND_TAG_TYPE_EQ_SQL);
    expect_clean_original_single_id(db, "boundary-plus-int", BOUNDARY_PLUS_INT_SQL, 0, 0, 14);
    expect_clean_original_single_id(db, "boundary-plus-param", BOUNDARY_PLUS_PARAM_SQL, 1, 6, 14);
    expect_clean_original_single_id(db, "boundary-hex", BOUNDARY_HEX_SQL, 0, 0, 15);
    expect_fail_open(db, sqlite3_compileoption_used("ENABLE_ICU") != 0);
    require_int("nonmatch/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [nonmatch]\n");
    return 0;
}

static int child_tag_membership(void) {
    int expect_rewrite;
    sqlite3 *db;
    sqlite3 *missing_index_db;
    sqlite3 *non_target_db;

    configure_env_all("1", "0", "1");
    make_temp_dir();
    expect_rewrite = sqlite3_compileoption_used("ENABLE_ICU") != 0;
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    create_tag_membership_index(db);
    expect_saved_sql(db, "tag-browse", TAG_BROWSE_SQL, -1, 2,
                     expect_rewrite ? TAG_BROWSE_SQL_REWRITTEN : TAG_BROWSE_SQL);
    expect_saved_sql(db, "tag-count", TAG_COUNT_SQL, -1, 2,
                     expect_rewrite ? TAG_COUNT_SQL_REWRITTEN : TAG_COUNT_SQL);
    expect_saved_sql(db, "tag-limit", TAG_LIMIT_SQL, -1, 2,
                     expect_rewrite ? TAG_LIMIT_SQL_REWRITTEN : TAG_LIMIT_SQL);
    expect_saved_sql(db, "tag-flipped-join", TAG_FLIPPED_JOIN_SQL, -1, 2,
                     expect_rewrite ? TAG_FLIPPED_JOIN_SQL_REWRITTEN : TAG_FLIPPED_JOIN_SQL);
    expect_saved_sql(db, "tag-already-rewritten", TAG_BROWSE_SQL_REWRITTEN, -1, 2,
                     TAG_BROWSE_SQL_REWRITTEN);
    expect_saved_sql(db, "tag-missing-join", TAG_MISSING_JOIN_SQL, -1, 2,
                     TAG_MISSING_JOIN_SQL);
    expect_saved_sql(db, "tag-nested-join", TAG_NESTED_JOIN_SQL, -1, 2,
                     TAG_NESTED_JOIN_SQL);
    expect_saved_sql(db, "tag-join-in-string", TAG_JOIN_IN_STRING_SQL, -1, 2,
                     TAG_JOIN_IN_STRING_SQL);
    expect_saved_sql(db, "tag-join-in-comment", TAG_JOIN_IN_COMMENT_SQL, -1, 2,
                     TAG_JOIN_IN_COMMENT_SQL);
    expect_saved_sql(db, "tag-duplicate-id", TAG_DUPLICATE_ID_SQL, -1, 2,
                     TAG_DUPLICATE_ID_SQL);
    expect_saved_sql(db, "tag-bound-id", TAG_BOUND_ID_SQL, -1, 2, TAG_BOUND_ID_SQL);
    expect_saved_sql(db, "tag-named-id", TAG_NAMED_ID_SQL, -1, 2, TAG_NAMED_ID_SQL);
    expect_saved_sql(db, "tag-string-id", TAG_STRING_ID_SQL, -1, 2, TAG_STRING_ID_SQL);
    expect_saved_sql(db, "tag-or-id", TAG_OR_ID_SQL, -1, 2, TAG_OR_ID_SQL);
    expect_saved_sql(db, "tag-not-id", TAG_NOT_ID_SQL, -1, 2, TAG_NOT_ID_SQL);
    expect_saved_sql(db, "tag-value-compared-id", TAG_VALUE_COMPARED_ID_SQL, -1, 2,
                     TAG_VALUE_COMPARED_ID_SQL);
    expect_saved_sql(db, "tag-column-rhs-only", TAG_COLUMN_RHS_ONLY_SQL, -1, 2,
                     TAG_COLUMN_RHS_ONLY_SQL);
    expect_saved_sql(db, "tag-metadata-fts", TAG_METADATA_FTS_SQL, -1, 2,
                     TAG_METADATA_FTS_SQL);
    expect_saved_sql(db, "tag-unrelated-fts", MATCH_SQL_INT, -1, 2, MATCH_SQL_INT);
    require_int("tag-membership/close", sqlite3_close(db), SQLITE_OK);

    missing_index_db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_saved_sql(missing_index_db, "tag-missing-index", TAG_BROWSE_SQL, -1, 2,
                     TAG_BROWSE_SQL);
    require_int("tag-membership/missing-index-close",
                sqlite3_close(missing_index_db), SQLITE_OK);

    non_target_db = open_seeded_temp("library.db");
    create_tag_membership_index(non_target_db);
    expect_saved_sql(non_target_db, "tag-non-target", TAG_BROWSE_SQL, -1, 2,
                     TAG_BROWSE_SQL);
    require_int("tag-membership/non-target-close", sqlite3_close(non_target_db), SQLITE_OK);

    cleanup_temp_dir();
    printf("PASS [tag-membership]: expect_rewrite=%d\n", expect_rewrite);
    return 0;
}

static int child_tag_row_parity(void) {
    sqlite3 *original_db;
    sqlite3 *rewritten_db;
    char original_ids[128];
    char rewritten_ids[128];
    int expect_rewrite;

    configure_env_all("1", "0", "1");
    make_temp_dir();
    expect_rewrite = sqlite3_compileoption_used("ENABLE_ICU") != 0;
    original_db = open_seeded_temp("not-target.db");
    rewritten_db = open_seeded_temp("com.plexapp.plugins.library.db");
    create_tag_membership_index(original_db);
    create_tag_membership_index(rewritten_db);
    exec_sql(original_db, "tag-row-parity-original-extra",
             "INSERT INTO taggings(id,metadata_item_id,tag_id,`index`) VALUES(7,2,10,7);");
    exec_sql(rewritten_db, "tag-row-parity-rewritten-extra",
             "INSERT INTO taggings(id,metadata_item_id,tag_id,`index`) VALUES(7,2,10,7);");

    collect_first_int_ids(original_db, "tag-row-parity-original", TAG_LIMIT_SQL, -1, 0,
                          original_ids, sizeof(original_ids));
    collect_first_int_ids(rewritten_db, "tag-row-parity-rewritten", TAG_LIMIT_SQL,
                          (int)strlen(TAG_LIMIT_SQL) + 1, expect_rewrite,
                          rewritten_ids, sizeof(rewritten_ids));
    require_str_eq("tag-row-parity/ids", rewritten_ids, original_ids);
    require_str_eq("tag-row-parity/expected", rewritten_ids, "1,2,");

    require_int("tag-row-parity/original-close", sqlite3_close(original_db), SQLITE_OK);
    require_int("tag-row-parity/rewrite-close", sqlite3_close(rewritten_db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [tag-row-parity]: expect_rewrite=%d\n", expect_rewrite);
    return 0;
}

static int child_tag_env_case(const char *value, const char *label, int env_enables_rewrite) {
    int expect_rewrite;
    sqlite3 *db;

    configure_env_all("1", value, "1");
    make_temp_dir();
    expect_rewrite = env_enables_rewrite && sqlite3_compileoption_used("ENABLE_ICU") != 0;
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    create_tag_membership_index(db);
    expect_saved_sql(db, label, TAG_BROWSE_SQL, -1, 2,
                     expect_rewrite ? TAG_BROWSE_SQL_REWRITTEN : TAG_BROWSE_SQL);
    require_int("tag-env/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [%s]: expect_rewrite=%d\n", label, expect_rewrite);
    return 0;
}

static int child_ondeck(void) {
    int expect_rewrite;
    sqlite3 *db;
    sqlite3 *missing_index_db;
    sqlite3 *non_target_db;

    configure_env_all("1", "1", "0");
    make_temp_dir();
    expect_rewrite = sqlite3_compileoption_used("ENABLE_ICU") != 0;
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    create_ondeck_index(db);
    expect_saved_sql(db, "ondeck-positive", ONDECK_SQL, -1, 2,
                     expect_rewrite ? ONDECK_SQL_REWRITTEN : ONDECK_SQL);
    if (expect_rewrite) expect_ondeck_rows(db);
    expect_saved_sql(db, "ondeck-left-join", ONDECK_LEFT_JOIN_SQL, -1, 2,
                     ONDECK_LEFT_JOIN_SQL);
    expect_saved_sql(db, "ondeck-param-list", ONDECK_PARAM_LIST_SQL, -1, 2,
                     ONDECK_PARAM_LIST_SQL);
    expect_saved_sql(db, "ondeck-param-account", ONDECK_PARAM_ACCOUNT_SQL, -1, 2,
                     ONDECK_PARAM_ACCOUNT_SQL);
    expect_saved_sql(db, "ondeck-duplicate-account", ONDECK_DUP_ACCOUNT_SQL, -1, 2,
                     ONDECK_DUP_ACCOUNT_SQL);
    expect_saved_sql(db, "ondeck-missing-viewcount", ONDECK_MISSING_VIEWCOUNT_SQL, -1, 2,
                     ONDECK_MISSING_VIEWCOUNT_SQL);
    expect_saved_sql(db, "ondeck-missing-settings-guid", ONDECK_MISSING_SETTINGS_GUID_SQL, -1, 2,
                     ONDECK_MISSING_SETTINGS_GUID_SQL);
    expect_saved_sql(db, "ondeck-missing-settings-account", ONDECK_MISSING_SETTINGS_ACCOUNT_SQL, -1, 2,
                     ONDECK_MISSING_SETTINGS_ACCOUNT_SQL);
    expect_saved_sql(db, "ondeck-projection-drift", ONDECK_PROJECTION_DRIFT_SQL, -1, 2,
                     ONDECK_PROJECTION_DRIFT_SQL);
    require_int("ondeck/close", sqlite3_close(db), SQLITE_OK);

    missing_index_db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_saved_sql(missing_index_db, "ondeck-missing-index", ONDECK_SQL, -1, 2,
                     ONDECK_SQL);
    require_int("ondeck/missing-index-close", sqlite3_close(missing_index_db), SQLITE_OK);

    non_target_db = open_seeded_temp("library.db");
    create_ondeck_index(non_target_db);
    expect_saved_sql(non_target_db, "ondeck-non-target", ONDECK_SQL, -1, 2,
                     ONDECK_SQL);
    require_int("ondeck/non-target-close", sqlite3_close(non_target_db), SQLITE_OK);

    cleanup_temp_dir();
    printf("PASS [ondeck]: expect_rewrite=%d\n", expect_rewrite);
    return 0;
}

static int child_ondeck_env_case(const char *value, const char *label, int env_enables_rewrite) {
    int expect_rewrite;
    sqlite3 *db;

    configure_env_all("1", "1", value);
    make_temp_dir();
    expect_rewrite = env_enables_rewrite && sqlite3_compileoption_used("ENABLE_ICU") != 0;
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    create_ondeck_index(db);
    expect_saved_sql(db, label, ONDECK_SQL, -1, 2,
                     expect_rewrite ? ONDECK_SQL_REWRITTEN : ONDECK_SQL);
    require_int("ondeck-env/close", sqlite3_close(db), SQLITE_OK);
    cleanup_temp_dir();
    printf("PASS [%s]: expect_rewrite=%d\n", label, expect_rewrite);
    return 0;
}

static int child_skip_log(void) {
    sqlite3 *db;
    char log_path[512];
    char *log_text;
    static const char *needle =
        "event=rewrite_skipped target=plex reason=rewritten_prepare_failed mode=fts+tag_type";

    if (sqlite3_compileoption_used("ENABLE_ICU") == 0) {
        printf("SKIP [skip-log]: ENABLE_ICU=0\n");
        return 0;
    }
    if (!configure_obs_enabled_env()) return 1;
    make_temp_dir();
    temp_path(log_path, sizeof(log_path), "rewrite-skipped.stderr");
    unlink(log_path);
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_rewritten_prepare_failed_skip_log(db, log_path);
    log_text = read_text_file(log_path);
    require_occurrences("skip-log/rewrite-skipped", log_text, needle, 1);
    free(log_text);
    require_int("skip-log/close", sqlite3_close(db), SQLITE_OK);
    unlink(log_path);
    cleanup_temp_dir();
    printf("PASS [skip-log]\n");
    return 0;
}

static int child_tag_applied_log(void) {
    sqlite3 *db;
    char log_path[512];
    char *log_text;
    static const char *needle =
        "event=rewrite_applied target=plex mode=taggings+membership";

    if (sqlite3_compileoption_used("ENABLE_ICU") == 0) {
        printf("SKIP [tag-applied-log]: ENABLE_ICU=0\n");
        return 0;
    }
    if (!configure_obs_tag_enabled_env()) return 1;
    make_temp_dir();
    temp_path(log_path, sizeof(log_path), "tag-applied.stderr");
    unlink(log_path);
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    create_tag_membership_index(db);
    expect_tag_applied_log(db, log_path);
    log_text = read_text_file(log_path);
    require_occurrences("tag-applied-log/rewrite-applied", log_text, needle, 1);
    free(log_text);
    require_int("tag-applied-log/close", sqlite3_close(db), SQLITE_OK);
    unlink(log_path);
    cleanup_temp_dir();
    printf("PASS [tag-applied-log]\n");
    return 0;
}

static int child_ondeck_capture_miss_log(void) {
    sqlite3 *db;
    char log_path[512];
    char *log_text;
    static const char *needle =
        "event=rewrite_skipped target=plex reason=capture_miss mode=ondeck";

    if (sqlite3_compileoption_used("ENABLE_ICU") == 0) {
        printf("SKIP [ondeck-capture-miss-log]: ENABLE_ICU=0\n");
        return 0;
    }
    if (!configure_obs_ondeck_enabled_env()) return 1;
    make_temp_dir();
    temp_path(log_path, sizeof(log_path), "ondeck-capture-miss.stderr");
    unlink(log_path);
    db = open_seeded_temp("com.plexapp.plugins.library.db");
    expect_ondeck_capture_miss_skip_log(db, log_path);
    log_text = read_text_file(log_path);
    require_occurrences("ondeck-capture-miss-log/rewrite-skipped", log_text, needle, 1);
    free(log_text);
    require_int("ondeck-capture-miss-log/close", sqlite3_close(db), SQLITE_OK);
    unlink(log_path);
    cleanup_temp_dir();
    printf("PASS [ondeck-capture-miss-log]\n");
    return 0;
}

static int run_child_body(const char *name) {
    if (strcmp(name, "positive") == 0) return child_positive();
    if (strcmp(name, "env-default") == 0) return child_env_case(NULL, "env-default", 1);
    if (strcmp(name, "env-one-disabled") == 0) return child_env_case("1", "env-one-disabled", 0);
    if (strcmp(name, "env-garbage-enabled") == 0) return child_env_case("false", "env-garbage-enabled", 1);
    if (strcmp(name, "path-negative") == 0) return child_path_negative();
    if (strcmp(name, "nonmatch") == 0) return child_nonmatch();
    if (strcmp(name, "tag-membership") == 0) return child_tag_membership();
    if (strcmp(name, "tag-row-parity") == 0) return child_tag_row_parity();
    if (strcmp(name, "tag-env-default") == 0) return child_tag_env_case(NULL, "tag-env-default", 0);
    if (strcmp(name, "tag-env-one-disabled") == 0) return child_tag_env_case("1", "tag-env-one-disabled", 0);
    if (strcmp(name, "tag-env-garbage-disabled") == 0) return child_tag_env_case("false", "tag-env-garbage-disabled", 0);
    if (strcmp(name, "ondeck") == 0) return child_ondeck();
    if (strcmp(name, "ondeck-env-default") == 0) return child_ondeck_env_case(NULL, "ondeck-env-default", 0);
    if (strcmp(name, "ondeck-env-one-disabled") == 0) return child_ondeck_env_case("1", "ondeck-env-one-disabled", 0);
    if (strcmp(name, "ondeck-env-garbage-disabled") == 0) return child_ondeck_env_case("false", "ondeck-env-garbage-disabled", 0);
    if (strcmp(name, "skip-log") == 0) return child_skip_log();
    if (strcmp(name, "tag-applied-log") == 0) return child_tag_applied_log();
    if (strcmp(name, "ondeck-capture-miss-log") == 0) return child_ondeck_capture_miss_log();
    failf("FATAL: unknown child %s", name);
    return 1;
}

static void wait_for_child(pid_t pid, const char *name) {
    int status;
    if (waitpid(pid, &status, 0) < 0) failf("FATAL: waitpid(%s) failed: %s", name, strerror(errno));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        failf("FATAL: child %s failed status=%d", name, status);
    }
}

static void run_child(const char *name) {
    pid_t pid = fork();
    if (pid < 0) failf("FATAL: fork(%s) failed: %s", name, strerror(errno));
    if (pid == 0) _exit(run_child_body(name));
    wait_for_child(pid, name);
}

static void run_exec_child(const char *name) {
    pid_t pid = fork();
    if (pid < 0) failf("FATAL: fork-exec(%s) failed: %s", name, strerror(errno));
    if (pid == 0) {
        if (!configure_obs_enabled_env()) _exit(127);
        execlp(g_program_path, g_program_path, "--child", name, (char *)NULL);
        fprintf(stderr, "exec(%s) failed: %s\n", g_program_path, strerror(errno));
        _exit(127);
    }
    wait_for_child(pid, name);
}

int main(int argc, char **argv) {
    g_program_path = argv[0];
    if (argc == 3 && strcmp(argv[1], "--child") == 0) {
        return run_child_body(argv[2]);
    }
    if (argc != 1) failf("FATAL: unexpected arguments");

    run_child("positive");
    run_child("env-default");
    run_child("env-one-disabled");
    run_child("env-garbage-enabled");
    run_child("path-negative");
    run_child("nonmatch");
    run_child("tag-membership");
    run_child("tag-row-parity");
    run_child("tag-env-default");
    run_child("tag-env-one-disabled");
    run_child("tag-env-garbage-disabled");
    run_child("ondeck");
    run_child("ondeck-env-default");
    run_child("ondeck-env-one-disabled");
    run_child("ondeck-env-garbage-disabled");
    if (sqlite3_compileoption_used("ENABLE_ICU") != 0) {
        run_exec_child("skip-log");
        run_exec_child("tag-applied-log");
        run_exec_child("ondeck-capture-miss-log");
    } else {
        printf("SKIP [skip-log]: ENABLE_ICU=0\n");
        printf("SKIP [tag-applied-log]: ENABLE_ICU=0\n");
        printf("SKIP [ondeck-capture-miss-log]: ENABLE_ICU=0\n");
    }
    printf("plex fts rewrite smoke passed\n");
    return 0;
}
