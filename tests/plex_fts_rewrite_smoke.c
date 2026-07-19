#include "rewrite_smoke_harness.h"
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

static const char MATCH_SQL_INT[] =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id  join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6  and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1   group by tags.id order by count(*) desc limit 100";
static const char MATCH_SQL_INT_REWRITTEN[] =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id  join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and unlikely(tag_type=6)  and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1   group by tags.id order by count(*) desc limit 100";
static const char MATCH_SQL_LEAN[] =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6";
static const char MATCH_SQL_LEAN_REWRITTEN[] =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and unlikely(tag_type=6)";
static const char MATCH_SQL_QUOTED[] =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type='6' and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 group by tags.id order by count(*) desc limit 100";
static const char MATCH_SQL_QUOTED_REWRITTEN[] =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and unlikely(tag_type='6') and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 group by tags.id order by count(*) desc limit 100";
static const char MATCH_SQL_PARAM[] =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=? and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 group by tags.id order by count(*) desc limit 100";
static const char MATCH_SQL_PARAM_REWRITTEN[] =
    "select distinct(tags.id) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and unlikely(tag_type=?) and metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 group by tags.id order by count(*) desc limit 100";
static const char MATCH_SQL_NAMED_MATCH_PARAM[] =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match @SearchTerm and tag_type=6";
static const char MATCH_SQL_NUMBERED_MATCH_PARAM[] =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match ?1 and tag_type=6";
static const char PROJECTED_TAG_TYPE_SQL[] =
    "select tag_type, tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6 order by tag_type, tags.id";
static const char PROJECTED_TAG_TYPE_SQL_REWRITTEN[] =
    "select tag_type, tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and unlikely(tag_type=6) order by tag_type, tags.id";
static const char BOUNDARY_PLUS_INT_SQL[] =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6+1 order by tags.id";
static const char BOUNDARY_PLUS_PARAM_SQL[] =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=?+1 order by tags.id";
static const char BOUNDARY_HEX_SQL[] =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=0x1f order by tags.id";
static const char NONMATCH_SQL[] = "select 1";
static const char NO_FTS_SQL[] =
    "select tags.id from tags where tag_type=6";
static const char NO_TARGET_SQL[] =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tags.id=10";
static const char DUPLICATE_TARGET_SQL[] =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and tag_type=6 and tag_type=1";
static const char CROSS_SCOPE_CTE_SQL[] =
    "with matched as (select rowid from fts4_tag_titles_icu where fts4_tag_titles_icu.tag match 'Django*') select tags.id from tags where tag_type=6 and tags.id in (select rowid from matched)";
static const char CROSS_SCOPE_SUBQUERY_SQL[] =
    "select tags.id from tags where tag_type=6 and exists (select 1 from fts4_tag_titles_icu where fts4_tag_titles_icu.rowid=tags.id and fts4_tag_titles_icu.tag match 'Django*')";
static const char PROJECTION_ONLY_TAG_TYPE_EQ_SQL[] =
    "select (tag_type=6) from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*'";
static const char ORDER_ONLY_TAG_TYPE_EQ_SQL[] =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' order by tag_type=6";
static const char LEFT_BOUND_TAG_TYPE_EQ_SQL[] =
    "select tags.id from tags join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match 'Django*' and 1 + tag_type=4";

static const char GUID_LIKE_SQL[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE (mt.`guid` LIKE :1) LIMIT :2";
static const char GUID_LIKE_SQL_REWRITTEN[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE :1 IS NOT NULL AND (mt.`guid` LIKE :1) LIMIT :2";
static const char GUID_LIKE_COLON_NAMED_SQL[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE (mt.`guid` LIKE :C1) LIMIT :C2";
static const char GUID_LIKE_COLON_NAMED_SQL_REWRITTEN[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE :C1 IS NOT NULL AND (mt.`guid` LIKE :C1) LIMIT :C2";
static const char GUID_LIKE_QMARK_NUMBERED_SQL[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE (mt.`guid` LIKE ?1) LIMIT ?2";
static const char GUID_LIKE_QMARK_NUMBERED_SQL_REWRITTEN[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE ?1 IS NOT NULL AND (mt.`guid` LIKE ?1) LIMIT ?2";
static const char GUID_LIKE_AT_NAMED_SQL[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE (mt.`guid` LIKE @pattern) LIMIT @limit";
static const char GUID_LIKE_AT_NAMED_SQL_REWRITTEN[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE @pattern IS NOT NULL AND (mt.`guid` LIKE @pattern) LIMIT @limit";
static const char GUID_LIKE_DOLLAR_NAMED_SQL[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE (mt.`guid` LIKE $pattern) LIMIT $limit";
static const char GUID_LIKE_DOLLAR_NAMED_SQL_REWRITTEN[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE $pattern IS NOT NULL AND (mt.`guid` LIKE $pattern) LIMIT $limit";
static const char GUID_LIKE_ANONYMOUS_SQL[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE (mt.`guid` LIKE ?) LIMIT ?";
static const char GUID_LIKE_ANONYMOUS_PATTERN_SQL[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE (mt.`guid` LIKE ?) LIMIT ?2";
static const char GUID_LIKE_ANONYMOUS_LIMIT_SQL[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE (mt.`guid` LIKE :C1) LIMIT ?";
static const char GUID_LIKE_NEGATIVE_SQL[] =
    "SELECT mt.`id` FROM metadata_items mt WHERE mt.`guid` LIKE :1 LIMIT :2";

#define TAG_MEMBERSHIP_10 " AND metadata_items.id IN (SELECT metadata_item_id FROM taggings WHERE tag_id=10)"

static const char TAG_BROWSE_SQL[] =
    "select metadata_items.id from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10)  order by taggings.`index` IS NULL,taggings.`index` asc, metadata_items.title_sort asc, metadata_items.originally_available_at asc, metadata_items.id asc limit 2 offset 0";
static const char TAG_BROWSE_SQL_REWRITTEN[] =
    "select metadata_items.id from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10" TAG_MEMBERSHIP_10 ")  order by taggings.`index` IS NULL,taggings.`index` asc, metadata_items.title_sort asc, metadata_items.originally_available_at asc, metadata_items.id asc limit 2 offset 0";
static const char TAG_COUNT_SQL[] =
    "select count(*) from (select distinct(metadata_items.id) from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10) )";
static const char TAG_COUNT_SQL_REWRITTEN[] =
    "select count(*) from (select distinct(metadata_items.id) from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10" TAG_MEMBERSHIP_10 ") )";
static const char TAG_LIMIT_SQL[] =
    "select metadata_items.id from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10)  order by taggings.`index` IS NULL,taggings.`index` asc, metadata_items.title_sort asc, metadata_items.originally_available_at asc, metadata_items.id asc limit 27 offset 0";
static const char TAG_LIMIT_SQL_REWRITTEN[] =
    "select metadata_items.id from metadata_items  left join taggings on taggings.metadata_item_id=metadata_items.id  left join tags on taggings.tag_id=tags.id  where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 and tags.id=10" TAG_MEMBERSHIP_10 ")  order by taggings.`index` IS NULL,taggings.`index` asc, metadata_items.title_sort asc, metadata_items.originally_available_at asc, metadata_items.id asc limit 27 offset 0";
static const char TAG_MISSING_JOIN_SQL[] =
    "select metadata_items.id from metadata_items left join taggings on taggings.tag_id=10 left join tags on taggings.tag_id=tags.id where tags.id=10";
static const char TAG_NESTED_JOIN_SQL[] =
    "select metadata_items.id from metadata_items left join tags on tags.id=10 where tags.id=10 and exists (select 1 from taggings where taggings.metadata_item_id=metadata_items.id and taggings.tag_id=tags.id)";
static const char TAG_JOIN_IN_STRING_SQL[] =
    "select metadata_items.id from metadata_items left join tags on tags.id=10 where tags.id=10 and 'taggings.metadata_item_id=metadata_items.id taggings.tag_id=tags.id' != ''";
static const char TAG_JOIN_IN_COMMENT_SQL[] =
    "select metadata_items.id from metadata_items left join tags on tags.id=10 /* taggings.metadata_item_id=metadata_items.id and taggings.tag_id=tags.id */ where tags.id=10";
static const char TAG_DUPLICATE_ID_SQL[] =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where tags.id=10 and tags.id=11";
static const char TAG_BOUND_ID_SQL[] =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where tags.id=?";
static const char TAG_NAMED_ID_SQL[] =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where tags.id=:tag_id";
static const char TAG_STRING_ID_SQL[] =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where tags.id='10'";
static const char TAG_METADATA_FTS_SQL[] =
    "select metadata_items.id from metadata_items join fts4_metadata_titles_icu on metadata_items.id=fts4_metadata_titles_icu.rowid left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where fts4_metadata_titles_icu.title match 'Django*' and tags.id=10";
static const char TAG_FLIPPED_JOIN_SQL[] =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on tags.id=taggings.tag_id where metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 and tags.id=10 order by metadata_items.id";
static const char TAG_FLIPPED_JOIN_SQL_REWRITTEN[] =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on tags.id=taggings.tag_id where metadata_items.library_section_id in (1) and metadata_items.metadata_type=1 and tags.id=10" TAG_MEMBERSHIP_10 " order by metadata_items.id";
static const char TAG_OR_ID_SQL[] =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where metadata_items.library_section_id in (1) and (metadata_items.metadata_type=1 or tags.id=10)";
static const char TAG_NOT_ID_SQL[] =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where metadata_items.library_section_id in (1) and not (tags.id=10)";
static const char TAG_VALUE_COMPARED_ID_SQL[] =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on taggings.tag_id=tags.id where metadata_items.library_section_id in (1) and (tags.id=10)=1";
static const char TAG_COLUMN_RHS_ONLY_SQL[] =
    "select metadata_items.id from metadata_items left join taggings on taggings.metadata_item_id=metadata_items.id left join tags on tags.id=taggings.tag_id where metadata_items.library_section_id in (1) and metadata_items.metadata_type=1";

#define ONDECK_HEAD \
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid and metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id="
#define ONDECK_AFTER_SECTION " and grandparents.id in ("
#define ONDECK_AFTER_THRESHOLD " and viewed_at > "
#define ONDECK_AFTER_IDS " and metadata_item_settings.view_count>0  and metadata_item_views.account_id="
#define ONDECK_AFTER_ACCOUNT " group by grandparents.id order by viewed_at desc"
#define ONDECK_IDS "101,101,102"
#define ONDECK_THRESHOLD "1500"
#define ONDECK_SQL_BODY ONDECK_HEAD "2" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT
#define ONDECK_THRESHOLD_SQL_BODY ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD ONDECK_THRESHOLD ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT

static const char ONDECK_SQL[] = ONDECK_SQL_BODY;
static const char ONDECK_PER_GUID_SQL[] =
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid and metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=? and true and metadata_item_settings.view_count>0  and metadata_item_views.account_id=? and grandparents.guid='plex://show/648dd4c8004a8e8652751de2'  group by grandparents.id order by viewed_at desc";
static const char ONDECK_SQL_REWRITTEN[] =
    "SELECT grandparents_id AS id,\n"
    "       originally_available_at AS originally_available_at,\n"
    "       parent_index AS parent_index,\n"
    "       metadata_item_views_index AS \"index\",\n"
    "       +viewed_at AS \"max(viewed_at)\",\n"
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
    "  JOIN metadata_item_views\n"
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
static const char ONDECK_THRESHOLD_SQL[] = ONDECK_THRESHOLD_SQL_BODY;
static const char ONDECK_THRESHOLD_SQL_REWRITTEN[] =
    "SELECT grandparents_id AS id,\n"
    "       originally_available_at AS originally_available_at,\n"
    "       parent_index AS parent_index,\n"
    "       metadata_item_views_index AS \"index\",\n"
    "       +viewed_at AS \"max(viewed_at)\",\n"
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
    "  JOIN metadata_item_views\n"
    "  JOIN metadata_item_settings\n"
    "  JOIN metadata_item_settings AS grandparentsSettings\n"
    "  WHERE grandparents.guid=metadata_item_views.grandparent_guid\n"
    "    AND metadata_item_settings.guid=metadata_item_views.guid\n"
    "    AND metadata_item_views.account_id=metadata_item_settings.account_id\n"
    "    AND grandparentsSettings.guid=metadata_item_views.grandparent_guid\n"
    "    AND metadata_item_views.account_id=grandparentsSettings.account_id\n"
    "    AND metadata_item_views.library_section_id=2\n"
    "    AND metadata_item_views.viewed_at > " ONDECK_THRESHOLD "\n"
    "    AND metadata_item_settings.view_count>0\n"
    "    AND metadata_item_views.account_id=42\n"
    ") AS dshadow_on_deck_ranked\n"
    "WHERE dshadow_on_deck_rank=1\n"
    "ORDER BY viewed_at DESC, grandparents_id DESC;";
static const char ONDECK_THRESHOLD_PARAM_SECTION_SQL[] =
    ONDECK_HEAD "?" ONDECK_AFTER_THRESHOLD ONDECK_THRESHOLD ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_THRESHOLD_PARAM_ACCOUNT_SQL[] =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD ONDECK_THRESHOLD ONDECK_AFTER_IDS "?" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_THRESHOLD_PARAM_BOTH_SQL[] =
    ONDECK_HEAD "?" ONDECK_AFTER_THRESHOLD ONDECK_THRESHOLD ONDECK_AFTER_IDS "?" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_THRESHOLD_CROSS_PRODUCT_SQL[] =
    ONDECK_HEAD "2" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_THRESHOLD ONDECK_THRESHOLD ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_THRESHOLD_BIND_SQL[] =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD "?" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_THRESHOLD_NAMED_SQL[] =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD ":threshold" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_THRESHOLD_POSITIVE_SIGN_SQL[] =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD "+1500" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_THRESHOLD_NEGATIVE_SIGN_SQL[] =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD "-1500" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_THRESHOLD_DECIMAL_SQL[] =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD "1500.0" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_THRESHOLD_EXPRESSION_SQL[] =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD "1500+0" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_THRESHOLD_OVERFLOW_SQL[] =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD "9223372036854775808" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_NO_SELECTOR_SQL[] =
    ONDECK_HEAD "2" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_THRESHOLD_WRONG_TAIL_SQL[] =
    ONDECK_HEAD "2" ONDECK_AFTER_THRESHOLD ONDECK_THRESHOLD ONDECK_AFTER_IDS "42 group by grandparents.id order by grandparents.id";
static const char ONDECK_LEFT_JOIN_SQL[] =
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid left join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid and metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=2 and grandparents.id in (" ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_PARAM_LIST_SQL[] =
    ONDECK_HEAD "2" ONDECK_AFTER_SECTION "101,?" ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_PARAM_ACCOUNT_SQL[] =
    ONDECK_HEAD "2" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "?" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_PARAM_BOTH_SQL[] =
    ONDECK_HEAD "?" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "?" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_PARAM_REVERSED_SQL[] =
    ONDECK_HEAD "?2" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "?1" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_PARAM_HOLE_SQL[] =
    ONDECK_HEAD "?2" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "?" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_PARAM_REUSE_FORWARD_SQL[] =
    ONDECK_HEAD "?1" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "?1" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_PARAM_REUSE_REVERSE_SQL[] =
    ONDECK_HEAD "?" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "?1" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_PARAM_LEADING_ZERO_SQL[] =
    ONDECK_HEAD "?01" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_PARAM_NAMED_SQL[] =
    ONDECK_HEAD ":section" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_DUP_ACCOUNT_SQL[] =
    ONDECK_HEAD "2" ONDECK_AFTER_SECTION ONDECK_IDS ")" ONDECK_AFTER_IDS "42 and metadata_item_views.account_id=43" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_MISSING_VIEWCOUNT_SQL[] =
    ONDECK_HEAD "2" ONDECK_AFTER_SECTION ONDECK_IDS ")" "  and metadata_item_views.account_id=42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_MISSING_SETTINGS_GUID_SQL[] =
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=2 and grandparents.id in (" ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_MISSING_SETTINGS_ACCOUNT_SQL[] =
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=2 and grandparents.id in (" ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;
static const char ONDECK_PROJECTION_DRIFT_SQL[] =
    "select grandparents.id,metadata_item_views.originally_available_at,metadata_item_views.parent_index,metadata_item_views.`index`,max(metadata_item_views.viewed_at),grandparents.library_section_id,grandparentsSettings.extra_data from metadata_item_views indexed by index_metadata_item_views_on_guid join metadata_items as grandparents indexed by index_metadata_items_on_guid on grandparents.guid=grandparent_guid join metadata_item_settings indexed by index_metadata_item_settings_on_account_id on metadata_item_settings.guid=metadata_item_views.guid and metadata_item_views.account_id=metadata_item_settings.account_id join metadata_item_settings as grandparentsSettings indexed by index_metadata_item_settings_on_guid on grandparentsSettings.guid=metadata_item_views.grandparent_guid and metadata_item_views.account_id=grandparentsSettings.account_id where metadata_item_views.library_section_id=2 and grandparents.id in (" ONDECK_IDS ")" ONDECK_AFTER_IDS "42" ONDECK_AFTER_ACCOUNT;

typedef struct auth_probe {
    int unlikely_calls;
    int deny_unlikely;
} auth_probe;

typedef struct ondeck_failure_probe {
    int deny_row_number;
    int deny_sqlite_master;
    int denied_calls;
} ondeck_failure_probe;

static const rsh_suite_spec plex_suite_spec;

static void failf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

#include "contract_parity.h"

static int rsh_public_prepare(
    sqlite3 *db,
    const char *sql,
    int nbyte,
    rsh_prepare_kind kind,
    sqlite3_stmt **stmt_out,
    const char **tail_out
) {
    if (kind == RSH_PREPARE_V3) {
        return sqlite3_prepare_v3(
            db, sql, nbyte, SQLITE_PREPARE_PERSISTENT, stmt_out, tail_out
        );
    }
    if (kind == RSH_PREPARE_V2) {
        return sqlite3_prepare_v2(db, sql, nbyte, stmt_out, tail_out);
    }
    return sqlite3_prepare(db, sql, nbyte, stmt_out, tail_out);
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

static void require_same_line(
    const char *label,
    const char *text,
    const char *first,
    const char *second
) {
    const char *line = text;

    if (!text) {
        failf("FAIL [%s]: text=(null)", label);
    }
    while (*line) {
        const char *end = strchr(line, '\n');
        const char *first_match = strstr(line, first);
        const char *second_match = strstr(line, second);
        if (first_match && (!end || first_match < end) &&
            second_match && (!end || second_match < end)) {
            return;
        }
        if (!end) break;
        line = end + 1;
    }
    failf("FAIL [%s]: no line contains \"%s\" and \"%s\" in \"%s\"",
          label, first, second, text ? text : "(null)");
}

static void require_no_same_line(
    const char *label,
    const char *text,
    const char *first,
    const char *second
) {
    const char *line = text;

    if (!text) {
        failf("FAIL [%s]: text=(null)", label);
    }
    while (*line) {
        const char *end = strchr(line, '\n');
        const char *first_match = strstr(line, first);
        const char *second_match = strstr(line, second);
        if (first_match && (!end || first_match < end) &&
            second_match && (!end || second_match < end)) {
            failf("FAIL [%s]: line contains \"%s\" and unexpected \"%s\" in \"%s\"",
                  label, first, second, text);
        }
        if (!end) break;
        line = end + 1;
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

static uint64_t sql_corr_key(const char *sql, size_t len) {
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t i;

    for (i = 0; i < len; i++) {
        hash ^= (unsigned char)sql[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void require_occurrences(const char *label, const char *got, const char *needle, int want) {
    int got_count = got ? count_occurrences(got, needle) : 0;
    if (got_count != want) {
        failf("FAIL [%s]: occurrences=%d want=%d needle=\"%s\" text=\"%s\"",
              label, got_count, want, needle, got ? got : "(null)");
    }
}


static uint64_t require_shape_field_on_line(
    const char *label,
    const char *text,
    const char *first,
    const char *second
) {
    const char *line = text;

    if (!text) failf("FAIL [%s]: text=(null)", label);
    while (*line) {
        const char *end = strchr(line, '\n');
        const char *first_match = strstr(line, first);
        const char *second_match = strstr(line, second);
        const char *field = strstr(line, "shape=");
        if (first_match && (!end || first_match < end) &&
            second_match && (!end || second_match < end) &&
            field && (!end || field < end)) {
            const char *start = field + strlen("shape=");
            char *value_end = NULL;
            unsigned long long value;

            errno = 0;
            value = strtoull(start, &value_end, 16);
            if (errno != 0 || value_end != start + 16 ||
                *value_end != ' ' || value == 0u ||
                (end && value_end >= end)) {
                failf("FAIL [%s]: invalid shape field in line \"%.*s\"",
                      label, end ? (int)(end - line) : (int)strlen(line), line);
            }
            return (uint64_t)value;
        }
        if (!end) break;
        line = end + 1;
    }
    failf("FAIL [%s]: no line contains \"%s\", \"%s\", and shape",
          label, first, second);
    return 0;
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
        "(1004,'plex://episode/4','plex://grandparent/102',42,2,104,4,4,1500),"
        "(1005,'plex://episode/5','plex://grandparent/102',2,2,105,5,5,1400);"
        "INSERT INTO metadata_item_settings(id,account_id,guid,view_count,extra_data) VALUES"
        "(201,42,'plex://episode/1',1,'episode-1'),"
        "(202,42,'plex://episode/2',1,'episode-2'),"
        "(203,42,'plex://episode/3',1,'episode-3'),"
        "(204,42,'plex://episode/4',1,'episode-4'),"
        "(205,2,'plex://episode/5',1,'episode-5'),"
        "(301,42,'plex://grandparent/101',1,'gp101-a'),"
        "(302,42,'plex://grandparent/101',1,'gp101-b'),"
        "(303,42,'plex://grandparent/102',1,'gp102'),"
        "(304,2,'plex://grandparent/102',1,'gp102-account-2');"
    );
}

static int rsh_open(const char *path, sqlite3 **db_out, void *suite_ctx) {
    (void)suite_ctx;
    *db_out = open_db(path);
    return SQLITE_OK;
}

static int rsh_base_seed(sqlite3 *db, void *suite_ctx) {
    (void)suite_ctx;
    setup_schema(db);
    return SQLITE_OK;
}

enum {
    PLEX_PROFILE_TAG_MEMBERSHIP_INDEX = 1,
    PLEX_PROFILE_ONDECK_INDEX,
    PLEX_PROFILE_TAG_ROW_PARITY
};

static int rsh_apply_plex_profile(
    sqlite3 *db,
    int profile,
    void *suite_ctx
);

static rsh_apply_profile_fn rsh_resolve_setup_profile(
    int profile,
    void *suite_ctx
) {
    (void)suite_ctx;
    if (profile == PLEX_PROFILE_TAG_MEMBERSHIP_INDEX ||
        profile == PLEX_PROFILE_ONDECK_INDEX ||
        profile == PLEX_PROFILE_TAG_ROW_PARITY) {
        return rsh_apply_plex_profile;
    }
    return NULL;
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

static int rsh_apply_plex_profile(
    sqlite3 *db,
    int profile,
    void *suite_ctx
) {
    (void)suite_ctx;
    if (profile == PLEX_PROFILE_TAG_MEMBERSHIP_INDEX) {
        create_tag_membership_index(db);
    } else if (profile == PLEX_PROFILE_ONDECK_INDEX) {
        create_ondeck_index(db);
    } else if (profile == PLEX_PROFILE_TAG_ROW_PARITY) {
        create_tag_membership_index(db);
        exec_sql(
            db, "tag-row-parity-extra",
            "INSERT INTO taggings(id,metadata_item_id,tag_id,`index`) "
            "VALUES(7,2,10,7);"
        );
    } else {
        return SQLITE_MISUSE;
    }
    return SQLITE_OK;
}

typedef struct ondeck_bind_values {
    int count;
    int values[3];
} ondeck_bind_values;

typedef struct guid_like_bind_values {
    const char *pattern;
    int limit;
} guid_like_bind_values;

typedef struct ondeck_contract {
    int expected_rows;
    unsigned int expected_mask;
    int account_2;
} ondeck_contract;

typedef struct plex_fts_tie_exception {
    unsigned int accepted_rows;
} plex_fts_tie_exception;

typedef struct digest_result {
    int rows;
    uint64_t hash;
} digest_result;

/* MATCH_SQL_INT leaves equal count(*) rows unordered. Accept only the complete
 * two-row permutation produced by the unchanged {10,11} result set. */
static int accept_plex_fts_count_tie(
    const char *label,
    int row,
    sqlite3_stmt *vendor,
    sqlite3_stmt *candidate,
    void *ctx
) {
    plex_fts_tie_exception *tie = (plex_fts_tie_exception *)ctx;
    sqlite3_int64 vendor_id;
    sqlite3_int64 candidate_id;
    unsigned int bit;

    (void)label;
    if (row < 0 || row > 1 || sqlite3_column_type(vendor, 0) != SQLITE_INTEGER ||
        sqlite3_column_type(candidate, 0) != SQLITE_INTEGER) {
        return 0;
    }
    vendor_id = sqlite3_column_int64(vendor, 0);
    candidate_id = sqlite3_column_int64(candidate, 0);
    if (!((vendor_id == 10 && candidate_id == 11) ||
          (vendor_id == 11 && candidate_id == 10))) {
        return 0;
    }
    bit = 1u << row;
    if ((tie->accepted_rows & bit) != 0) return 0;
    tie->accepted_rows |= bit;
    return 1;
}

static void bind_guid_like_values(
    sqlite3_stmt *stmt,
    const char *side,
    const char *label,
    void *ctx
) {
    guid_like_bind_values *binds = (guid_like_bind_values *)ctx;
    int rc;

    if (binds->pattern) {
        rc = sqlite3_bind_text(stmt, 1, binds->pattern, -1, SQLITE_STATIC);
    } else {
        rc = sqlite3_bind_null(stmt, 1);
    }
    if (rc != SQLITE_OK) {
        failf("FAIL [%s/%s-bind-pattern]: rc=%d", label, side, rc);
    }
    rc = sqlite3_bind_int(stmt, 2, binds->limit);
    if (rc != SQLITE_OK) {
        failf("FAIL [%s/%s-bind-limit]: rc=%d", label, side, rc);
    }
}

static int accept_guid_like_legal_difference(
    const char *label,
    int row,
    sqlite3_stmt *vendor,
    sqlite3_stmt *candidate,
    void *ctx
) {
    sqlite3_int64 vendor_id;
    sqlite3_int64 candidate_id;

    (void)label;
    (void)row;
    (void)ctx;
    if (sqlite3_column_type(vendor, 0) != SQLITE_INTEGER ||
        sqlite3_column_type(candidate, 0) != SQLITE_INTEGER) {
        return 0;
    }
    vendor_id = sqlite3_column_int64(vendor, 0);
    candidate_id = sqlite3_column_int64(candidate, 0);
    return vendor_id >= 1 && vendor_id <= 6 &&
        candidate_id >= 1 && candidate_id <= 6;
}

static void bind_ondeck_values(
    sqlite3_stmt *stmt,
    const char *side,
    const char *label,
    void *ctx
) {
    ondeck_bind_values *binds = (ondeck_bind_values *)ctx;
    int i;
    for (i = 0; i < binds->count; i++) {
        int rc = sqlite3_bind_int(stmt, i + 1, binds->values[i]);
        if (rc != SQLITE_OK) {
            failf("FAIL [%s/%s-bind-%d]: rc=%d", label, side, i + 1, rc);
        }
    }
}

static void require_ondeck_integer_cell(
    const char *label,
    const char *side,
    sqlite3_stmt *stmt,
    int column,
    sqlite3_int64 want
) {
    int type = sqlite3_column_type(stmt, column);
    sqlite3_int64 got = sqlite3_column_int64(stmt, column);
    if (type != SQLITE_INTEGER || got != want) {
        failf("FAIL [%s/tied-%s-column-%d]: type=%d value=%lld want_type=%d want=%lld",
              label, side, column, type, (long long)got, SQLITE_INTEGER,
              (long long)want);
    }
}

static void require_ondeck_text_cell(
    const char *label,
    const char *side,
    sqlite3_stmt *stmt,
    int column,
    const char *want
) {
    int type = sqlite3_column_type(stmt, column);
    const unsigned char *got = sqlite3_column_text(stmt, column);
    if (type != SQLITE_TEXT || !got || strcmp((const char *)got, want) != 0) {
        failf("FAIL [%s/tied-%s-column-%d]: type=%d value=%s want_type=%d want=%s",
              label, side, column, type, got ? (const char *)got : "(null)",
              SQLITE_TEXT, want);
    }
}

static void require_ondeck_candidate_row(
    const char *label,
    int row,
    sqlite3_stmt *stmt,
    const ondeck_contract *contract
) {
    if (contract->account_2) {
        if (row != 0) {
            failf("FAIL [%s/candidate-row]: row=%d want=0", label, row);
        }
        require_ondeck_integer_cell(label, "candidate", stmt, 0, 102);
        require_ondeck_integer_cell(label, "candidate", stmt, 1, 105);
        require_ondeck_integer_cell(label, "candidate", stmt, 2, 5);
        require_ondeck_integer_cell(label, "candidate", stmt, 3, 5);
        require_ondeck_integer_cell(label, "candidate", stmt, 4, 1400);
        require_ondeck_integer_cell(label, "candidate", stmt, 5, 2);
        require_ondeck_text_cell(
            label, "candidate", stmt, 6, "gp102-account-2"
        );
        return;
    }
    if (row == 0) {
        require_ondeck_integer_cell(label, "candidate", stmt, 0, 101);
        require_ondeck_integer_cell(label, "candidate", stmt, 1, 103);
        require_ondeck_integer_cell(label, "candidate", stmt, 2, 3);
        require_ondeck_integer_cell(label, "candidate", stmt, 3, 3);
        require_ondeck_integer_cell(label, "candidate", stmt, 4, 2000);
        require_ondeck_integer_cell(label, "candidate", stmt, 5, 2);
        require_ondeck_text_cell(label, "candidate", stmt, 6, "gp101-b");
        return;
    }
    if (row == 1) {
        require_ondeck_integer_cell(label, "candidate", stmt, 0, 102);
        require_ondeck_integer_cell(label, "candidate", stmt, 1, 104);
        require_ondeck_integer_cell(label, "candidate", stmt, 2, 4);
        require_ondeck_integer_cell(label, "candidate", stmt, 3, 4);
        require_ondeck_integer_cell(label, "candidate", stmt, 4, 1500);
        require_ondeck_integer_cell(label, "candidate", stmt, 5, 2);
        require_ondeck_text_cell(label, "candidate", stmt, 6, "gp102");
        return;
    }
    failf("FAIL [%s/candidate-row]: row=%d want=0|1", label, row);
}

static unsigned int require_ondeck_vendor_row(
    const char *label,
    int row,
    sqlite3_stmt *stmt,
    const ondeck_contract *contract
) {
    sqlite3_int64 id = sqlite3_column_int64(stmt, 0);

    if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER) {
        failf("FAIL [%s/vendor-id]: type=%d want=%d", label,
              sqlite3_column_type(stmt, 0), SQLITE_INTEGER);
    }
    if (!contract->account_2 && contract->expected_rows == 2 &&
        id != (row == 0 ? 101 : 102)) {
        failf("FAIL [%s/vendor-order]: row=%d got=%lld want=%d", label, row,
              (long long)id, row == 0 ? 101 : 102);
    }
    if (id == 101) {
        sqlite3_int64 originally_available_at = sqlite3_column_int64(stmt, 1);
        sqlite3_int64 parent_index = sqlite3_column_int64(stmt, 2);
        sqlite3_int64 index = sqlite3_column_int64(stmt, 3);
        const unsigned char *extra_data = sqlite3_column_text(stmt, 6);
        int episode_is_2 = originally_available_at == 102 &&
            parent_index == 2 && index == 2;
        int episode_is_3 = originally_available_at == 103 &&
            parent_index == 3 && index == 3;
        int extra_is_legal = sqlite3_column_type(stmt, 6) == SQLITE_TEXT &&
            extra_data &&
            (strcmp((const char *)extra_data, "gp101-a") == 0 ||
             strcmp((const char *)extra_data, "gp101-b") == 0);

        if (sqlite3_column_type(stmt, 1) != SQLITE_INTEGER ||
            sqlite3_column_type(stmt, 2) != SQLITE_INTEGER ||
            sqlite3_column_type(stmt, 3) != SQLITE_INTEGER ||
            (!episode_is_2 && !episode_is_3) || !extra_is_legal) {
            failf("FAIL [%s/vendor-101-representative]: oa=%lld parent=%lld "
                  "index=%lld extra=%s", label,
                  (long long)originally_available_at, (long long)parent_index,
                  (long long)index,
                  extra_data ? (const char *)extra_data : "(null)");
        }
        require_ondeck_integer_cell(label, "vendor", stmt, 4, 2000);
        require_ondeck_integer_cell(label, "vendor", stmt, 5, 2);
        return 1u;
    }
    if (id == 102) {
        require_ondeck_integer_cell(
            label, "vendor", stmt, 1, contract->account_2 ? 105 : 104
        );
        require_ondeck_integer_cell(
            label, "vendor", stmt, 2, contract->account_2 ? 5 : 4
        );
        require_ondeck_integer_cell(
            label, "vendor", stmt, 3, contract->account_2 ? 5 : 4
        );
        require_ondeck_integer_cell(
            label, "vendor", stmt, 4, contract->account_2 ? 1400 : 1500
        );
        require_ondeck_integer_cell(label, "vendor", stmt, 5, 2);
        require_ondeck_text_cell(
            label, "vendor", stmt, 6,
            contract->account_2 ? "gp102-account-2" : "gp102"
        );
        return 2u;
    }
    failf("FAIL [%s/vendor-id]: got=%lld want=101|102", label, (long long)id);
    return 0;
}

static int accept_ondeck_legal_difference(
    const char *label,
    int row,
    sqlite3_stmt *vendor,
    sqlite3_stmt *candidate,
    void *ctx
) {
    ondeck_contract *contract = (ondeck_contract *)ctx;

    if (row < 0 || row >= contract->expected_rows) return 0;
    (void)require_ondeck_vendor_row(label, row, vendor, contract);
    require_ondeck_candidate_row(label, row, candidate, contract);
    return 1;
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

static int ondeck_failure_authorizer_cb(
    void *ctx,
    int action,
    const char *p1,
    const char *p2,
    const char *db,
    const char *trigger
) {
    ondeck_failure_probe *probe = (ondeck_failure_probe *)ctx;
    (void)db;
    (void)trigger;
    if (probe->deny_row_number && action == SQLITE_FUNCTION &&
        ((p1 && strcmp(p1, "row_number") == 0) ||
         (p2 && strcmp(p2, "row_number") == 0))) {
        probe->denied_calls++;
        return SQLITE_DENY;
    }
    if (probe->deny_sqlite_master && action == SQLITE_READ && p1 &&
        (strcmp(p1, "sqlite_master") == 0 || strcmp(p1, "sqlite_schema") == 0)) {
        probe->denied_calls++;
        return SQLITE_DENY;
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

static void expect_ondeck_authorizer_fallback(
    sqlite3 *db,
    const char *label,
    int deny_row_number,
    int deny_sqlite_master
) {
    ondeck_failure_probe probe = {deny_row_number, deny_sqlite_master, 0};

    require_int(label,
                sqlite3_set_authorizer(db, ondeck_failure_authorizer_cb, &probe),
                SQLITE_OK);
    expect_saved_sql(db, label, ONDECK_THRESHOLD_SQL, -1, 2, ONDECK_THRESHOLD_SQL);
    require_int(label, sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    if (probe.denied_calls == 0) {
        failf("FAIL [%s]: denied_calls=0 want=>0", label);
    }
}

static void prepare_step_original(
    sqlite3 *db,
    const char *label,
    const char *sql
) {
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int rc;

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, &tail);
    if (rc == SQLITE_OK) {
        int step_rc;
        while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        }
        if (step_rc != SQLITE_DONE) rc = step_rc;
    }
    if (rc != SQLITE_OK) {
        failf("FAIL [%s/prepare]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
    require_str_eq(label, sqlite3_sql(stmt), sql);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s/tail]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static void prepare_original_without_step(
    sqlite3 *db,
    const char *label,
    const char *sql
) {
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, &tail);

    if (rc != SQLITE_OK) {
        failf("FAIL [%s/prepare]: rc=%d err=%s", label, rc, sqlite3_errmsg(db));
    }
    require_str_eq(label, sqlite3_sql(stmt), sql);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s/tail]: tail_offset=%ld want=%ld",
              label, (long)(tail - sql), (long)strlen(sql));
    }
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
}

static char *copy_ondeck_per_guid_sql(const char *guid) {
    static const char guid_prefix[] = "plex://show/";
    size_t sql_len = strlen(ONDECK_PER_GUID_SQL);
    char *sql;
    char *slot;

    if (!guid || strlen(guid) != 24u) {
        failf("FATAL: per-GUID value must contain exactly 24 bytes");
    }
    sql = (char *)malloc(sql_len + 1u);
    if (!sql) failf("FATAL: per-GUID SQL allocation failed");
    memcpy(sql, ONDECK_PER_GUID_SQL, sql_len + 1u);
    slot = strstr(sql, guid_prefix);
    if (!slot) failf("FATAL: per-GUID prefix missing");
    slot += sizeof(guid_prefix) - 1u;
    if (slot[24] != '\'') failf("FATAL: per-GUID slot width drifted");
    memcpy(slot, guid, 24u);
    return sql;
}

static char *make_ondeck_per_guid_near_miss(void) {
    static const char tail[] = "order by viewed_at desc";
    char *sql = copy_ondeck_per_guid_sql("648dd4c8004a8e8652751de2");
    char *found = strstr(sql, tail);

    if (!found) failf("FATAL: per-GUID tail missing");
    memcpy(found + strlen("order by viewed_at "), "asc ", 4u);
    return sql;
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


static const char *const plex_controlled_env_gates[] = {
    "SQLITE3_DISABLE_AUTOPRAGMA",
    "SQLITE3_DISABLE_RUNTIME_OPTIMIZE",
    "SQLITE3_DISABLE_OBSERVABILITY",
    "SQLITE3_DISABLE_STMT_TRACE",
    "SQLITE3_DISABLE_STMT_TRACE_SAMPLING",
    "SQLITE3_DISABLE_REWRITE_APPLIED_SQL",
    "SQLITE3_DISABLE_PLEX_FTS_REWRITE",
    "SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE",
    "SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE",
    "SQLITE3_DISABLE_PLEX_ONDECK_REWRITE"
};

#define PLEX_ENV_UNSET(gate_name) \
    {.name = (gate_name), .value = {.state = RSH_ENV_UNSET}}
#define PLEX_ENV_SET(gate_name, gate_value) \
    {.name = (gate_name), \
     .value = {.state = RSH_ENV_VALUE, .value = (gate_value)}}
#define PLEX_NATIVE_ENV_COMMON \
    PLEX_ENV_SET("SQLITE3_DISABLE_AUTOPRAGMA", "1"), \
    PLEX_ENV_SET("SQLITE3_DISABLE_RUNTIME_OPTIMIZE", "1"), \
    PLEX_ENV_SET("SQLITE3_DISABLE_OBSERVABILITY", "1"), \
    PLEX_ENV_UNSET("SQLITE3_DISABLE_STMT_TRACE"), \
    PLEX_ENV_UNSET("SQLITE3_DISABLE_STMT_TRACE_SAMPLING"), \
    PLEX_ENV_UNSET("SQLITE3_DISABLE_REWRITE_APPLIED_SQL")

static const rsh_env_assignment plex_fts_default_env[] = {
    PLEX_NATIVE_ENV_COMMON,
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_FTS_REWRITE"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "1"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE")
};

static const rsh_env_assignment plex_fts_enabled_env[] = {
    PLEX_NATIVE_ENV_COMMON,
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "0"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "1"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE")
};

static const rsh_env_assignment plex_fts_disabled_env[] = {
    PLEX_NATIVE_ENV_COMMON,
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "1"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE")
};

static const rsh_env_assignment plex_fts_garbage_env[] = {
    PLEX_NATIVE_ENV_COMMON,
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "false"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "1"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE")
};

static const rsh_env_assignment plex_all_rewrites_disabled_env[] = {
    PLEX_NATIVE_ENV_COMMON,
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "1"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", "1")
};

static const rsh_env_assignment plex_guid_like_enabled_env[] = {
    PLEX_NATIVE_ENV_COMMON,
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE", "0"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "1"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", "1")
};

static const rsh_env_assignment plex_guid_like_disabled_env[] = {
    PLEX_NATIVE_ENV_COMMON,
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE", "1"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "1"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", "1")
};

static const rsh_env_assignment plex_guid_like_garbage_env[] = {
    PLEX_NATIVE_ENV_COMMON,
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE", "false"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "1"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", "1")
};

static const rsh_env_assignment plex_tag_default_env[] = {
    PLEX_NATIVE_ENV_COMMON,
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", "1")
};

static const rsh_env_assignment plex_tag_enabled_env[] = {
    PLEX_NATIVE_ENV_COMMON,
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "0"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", "1")
};

static const rsh_env_assignment plex_tag_garbage_env[] = {
    PLEX_NATIVE_ENV_COMMON,
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "false"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", "1")
};

static const rsh_env_assignment plex_ondeck_default_env[] = {
    PLEX_NATIVE_ENV_COMMON,
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "1"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE")
};

static const rsh_env_assignment plex_ondeck_enabled_env[] = {
    PLEX_NATIVE_ENV_COMMON,
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "1"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", "0")
};

static const rsh_env_assignment plex_ondeck_garbage_env[] = {
    PLEX_NATIVE_ENV_COMMON,
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_FTS_REWRITE", "1"),
    PLEX_ENV_UNSET("SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE", "1"),
    PLEX_ENV_SET("SQLITE3_DISABLE_PLEX_ONDECK_REWRITE", "false")
};

#undef PLEX_NATIVE_ENV_COMMON
#undef PLEX_ENV_SET
#undef PLEX_ENV_UNSET

static const rsh_env_assignment plex_exec_fts_env[] = {
    {
        .name = "SQLITE3_DISABLE_AUTOPRAGMA",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_RUNTIME_OPTIMIZE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE_SAMPLING",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_REWRITE_APPLIED_SQL",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_OBSERVABILITY",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_FTS_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "0"}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_ONDECK_REWRITE",
        .value = {.state = RSH_ENV_UNSET}
    }
};

static const rsh_env_assignment plex_exec_guid_like_env[] = {
    {
        .name = "SQLITE3_DISABLE_AUTOPRAGMA",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_RUNTIME_OPTIMIZE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE_SAMPLING",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_OBSERVABILITY",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_REWRITE_APPLIED_SQL",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_FTS_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "0"}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_ONDECK_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    }
};

static const rsh_env_assignment plex_exec_tag_visible_env[] = {
    {
        .name = "SQLITE3_DISABLE_AUTOPRAGMA",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_RUNTIME_OPTIMIZE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE_SAMPLING",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_OBSERVABILITY",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_FTS_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "0"}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_ONDECK_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_REWRITE_APPLIED_SQL",
        .value = {.state = RSH_ENV_UNSET}
    }
};

static const rsh_env_assignment plex_exec_tag_suppressed_env[] = {
    {
        .name = "SQLITE3_DISABLE_AUTOPRAGMA",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_RUNTIME_OPTIMIZE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE_SAMPLING",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_OBSERVABILITY",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_FTS_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "0"}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_ONDECK_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_REWRITE_APPLIED_SQL",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    }
};

static const rsh_env_assignment plex_exec_ondeck_env[] = {
    {
        .name = "SQLITE3_DISABLE_AUTOPRAGMA",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_RUNTIME_OPTIMIZE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE",
        .value = {.state = RSH_ENV_VALUE, .value = "0"}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE_SAMPLING",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_OBSERVABILITY",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_FTS_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_ONDECK_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "0"}
    },
    {
        .name = "SQLITE3_DISABLE_REWRITE_APPLIED_SQL",
        .value = {.state = RSH_ENV_UNSET}
    }
};

static int rsh_case_icu_available(
    const rsh_case_spec *test_case,
    const rsh_case_context *context,
    void *suite_ctx
) {
    (void)test_case;
    (void)context;
    (void)suite_ctx;
    return sqlite3_compileoption_used("ENABLE_ICU") != 0;
}

static int rsh_case_icu_unavailable(
    const rsh_case_spec *test_case,
    const rsh_case_context *context,
    void *suite_ctx
) {
    return !rsh_case_icu_available(test_case, context, suite_ctx);
}

static sqlite3 *rsh_context_db(
    const rsh_case_context *context,
    const char *role
) {
    size_t i;

    for (i = 0; i < context->db_count; i++) {
        if (context->dbs[i].spec &&
            strcmp(context->dbs[i].spec->role, role) == 0) {
            return context->dbs[i].db;
        }
    }
    failf("FAIL [%s/%s/role]: missing=%s", context->run_name,
          context->phase_label, role);
    return NULL;
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

static int rsh_custom_adapter_grouped_digest_identity(
    const rsh_case_context *context,
    const void *immutable_data
) {
    static const char *original =
        "select tags.id, count(*) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match ? and tag_type=? and metadata_items.library_section_id in (?) and metadata_items.metadata_type=? group by tags.id order by tags.id, count(*)";
    static const char *rewritten =
        "select tags.id, count(*) from metadata_items join taggings on taggings.metadata_item_id=metadata_items.id join tags on tags.id=taggings.tag_id join fts4_tag_titles_icu on fts4_tag_titles_icu.rowid=tags.id where fts4_tag_titles_icu.tag match ? and unlikely(tag_type=?) and metadata_items.library_section_id in (?) and metadata_items.metadata_type=? group by tags.id order by tags.id, count(*)";
    sqlite3 *db = rsh_context_db(context, (const char *)immutable_data);
    int mode;

    for (mode = 0; mode < 3; mode++) {
        digest_result a = digest_grouped(db, original, mode);
        digest_result b = digest_grouped(db, rewritten, mode);
        if (a.rows != b.rows || a.hash != b.hash) {
            failf("FAIL [grouped-digest mode=%d]: original rows=%d hash=%llu rewritten rows=%d hash=%llu",
                  mode, a.rows, (unsigned long long)a.hash, b.rows, (unsigned long long)b.hash);
        }
    }
    return SQLITE_OK;
}

static int rsh_custom_adapter_plex_fts_tied_order(
    const rsh_case_context *context,
    const void *immutable_data
) {
    const plex_fts_tie_exception *tie =
        (const plex_fts_tie_exception *)immutable_data;

    (void)context;
    if (tie->accepted_rows != 0 && tie->accepted_rows != 3) {
        failf("FAIL [plex-fts-contract/tied-order]: accepted=0x%x want=0x0|0x3",
              tie->accepted_rows);
    }
    return SQLITE_OK;
}

static void rsh_require_authorized_original(
    const rsh_case_context *context,
    const char *role,
    const char *label,
    const char *sql,
    int expected_attempts
) {
    auth_probe probe = {0, 1};
    sqlite3 *db = rsh_context_db(context, role);
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int rc;

    require_int(label, sqlite3_set_authorizer(db, authorizer_cb, &probe), SQLITE_OK);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, &tail);
    require_int(label, sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK);
    if (rc != SQLITE_OK || !stmt) {
        failf("FAIL [%s/prepare]: rc=%d stmt=%s err=%s", label, rc,
              stmt ? "non-NULL" : "NULL", sqlite3_errmsg(db));
    }
    require_str_eq(label, sqlite3_sql(stmt), sql);
    if (tail != sql + strlen(sql)) {
        failf("FAIL [%s/tail]: got=%ld want=%ld", label,
              (long)(tail - sql), (long)strlen(sql));
    }
    require_int(label, sqlite3_finalize(stmt), SQLITE_OK);
    if ((expected_attempts && probe.unlikely_calls < 1) ||
        (!expected_attempts && probe.unlikely_calls != 0)) {
        failf("FAIL [%s/attempts]: got=%d want=%s", label,
              probe.unlikely_calls, expected_attempts ? ">=1" : "0");
    }
}

static int rsh_custom_adapter_path_non_ascii(
    const rsh_case_context *context,
    const void *immutable_data
) {
    rsh_require_authorized_original(
        context, (const char *)immutable_data, "non-ascii-path",
        MATCH_SQL_INT, 0
    );
    return SQLITE_OK;
}

static int rsh_custom_adapter_nonmatch_prepare_denial(
    const rsh_case_context *context,
    const void *immutable_data
) {
    (void)immutable_data;
    rsh_require_authorized_original(
        context, "candidate", "fail-open", MATCH_SQL_INT,
        sqlite3_compileoption_used("ENABLE_ICU") != 0
    );
    return SQLITE_OK;
}

typedef struct plex_tag_log_producer_spec {
    const char *role;
    int count;
} plex_tag_log_producer_spec;

typedef struct plex_ondeck_log_miss {
    const char *label;
    const char *sql;
    const char *sub_reason;
} plex_ondeck_log_miss;

static const char *const plex_guid_like_sampling_roles[] = {
    "candidate-a",
    "candidate-b"
};

static const plex_tag_log_producer_spec plex_tag_visible_producer = {
    .role = "candidate",
    .count = 1025
};

static const plex_tag_log_producer_spec plex_tag_suppressed_producer = {
    .role = "candidate",
    .count = 1024
};

static const char *const plex_ondeck_per_guid_values[] = {
    "648dd4c8004a8e8652751de2",
    "111111111111111111111111",
    "aaaaaaaaaaaaaaaaaaaaaaaa",
    "0123456789abcdef01234567"
};

static const plex_ondeck_log_miss plex_ondeck_threshold_log_misses[] = {
    {"ondeck-threshold-cross-product-log", ONDECK_THRESHOLD_CROSS_PRODUCT_SQL, "post_id"},
    {"ondeck-threshold-bind-log", ONDECK_THRESHOLD_BIND_SQL, "threshold"},
    {"ondeck-threshold-named-log", ONDECK_THRESHOLD_NAMED_SQL, "threshold"},
    {"ondeck-threshold-positive-sign-log", ONDECK_THRESHOLD_POSITIVE_SIGN_SQL, "threshold"},
    {"ondeck-threshold-negative-sign-log", ONDECK_THRESHOLD_NEGATIVE_SIGN_SQL, "threshold"},
    {"ondeck-threshold-decimal-log", ONDECK_THRESHOLD_DECIMAL_SQL, "post_id"},
    {"ondeck-threshold-expression-log", ONDECK_THRESHOLD_EXPRESSION_SQL, "post_id"},
    {"ondeck-threshold-overflow-log", ONDECK_THRESHOLD_OVERFLOW_SQL, "threshold"},
    {"ondeck-threshold-no-selector-log", ONDECK_NO_SELECTOR_SQL, "selector"},
    {"ondeck-threshold-wrong-tail-log", ONDECK_THRESHOLD_WRONG_TAIL_SQL, "tail"}
};

static int rsh_custom_adapter_tag_row_parity(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *original_db = rsh_context_db(context, "vendor");
    sqlite3 *rewritten_db = rsh_context_db(context, "candidate");
    char original_ids[128];
    char rewritten_ids[128];
    int expect_rewrite = sqlite3_compileoption_used("ENABLE_ICU") != 0;

    (void)immutable_data;
    collect_first_int_ids(
        original_db, "tag-row-parity-original", TAG_LIMIT_SQL, -1, 0,
        original_ids, sizeof(original_ids)
    );
    collect_first_int_ids(
        rewritten_db, "tag-row-parity-rewritten", TAG_LIMIT_SQL,
        (int)strlen(TAG_LIMIT_SQL) + 1, expect_rewrite,
        rewritten_ids, sizeof(rewritten_ids)
    );
    require_str_eq("tag-row-parity/ids", rewritten_ids, original_ids);
    require_str_eq("tag-row-parity/expected", rewritten_ids, "1,2,");
    return SQLITE_OK;
}

static int rsh_custom_adapter_ondeck_threshold_fail_open(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *db = rsh_context_db(context, "candidate");
    int old_limit;
    int constrained_limit;

    (void)immutable_data;
    if (strlen(ONDECK_THRESHOLD_SQL) >= (size_t)INT_MAX) {
        failf("FATAL: On-Deck threshold SQL length exceeds int range");
    }
    constrained_limit = (int)strlen(ONDECK_THRESHOLD_SQL) + 1;
    old_limit = sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, constrained_limit);
    expect_saved_sql(
        db, "ondeck-threshold-build-fallback", ONDECK_THRESHOLD_SQL,
        -1, 2, ONDECK_THRESHOLD_SQL
    );
    sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, old_limit);
    if (sqlite3_compileoption_used("ENABLE_ICU") != 0) {
        expect_ondeck_authorizer_fallback(
            db, "ondeck-threshold-index-probe-fallback", 0, 1
        );
        expect_ondeck_authorizer_fallback(
            db, "ondeck-threshold-rewritten-prepare-fallback", 1, 0
        );
    }
    return SQLITE_OK;
}

static int rsh_custom_adapter_skip_log_producer(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *db = rsh_context_db(context, (const char *)immutable_data);
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    int old_limit;
    int limit;
    int rc;

    if (strlen(MATCH_SQL_LEAN) + 5 > (size_t)INT_MAX) {
        failf("FATAL: skip-log SQL length exceeds int range");
    }
    limit = (int)strlen(MATCH_SQL_LEAN) + 5;
    /* Plex maps the over-limit rewrite to rewritten_prepare_failed. */
    old_limit = sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, limit);
    rc = sqlite3_prepare_v2(db, MATCH_SQL_LEAN, -1, &stmt, &tail);
    sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, old_limit);
    if (rc != SQLITE_OK) {
        failf("FAIL [skip-log/prepare]: rc=%d err=%s", rc, sqlite3_errmsg(db));
    }
    require_str_eq("skip-log/sql", sqlite3_sql(stmt), MATCH_SQL_LEAN);
    if (tail != MATCH_SQL_LEAN + strlen(MATCH_SQL_LEAN)) {
        failf("FAIL [skip-log/tail]: tail_offset=%ld want=%ld",
              (long)(tail - MATCH_SQL_LEAN), (long)strlen(MATCH_SQL_LEAN));
    }
    require_int("skip-log/finalize", sqlite3_finalize(stmt), SQLITE_OK);
    return SQLITE_OK;
}

static int rsh_custom_adapter_skip_log_assert(
    const rsh_case_context *context,
    const void *immutable_data
) {
    static const char *needle =
        "event=rewrite_skipped target=plex reason=rewritten_prepare_failed mode=fts+tag_type";

    (void)immutable_data;
    require_occurrences(
        "skip-log/rewrite-skipped", context->captured_stderr, needle, 1
    );
    return SQLITE_OK;
}

static int rsh_custom_adapter_guid_like_sampling_producer(
    const rsh_case_context *context,
    const void *immutable_data
) {
    const char *const *roles = (const char *const *)immutable_data;
    size_t db_index;

    for (db_index = 0; db_index < 2; db_index++) {
        sqlite3 *db = rsh_context_db(context, roles[db_index]);
        int i;
        for (i = 0; i < 1025; i++) {
            sqlite3_stmt *stmt = NULL;
            const char *tail = NULL;
            int rc = sqlite3_prepare_v2(db, GUID_LIKE_SQL, -1, &stmt, &tail);
            if (rc != SQLITE_OK) {
                failf(
                    "FAIL [guid-like-applied/prepare]: db=%lu i=%d rc=%d err=%s",
                    (unsigned long)db_index, i, rc, sqlite3_errmsg(db)
                );
            }
            require_str_eq(
                "guid-like-applied/sql", sqlite3_sql(stmt),
                GUID_LIKE_SQL_REWRITTEN
            );
            if (tail != GUID_LIKE_SQL + strlen(GUID_LIKE_SQL)) {
                failf(
                    "FAIL [guid-like-applied/tail]: db=%lu i=%d got=%ld want=%ld",
                    (unsigned long)db_index, i,
                    (long)(tail - GUID_LIKE_SQL), (long)strlen(GUID_LIKE_SQL)
                );
            }
            require_int(
                "guid-like-applied/finalize", sqlite3_finalize(stmt), SQLITE_OK
            );
        }
    }
    return SQLITE_OK;
}

static int rsh_custom_adapter_guid_like_sampling_assert(
    const rsh_case_context *context,
    const void *immutable_data
) {
    static const char *needle =
        "event=rewrite_applied target=plex mode=guid+like-null db=";
    char source_field[512];
    char rewritten_field[512];
    const char *log_text = context->captured_stderr;
    int rc;

    (void)immutable_data;
    require_occurrences("guid-like-applied/records", log_text, needle, 4);
    require_occurrences(
        "guid-like-applied/first", log_text, "sample=first count=1", 2
    );
    require_occurrences(
        "guid-like-applied/periodic", log_text,
        "sample=periodic count=1024", 2
    );
    require_no_same_line("guid-like-applied/new", log_text, needle, "sample=new");
    require_no_same_line("guid-like-applied/count-2", log_text, needle, "count=2");
    require_no_same_line("guid-like-applied/count-3", log_text, needle, "count=3");
    require_no_same_line(
        "guid-like-applied/count-1025", log_text, needle, "count=1025"
    );
    rc = snprintf(
        source_field, sizeof(source_field), "source_sql=\"%s\"", GUID_LIKE_SQL
    );
    if (rc < 0 || (size_t)rc >= sizeof(source_field)) {
        failf("FATAL: guid-like source field overflow");
    }
    rc = snprintf(
        rewritten_field, sizeof(rewritten_field),
        "sql=\"%s\"", GUID_LIKE_SQL_REWRITTEN
    );
    if (rc < 0 || (size_t)rc >= sizeof(rewritten_field)) {
        failf("FATAL: guid-like rewritten field overflow");
    }
    require_occurrences(
        "guid-like-applied/source SQL", log_text, source_field, 4
    );
    require_occurrences(
        "guid-like-applied/rewritten SQL", log_text, rewritten_field, 4
    );
    return SQLITE_OK;
}

static int rsh_custom_adapter_tag_applied_producer(
    const rsh_case_context *context,
    const void *immutable_data
) {
    const plex_tag_log_producer_spec *producer =
        (const plex_tag_log_producer_spec *)immutable_data;
    sqlite3 *db = rsh_context_db(context, producer->role);
    int i;

    for (i = 0; i < producer->count; i++) {
        const char *input_sql =
            (i == 1 || i == 2) ? TAG_COUNT_SQL : TAG_BROWSE_SQL;
        sqlite3_stmt *stmt = NULL;
        const char *tail = NULL;
        int rc = sqlite3_prepare_v2(db, input_sql, -1, &stmt, &tail);
        if (rc != SQLITE_OK) {
            failf("FAIL [tag-applied-log/prepare]: rc=%d err=%s",
                  rc, sqlite3_errmsg(db));
        }
        require_contains(
            "tag-applied-log/sql", sqlite3_sql(stmt), TAG_MEMBERSHIP_10
        );
        if (tail != input_sql + strlen(input_sql)) {
            failf("FAIL [tag-applied-log/tail]: tail_offset=%ld want=%ld",
                  (long)(tail - input_sql), (long)strlen(input_sql));
        }
        require_int(
            "tag-applied-log/finalize", sqlite3_finalize(stmt), SQLITE_OK
        );
    }
    return SQLITE_OK;
}

static int rsh_custom_adapter_tag_applied_visible_assert(
    const rsh_case_context *context,
    const void *immutable_data
) {
    static const char *needle =
        "event=rewrite_applied target=plex mode=taggings+membership";
    const char *log_text = context->captured_stderr;

    (void)immutable_data;
    require_occurrences("tag-applied-log/rewrite-applied", log_text, needle, 3);
    require_contains("tag-applied-log/first", log_text, "sample=first count=1");
    require_contains("tag-applied-log/new", log_text, "sample=new count=2");
    require_absent("tag-applied-log/repeated", log_text, "sample=new count=3");
    require_contains(
        "tag-applied-log/periodic", log_text, "sample=periodic count=1024"
    );
    require_contains("tag-applied-log/source", log_text, "source_sql=\"");
    require_contains("tag-applied-log/source-corr", log_text, "source_corr=");
    require_contains("tag-applied-log/rewritten-corr", log_text, " corr=");
    require_occurrences(
        "tag-applied-log/source-full", log_text, "source_sql=\"", 3
    );
    return SQLITE_OK;
}

static int rsh_custom_adapter_tag_applied_suppressed_assert(
    const rsh_case_context *context,
    const void *immutable_data
) {
    static const char *needle =
        "event=rewrite_applied target=plex mode=taggings+membership";
    const char *log_text = context->captured_stderr;

    (void)immutable_data;
    require_contains(
        "tag-applied-suppressed/first", log_text, "sample=first count=1"
    );
    require_contains(
        "tag-applied-suppressed/new", log_text, "sample=new count=2"
    );
    require_contains(
        "tag-applied-suppressed/periodic", log_text,
        "sample=periodic count=1024"
    );
    require_occurrences(
        "tag-applied-suppressed/rewrite-applied", log_text, needle, 3
    );
    require_contains(
        "tag-applied-suppressed/source-corr", log_text, "source_corr="
    );
    require_contains(
        "tag-applied-suppressed/rewritten-corr", log_text, " corr="
    );
    require_absent(
        "tag-applied-suppressed/source", log_text, " source_sql=\""
    );
    require_absent(
        "tag-applied-suppressed/rewritten", log_text, " sql=\""
    );
    require_no_same_line(
        "tag-applied-suppressed/source", log_text, needle, " source_sql=\""
    );
    require_no_same_line(
        "tag-applied-suppressed/rewritten", log_text, needle, " sql=\""
    );
    return SQLITE_OK;
}

static int rsh_custom_adapter_tag_index_missing_producer(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *db = rsh_context_db(context, (const char *)immutable_data);

    prepare_step_original(db, "tag-index-missing-first", TAG_BROWSE_SQL);
    prepare_step_original(db, "tag-index-missing-repeat", TAG_BROWSE_SQL);
    return SQLITE_OK;
}

static int rsh_custom_adapter_tag_index_probe_error_producer(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *db = rsh_context_db(context, (const char *)immutable_data);
    ondeck_failure_probe probe = {0, 1, 0};

    prepare_step_original(
        db, "tag-index-missing-new-connection", TAG_BROWSE_SQL
    );
    require_int(
        "tag-index-probe-error/authorizer",
        sqlite3_set_authorizer(db, ondeck_failure_authorizer_cb, &probe),
        SQLITE_OK
    );
    prepare_step_original(db, "tag-index-probe-error-first", TAG_BROWSE_SQL);
    prepare_step_original(db, "tag-index-probe-error-second", TAG_BROWSE_SQL);
    require_int(
        "tag-index-probe-error/authorizer-clear",
        sqlite3_set_authorizer(db, NULL, NULL), SQLITE_OK
    );
    return SQLITE_OK;
}

static int rsh_custom_adapter_tag_index_log_assert(
    const rsh_case_context *context,
    const void *immutable_data
) {
    static const char *missing =
        "event=rewrite_skipped target=plex reason=index_missing mode=taggings+membership";
    static const char *probe_error =
        "event=rewrite_skipped target=plex reason=index_probe_error mode=taggings+membership";
    const char *log_text = context->captured_stderr;

    (void)immutable_data;
    require_occurrences("tag-index-missing-run-total", log_text, missing, 1);
    require_same_line(
        "tag-index-missing-first/sample", log_text, missing,
        "sample=first count=1"
    );
    require_occurrences(
        "tag-index-probe-error-run-total", log_text, probe_error, 2
    );
    return SQLITE_OK;
}

static int rsh_custom_adapter_ondeck_capture_miss_producer(
    const rsh_case_context *context,
    const void *immutable_data
) {
    sqlite3 *db = rsh_context_db(context, (const char *)immutable_data);
    const char *per_guid_sqls[
        sizeof(plex_ondeck_per_guid_values) /
        sizeof(plex_ondeck_per_guid_values[0])
    ];
    char *per_guid_owned[
        sizeof(plex_ondeck_per_guid_values) /
        sizeof(plex_ondeck_per_guid_values[0]) - 1u
    ];
    char *per_guid_near_miss;
    size_t i;

    if (strlen(ONDECK_PER_GUID_SQL) != 1075u) {
        failf("FAIL [ondeck-per-guid/fixture-length]: got=%lu want=1075",
              (unsigned long)strlen(ONDECK_PER_GUID_SQL));
    }
    per_guid_sqls[0] = ONDECK_PER_GUID_SQL;
    for (i = 1;
         i < sizeof(plex_ondeck_per_guid_values) /
             sizeof(plex_ondeck_per_guid_values[0]);
         i++) {
        per_guid_owned[i - 1u] = copy_ondeck_per_guid_sql(
            plex_ondeck_per_guid_values[i]
        );
        per_guid_sqls[i] = per_guid_owned[i - 1u];
    }
    for (i = 0; i < sizeof(per_guid_sqls) / sizeof(per_guid_sqls[0]); i++) {
        prepare_original_without_step(
            db, "ondeck-per-guid-originals", per_guid_sqls[i]
        );
    }
    for (i = 0; i < sizeof(per_guid_owned) / sizeof(per_guid_owned[0]); i++) {
        free(per_guid_owned[i]);
    }

    per_guid_near_miss = make_ondeck_per_guid_near_miss();
    prepare_step_original(
        db, "ondeck-per-guid-near-miss", per_guid_near_miss
    );
    free(per_guid_near_miss);
    prepare_step_original(
        db, "ondeck-param-list-log", ONDECK_PARAM_LIST_SQL
    );
    prepare_step_original(
        db, "ondeck-param-named-log", ONDECK_PARAM_NAMED_SQL
    );
    prepare_step_original(
        db, "ondeck-param-leading-zero-log", ONDECK_PARAM_LEADING_ZERO_SQL
    );
    for (i = 0;
         i < sizeof(plex_ondeck_threshold_log_misses) /
             sizeof(plex_ondeck_threshold_log_misses[0]);
         i++) {
        prepare_step_original(
            db, plex_ondeck_threshold_log_misses[i].label,
            plex_ondeck_threshold_log_misses[i].sql
        );
    }
    prepare_step_original(db, "ondeck-early-head-miss", NONMATCH_SQL);
    return SQLITE_OK;
}

static int rsh_custom_adapter_ondeck_capture_miss_assert(
    const rsh_case_context *context,
    const void *immutable_data
) {
    static const char *capture_needle =
        "event=rewrite_skipped target=plex reason=capture_miss mode=ondeck";
    static const char *out_of_scope_needle =
        "event=rewrite_skipped target=plex reason=out_of_scope mode=ondeck";
    const char *log_text = context->captured_stderr;
    char *per_guid_near_miss;
    uint64_t param_shapes[4];
    size_t i;

    (void)immutable_data;
    require_occurrences(
        "ondeck-per-guid/out-of-scope-once", log_text, out_of_scope_needle, 1
    );
    require_same_line(
        "ondeck-per-guid/reason", log_text, out_of_scope_needle,
        "sub_reason=ondeck_per_guid"
    );
    require_same_line(
        "ondeck-per-guid/sample", log_text, out_of_scope_needle,
        "sample=first count=1"
    );
    require_same_line(
        "ondeck-per-guid/verbatim", log_text, out_of_scope_needle,
        ONDECK_PER_GUID_SQL
    );
    require_same_line(
        "ondeck-per-guid/length", log_text, out_of_scope_needle,
        "sql_len=1075"
    );
    require_occurrences(
        "ondeck-capture-miss/run-total", log_text, capture_needle, 14
    );

    per_guid_near_miss = make_ondeck_per_guid_near_miss();
    require_same_line(
        "ondeck-per-guid-near-miss/capture", log_text, capture_needle,
        per_guid_near_miss
    );
    require_same_line(
        "ondeck-per-guid-near-miss/selector", log_text, capture_needle,
        "sub_reason=selector"
    );
    require_no_same_line(
        "ondeck-per-guid-near-miss/out-of-scope", log_text,
        out_of_scope_needle, per_guid_near_miss
    );
    free(per_guid_near_miss);

    require_same_line(
        "ondeck-param-list-log/rewrite-skipped", log_text, capture_needle,
        ONDECK_PARAM_LIST_SQL
    );
    require_same_line(
        "ondeck-param-list-log/sub-reason", log_text, capture_needle,
        "sub_reason=id_list"
    );
    require_same_line(
        "ondeck-param-named-log/rewrite-skipped", log_text, capture_needle,
        ONDECK_PARAM_NAMED_SQL
    );
    require_same_line(
        "ondeck-param-named-log/sub-reason", log_text,
        ONDECK_PARAM_NAMED_SQL, "sub_reason=section"
    );
    require_same_line(
        "ondeck-param-named-log/sample", log_text,
        ONDECK_PARAM_NAMED_SQL, "sample=new"
    );
    param_shapes[0] = require_shape_field_on_line(
        "ondeck-param-named-log/shape", log_text,
        capture_needle, ONDECK_PARAM_NAMED_SQL
    );
    require_same_line(
        "ondeck-param-leading-zero-log/rewrite-skipped", log_text,
        capture_needle, ONDECK_PARAM_LEADING_ZERO_SQL
    );
    require_same_line(
        "ondeck-param-leading-zero-log/sub-reason", log_text,
        ONDECK_PARAM_LEADING_ZERO_SQL, "sub_reason=section"
    );
    require_same_line(
        "ondeck-param-leading-zero-log/sample", log_text,
        ONDECK_PARAM_LEADING_ZERO_SQL, "sample=new"
    );
    param_shapes[1] = require_shape_field_on_line(
        "ondeck-param-leading-zero-log/shape", log_text,
        capture_needle, ONDECK_PARAM_LEADING_ZERO_SQL
    );

    for (i = 0;
         i < sizeof(plex_ondeck_threshold_log_misses) /
             sizeof(plex_ondeck_threshold_log_misses[0]);
         i++) {
        const plex_ondeck_log_miss *miss =
            &plex_ondeck_threshold_log_misses[i];
        char metadata[160];
        int rc;

        require_same_line(miss->label, log_text, capture_needle, miss->sql);
        if (i == 1u || i == 2u) {
            require_same_line(miss->label, log_text, miss->sql, "sample=new");
            param_shapes[i + 1u] = require_shape_field_on_line(
                miss->label, log_text, capture_needle, miss->sql
            );
        }
        rc = snprintf(
            metadata, sizeof(metadata), "sub_reason=%s db=", miss->sub_reason
        );
        if (rc < 0 || (size_t)rc >= sizeof(metadata)) {
            failf("FATAL: capture metadata buffer overflow");
        }
        require_same_line(miss->label, log_text, capture_needle, metadata);
        rc = snprintf(
            metadata, sizeof(metadata), "sql_len=%lu corr=%016llx",
            (unsigned long)strlen(miss->sql),
            (unsigned long long)sql_corr_key(miss->sql, strlen(miss->sql))
        );
        if (rc < 0 || (size_t)rc >= sizeof(metadata)) {
            failf("FATAL: capture correlation buffer overflow");
        }
        require_same_line(miss->label, log_text, capture_needle, metadata);
        require_same_line(
            miss->label, log_text, "event=SQLITE_TRACE_STMT", metadata
        );
        require_same_line(miss->label, log_text, capture_needle, " sql=\"");
    }
    for (i = 0; i < sizeof(param_shapes) / sizeof(param_shapes[0]); i++) {
        size_t j;
        for (j = i + 1u; j < sizeof(param_shapes) / sizeof(param_shapes[0]); j++) {
            if (param_shapes[i] == param_shapes[j]) {
                failf(
                    "FAIL [ondeck-param-shapes]: index=%lu and index=%lu "
                    "share shape=%016llx",
                    (unsigned long)i, (unsigned long)j,
                    (unsigned long long)param_shapes[i]
                );
            }
        }
    }
    require_no_same_line(
        "ondeck-early-head-miss", log_text, capture_needle, " sql=\"select 1\""
    );
    require_same_line(
        "ondeck-early-head-trace", log_text, "event=SQLITE_TRACE_STMT",
        " sql=\"select 1\""
    );
    require_no_same_line(
        "ondeck-early-head-source", log_text,
        "event=rewrite_skipped target=plex", " sql=\"select 1\""
    );
    return SQLITE_OK;
}

static int rsh_bind_first_int(
    sqlite3_stmt *stmt,
    const char *label,
    void *ctx
) {
    (void)label;
    return sqlite3_bind_int(stmt, 1, *(const int *)ctx);
}

#define PLEX_PREPARE_V2 \
    {.kind = RSH_PREPARE_V2, \
     .nbyte_kind = RSH_NBYTE_MINUS_ONE, \
     .tail_kind = RSH_TAIL_FULL}
#define PLEX_SQL_CASE(case_label, role_name, source_sql, saved_sql) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_SQL_EXACT, \
        .build_mask = RSH_BUILD_PLEX_LINKED, \
        .data.sql_exact = { \
            .role = (role_name), \
            .sql = (source_sql), \
            .expected_sql = (saved_sql), \
            .prepare = PLEX_PREPARE_V2 \
        } \
    }
#define PLEX_ICU_SQL_CASE_PAIR(case_label, role_name, source_sql, rewritten_sql) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_SQL_EXACT, \
        .build_mask = RSH_BUILD_PLEX_LINKED, \
        .runtime_predicate = rsh_case_icu_available, \
        .data.sql_exact = { \
            .role = (role_name), \
            .sql = (source_sql), \
            .expected_sql = (rewritten_sql), \
            .prepare = PLEX_PREPARE_V2 \
        } \
    }, \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_SQL_EXACT, \
        .build_mask = RSH_BUILD_PLEX_LINKED, \
        .runtime_predicate = rsh_case_icu_unavailable, \
        .data.sql_exact = { \
            .role = (role_name), \
            .sql = (source_sql), \
            .expected_sql = (source_sql), \
            .prepare = PLEX_PREPARE_V2 \
        } \
    }
#define PLEX_ICU_LEGACY_SQL_CASE_PAIR( \
    case_label, role_name, source_sql, rewritten_sql) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_SQL_EXACT, \
        .build_mask = RSH_BUILD_PLEX_LINKED, \
        .runtime_predicate = rsh_case_icu_available, \
        .data.sql_exact = { \
            .role = (role_name), \
            .sql = (source_sql), \
            .expected_sql = (rewritten_sql), \
            .prepare = { \
                .kind = RSH_PREPARE_LEGACY, \
                .nbyte_kind = RSH_NBYTE_MINUS_ONE, \
                .tail_kind = RSH_TAIL_FULL \
            } \
        } \
    }, \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_SQL_EXACT, \
        .build_mask = RSH_BUILD_PLEX_LINKED, \
        .runtime_predicate = rsh_case_icu_unavailable, \
        .data.sql_exact = { \
            .role = (role_name), \
            .sql = (source_sql), \
            .expected_sql = (source_sql), \
            .prepare = { \
                .kind = RSH_PREPARE_LEGACY, \
                .nbyte_kind = RSH_NBYTE_MINUS_ONE, \
                .tail_kind = RSH_TAIL_FULL \
            } \
        } \
    }
#define PLEX_NEGATIVE_CASE(case_label, source_sql, unique_needle) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_NEGATIVE, \
        .build_mask = RSH_BUILD_PLEX_LINKED, \
        .data.negative = { \
            .source_kind = RSH_NEGATIVE_STATIC, \
            .sql = (source_sql), \
            .discriminating_needle = (unique_needle), \
            .prepare = PLEX_PREPARE_V2, \
            .vendor_role = "vendor", \
            .candidate_role = "candidate" \
        } \
    }

static const rsh_db_spec plex_candidate_db[] = {
    {
        .role = "candidate",
        .relative_path = "com.plexapp.plugins.library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE
    }
};

static const rsh_db_spec plex_candidate_tag_index_db[] = {
    {
        .role = "candidate",
        .relative_path = "com.plexapp.plugins.library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = PLEX_PROFILE_TAG_MEMBERSHIP_INDEX
    }
};

static const rsh_db_spec plex_candidate_ondeck_index_db[] = {
    {
        .role = "candidate",
        .relative_path = "com.plexapp.plugins.library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = PLEX_PROFILE_ONDECK_INDEX
    }
};

#define DEFINE_PLEX_SCALAR_PHASE(name, phase_label, db_rows, case_rows) \
    static const rsh_phase_spec name[] = { \
        { \
            .label = (phase_label), \
            .dbs = (db_rows), \
            .db_count = sizeof(db_rows) / sizeof((db_rows)[0]), \
            .cases = (case_rows), \
            .case_count = sizeof(case_rows) / sizeof((case_rows)[0]) \
        } \
    }

static const rsh_db_spec plex_tag_row_parity_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = PLEX_PROFILE_TAG_ROW_PARITY
    },
    {
        .role = "candidate",
        .relative_path = "com.plexapp.plugins.library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = PLEX_PROFILE_TAG_ROW_PARITY
    }
};

static const rsh_case_spec plex_tag_row_parity_cases[] = {
    {
        .label = "plex-taggings-contract",
        .kind = RSH_CASE_CONTRACT_PARITY,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_available,
        .data.contract_parity = {
            .vendor_role = "vendor",
            .candidate_role = "candidate",
            .vendor_sql = TAG_LIMIT_SQL,
            .candidate_source_sql = TAG_LIMIT_SQL,
            .expected_candidate_sql = TAG_LIMIT_SQL_REWRITTEN,
            .prepare = PLEX_PREPARE_V2
        }
    },
    {
        .label = "tag-row-parity",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom = rsh_custom_adapter_tag_row_parity,
        }
    }
};

DEFINE_PLEX_SCALAR_PHASE(
    plex_tag_row_parity_phases, "tag-row-parity",
    plex_tag_row_parity_dbs, plex_tag_row_parity_cases
);

static const rsh_case_spec plex_ondeck_threshold_fail_open_cases[] = {
    {
        .label = "ondeck-threshold-fail-open",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom = rsh_custom_adapter_ondeck_threshold_fail_open,
        }
    }
};

DEFINE_PLEX_SCALAR_PHASE(
    plex_ondeck_threshold_fail_open_phases, "ondeck-threshold-fail-open",
    plex_candidate_ondeck_index_db, plex_ondeck_threshold_fail_open_cases
);

static const rsh_case_spec plex_skip_log_producer_cases[] = {
    {
        .label = "skip-log-producer",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom = rsh_custom_adapter_skip_log_producer,
            .immutable_data = "candidate"
        }
    }
};

DEFINE_PLEX_SCALAR_PHASE(
    plex_skip_log_phases, "skip-log", plex_candidate_db,
    plex_skip_log_producer_cases
);

static const rsh_case_spec plex_skip_log_post_close_cases[] = {
    {
        .label = "skip-log",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_LOG_CAPTURE,
            .assert_custom = rsh_custom_adapter_skip_log_assert,
        }
    }
};

static const rsh_db_spec plex_guid_like_sampling_dbs[] = {
    {
        .role = "candidate-a",
        .relative_path = "guid-like-a/com.plexapp.plugins.library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE
    },
    {
        .role = "candidate-b",
        .relative_path = "guid-like-b/com.plexapp.plugins.library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE
    }
};

static const rsh_case_spec plex_guid_like_sampling_producer_cases[] = {
    {
        .label = "guid-like-applied-sampling-producer",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom = rsh_custom_adapter_guid_like_sampling_producer,
            .immutable_data = plex_guid_like_sampling_roles
        }
    }
};

DEFINE_PLEX_SCALAR_PHASE(
    plex_guid_like_sampling_phases, "guid-like-applied-sampling",
    plex_guid_like_sampling_dbs, plex_guid_like_sampling_producer_cases
);

static const rsh_case_spec plex_guid_like_sampling_post_close_cases[] = {
    {
        .label = "guid-like-applied-sampling",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_LOG_CAPTURE,
            .assert_custom = rsh_custom_adapter_guid_like_sampling_assert,
        }
    }
};

static const rsh_case_spec plex_tag_visible_producer_cases[] = {
    {
        .label = "tag-applied-log-producer",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom = rsh_custom_adapter_tag_applied_producer,
            .immutable_data = &plex_tag_visible_producer
        }
    }
};

DEFINE_PLEX_SCALAR_PHASE(
    plex_tag_visible_phases, "tag-applied-log",
    plex_candidate_tag_index_db, plex_tag_visible_producer_cases
);

static const rsh_case_spec plex_tag_visible_post_close_cases[] = {
    {
        .label = "tag-applied-log",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_LOG_CAPTURE,
            .assert_custom = rsh_custom_adapter_tag_applied_visible_assert,
        }
    }
};

static const rsh_case_spec plex_tag_suppressed_producer_cases[] = {
    {
        .label = "tag-applied-sql-suppressed-producer",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom = rsh_custom_adapter_tag_applied_producer,
            .immutable_data = &plex_tag_suppressed_producer
        }
    }
};

DEFINE_PLEX_SCALAR_PHASE(
    plex_tag_suppressed_phases, "tag-applied-sql-suppressed",
    plex_candidate_tag_index_db, plex_tag_suppressed_producer_cases
);

static const rsh_case_spec plex_tag_suppressed_post_close_cases[] = {
    {
        .label = "tag-applied-sql-suppressed",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_LOG_CAPTURE,
            .assert_custom = rsh_custom_adapter_tag_applied_suppressed_assert,
        }
    }
};

static const rsh_case_spec plex_tag_index_missing_producer_cases[] = {
    {
        .label = "tag-index-missing-producer",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom = rsh_custom_adapter_tag_index_missing_producer,
            .immutable_data = "candidate"
        }
    }
};

static const rsh_case_spec plex_tag_index_probe_error_producer_cases[] = {
    {
        .label = "tag-index-probe-error-producer",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom = rsh_custom_adapter_tag_index_probe_error_producer,
            .immutable_data = "candidate"
        }
    }
};

static const rsh_phase_spec plex_tag_index_log_phases[] = {
    {
        .label = "tag-index-missing-first-connection",
        .dbs = plex_candidate_db,
        .db_count = sizeof(plex_candidate_db) / sizeof(plex_candidate_db[0]),
        .cases = plex_tag_index_missing_producer_cases,
        .case_count = sizeof(plex_tag_index_missing_producer_cases) /
                      sizeof(plex_tag_index_missing_producer_cases[0])
    },
    {
        .label = "tag-index-missing-second-connection",
        .dbs = plex_candidate_db,
        .db_count = sizeof(plex_candidate_db) / sizeof(plex_candidate_db[0]),
        .cases = plex_tag_index_probe_error_producer_cases,
        .case_count = sizeof(plex_tag_index_probe_error_producer_cases) /
                      sizeof(plex_tag_index_probe_error_producer_cases[0])
    }
};

static const rsh_case_spec plex_tag_index_log_post_close_cases[] = {
    {
        .label = "tag-index-log-dedup",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_LOG_CAPTURE,
            .assert_custom = rsh_custom_adapter_tag_index_log_assert,
        }
    }
};

static const rsh_case_spec plex_ondeck_capture_miss_producer_cases[] = {
    {
        .label = "ondeck-capture-miss-log-producer",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom = rsh_custom_adapter_ondeck_capture_miss_producer,
            .immutable_data = "candidate"
        }
    }
};

DEFINE_PLEX_SCALAR_PHASE(
    plex_ondeck_capture_miss_phases, "ondeck-capture-miss-log",
    plex_candidate_db, plex_ondeck_capture_miss_producer_cases
);

static const rsh_case_spec plex_ondeck_capture_miss_post_close_cases[] = {
    {
        .label = "ondeck-capture-miss-log",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_LOG_CAPTURE,
            .assert_custom = rsh_custom_adapter_ondeck_capture_miss_assert,
        }
    }
};

static const rsh_case_spec plex_env_default_cases[] = {
    PLEX_ICU_SQL_CASE_PAIR(
        "env-default", "candidate", MATCH_SQL_INT, MATCH_SQL_INT_REWRITTEN
    ),
    PLEX_ICU_LEGACY_SQL_CASE_PAIR(
        "legacy-authorizer", "candidate", MATCH_SQL_INT,
        MATCH_SQL_INT_REWRITTEN
    )
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_env_default_phases, "env-default", plex_candidate_db,
    plex_env_default_cases
);

static const rsh_case_spec plex_env_one_disabled_cases[] = {
    PLEX_SQL_CASE("env-one-disabled", "candidate", MATCH_SQL_INT, MATCH_SQL_INT),
    {
        .label = "legacy-authorizer",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.sql_exact = {
            .role = "candidate",
            .sql = MATCH_SQL_INT,
            .expected_sql = MATCH_SQL_INT,
            .prepare = {
                .kind = RSH_PREPARE_LEGACY,
                .nbyte_kind = RSH_NBYTE_MINUS_ONE,
                .tail_kind = RSH_TAIL_FULL
            }
        }
    }
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_env_one_disabled_phases, "env-one-disabled", plex_candidate_db,
    plex_env_one_disabled_cases
);

static const rsh_case_spec plex_env_garbage_enabled_cases[] = {
    PLEX_ICU_SQL_CASE_PAIR(
        "env-garbage-enabled", "candidate", MATCH_SQL_INT,
        MATCH_SQL_INT_REWRITTEN
    ),
    PLEX_ICU_LEGACY_SQL_CASE_PAIR(
        "legacy-authorizer", "candidate", MATCH_SQL_INT,
        MATCH_SQL_INT_REWRITTEN
    )
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_env_garbage_enabled_phases, "env-garbage-enabled", plex_candidate_db,
    plex_env_garbage_enabled_cases
);

static const rsh_case_spec plex_guid_like_env_default_cases[] = {
    PLEX_SQL_CASE(
        "guid-like-env-default", "candidate", GUID_LIKE_SQL, GUID_LIKE_SQL
    )
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_guid_like_env_default_phases, "guid-like-env-default",
    plex_candidate_db, plex_guid_like_env_default_cases
);

static const rsh_case_spec plex_guid_like_env_one_disabled_cases[] = {
    PLEX_SQL_CASE(
        "guid-like-env-one-disabled", "candidate", GUID_LIKE_SQL, GUID_LIKE_SQL
    )
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_guid_like_env_one_disabled_phases, "guid-like-env-one-disabled",
    plex_candidate_db, plex_guid_like_env_one_disabled_cases
);

static const rsh_case_spec plex_guid_like_env_garbage_disabled_cases[] = {
    PLEX_SQL_CASE(
        "guid-like-env-garbage-disabled", "candidate", GUID_LIKE_SQL,
        GUID_LIKE_SQL
    )
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_guid_like_env_garbage_disabled_phases,
    "guid-like-env-garbage-disabled", plex_candidate_db,
    plex_guid_like_env_garbage_disabled_cases
);

static const rsh_case_spec plex_tag_env_default_cases[] = {
    PLEX_ICU_SQL_CASE_PAIR(
        "tag-env-default", "candidate", TAG_BROWSE_SQL, TAG_BROWSE_SQL_REWRITTEN
    )
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_tag_env_default_phases, "tag-env-default", plex_candidate_tag_index_db,
    plex_tag_env_default_cases
);

static const rsh_case_spec plex_tag_env_zero_enabled_cases[] = {
    PLEX_ICU_SQL_CASE_PAIR(
        "tag-env-zero-enabled", "candidate", TAG_BROWSE_SQL,
        TAG_BROWSE_SQL_REWRITTEN
    )
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_tag_env_zero_enabled_phases, "tag-env-zero-enabled",
    plex_candidate_tag_index_db, plex_tag_env_zero_enabled_cases
);

static const rsh_case_spec plex_tag_env_one_disabled_cases[] = {
    PLEX_SQL_CASE(
        "tag-env-one-disabled", "candidate", TAG_BROWSE_SQL, TAG_BROWSE_SQL
    )
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_tag_env_one_disabled_phases, "tag-env-one-disabled",
    plex_candidate_tag_index_db, plex_tag_env_one_disabled_cases
);

static const rsh_case_spec plex_tag_env_garbage_enabled_cases[] = {
    PLEX_ICU_SQL_CASE_PAIR(
        "tag-env-garbage-enabled", "candidate", TAG_BROWSE_SQL,
        TAG_BROWSE_SQL_REWRITTEN
    )
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_tag_env_garbage_enabled_phases, "tag-env-garbage-enabled",
    plex_candidate_tag_index_db, plex_tag_env_garbage_enabled_cases
);

static const rsh_case_spec plex_ondeck_env_default_cases[] = {
    PLEX_SQL_CASE("ondeck-env-default", "candidate", ONDECK_SQL, ONDECK_SQL)
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_ondeck_env_default_phases, "ondeck-env-default",
    plex_candidate_ondeck_index_db, plex_ondeck_env_default_cases
);

static const rsh_case_spec plex_ondeck_env_one_disabled_cases[] = {
    PLEX_SQL_CASE("ondeck-env-one-disabled", "candidate", ONDECK_SQL, ONDECK_SQL)
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_ondeck_env_one_disabled_phases, "ondeck-env-one-disabled",
    plex_candidate_ondeck_index_db, plex_ondeck_env_one_disabled_cases
);

static const rsh_case_spec plex_ondeck_env_garbage_disabled_cases[] = {
    PLEX_SQL_CASE(
        "ondeck-env-garbage-disabled", "candidate", ONDECK_SQL, ONDECK_SQL
    )
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_ondeck_env_garbage_disabled_phases, "ondeck-env-garbage-disabled",
    plex_candidate_ondeck_index_db, plex_ondeck_env_garbage_disabled_cases
);

static const rsh_db_spec plex_path_negative_dbs[] = {
    {
        .role = "library",
        .relative_path = "library.db",
        .kind = RSH_DB_AUXILIARY,
        .storage = RSH_DB_RELATIVE
    },
    {
        .role = "jellyfin",
        .relative_path = "jellyfin.db",
        .kind = RSH_DB_AUXILIARY,
        .storage = RSH_DB_RELATIVE
    },
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE
    },
    {
        .role = "memory",
        .relative_path = ":memory:",
        .kind = RSH_DB_AUXILIARY,
        .storage = RSH_DB_MEMORY
    },
    {
        .role = "non-ascii",
        .relative_path = "pl\303\251x/com.plexapp.plugins.library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE
    }
};

static const rsh_case_spec plex_path_negative_cases[] = {
    {
        .label = "library.db",
        .kind = RSH_CASE_PATH,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.path.assertion = {
            .role = "library",
            .sql = MATCH_SQL_INT,
            .expected_sql = MATCH_SQL_INT,
            .prepare = PLEX_PREPARE_V2
        }
    },
    {
        .label = "jellyfin.db",
        .kind = RSH_CASE_PATH,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.path.assertion = {
            .role = "jellyfin",
            .sql = MATCH_SQL_INT,
            .expected_sql = MATCH_SQL_INT,
            .prepare = PLEX_PREPARE_V2
        }
    },
    {
        .label = "not-target.db",
        .kind = RSH_CASE_PATH,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.path.assertion = {
            .role = "vendor",
            .sql = MATCH_SQL_INT,
            .expected_sql = MATCH_SQL_INT,
            .prepare = PLEX_PREPARE_V2
        }
    },
    {
        .label = "memory",
        .kind = RSH_CASE_PATH,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.path.assertion = {
            .role = "memory",
            .sql = MATCH_SQL_INT,
            .expected_sql = MATCH_SQL_INT,
            .prepare = PLEX_PREPARE_V2
        }
    },
    {
        .label = "non-ascii-path",
        .kind = RSH_CASE_PATH,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.path = {
            .assertion = {
                .role = "non-ascii",
                .sql = MATCH_SQL_INT,
                .expected_sql = MATCH_SQL_INT,
                .prepare = PLEX_PREPARE_V2
            },
            .assert_after_prepare = rsh_custom_adapter_path_non_ascii,
            .immutable_data = "non-ascii"
        }
    }
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_path_negative_phases, "path-negative", plex_path_negative_dbs,
    plex_path_negative_cases
);

static const rsh_db_spec plex_vendor_candidate_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE
    },
    {
        .role = "candidate",
        .relative_path = "com.plexapp.plugins.library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE
    }
};

static const int plex_boundary_bind_six = 6;
static const sqlite3_int64 plex_boundary_plus_int_id[] = {14};
static const sqlite3_int64 plex_boundary_plus_param_id[] = {14};
static const sqlite3_int64 plex_boundary_hex_id[] = {15};

static const rsh_case_spec plex_nonmatch_cases[] = {
    PLEX_NEGATIVE_CASE("nonmatch-select", NONMATCH_SQL, "select 1"),
    PLEX_NEGATIVE_CASE("no-fts-table", NO_FTS_SQL, "from tags where tag_type=6"),
    PLEX_NEGATIVE_CASE("no-target-column", NO_TARGET_SQL, "and tags.id=10"),
    PLEX_NEGATIVE_CASE(
        "match-named-param", MATCH_SQL_NAMED_MATCH_PARAM,
        "match @SearchTerm and tag_type=6"
    ),
    PLEX_NEGATIVE_CASE(
        "match-numbered-param", MATCH_SQL_NUMBERED_MATCH_PARAM,
        "match ?1 and tag_type=6"
    ),
    PLEX_NEGATIVE_CASE(
        "duplicate-target-column", DUPLICATE_TARGET_SQL,
        "and tag_type=6 and tag_type=1"
    ),
    PLEX_NEGATIVE_CASE("cross-scope-cte", CROSS_SCOPE_CTE_SQL, "with matched as ("),
    PLEX_NEGATIVE_CASE(
        "cross-scope-subquery", CROSS_SCOPE_SUBQUERY_SQL,
        "exists (select 1 from fts4_tag_titles_icu"
    ),
    PLEX_NEGATIVE_CASE(
        "projection-only-target-column", PROJECTION_ONLY_TAG_TYPE_EQ_SQL,
        "select (tag_type=6)"
    ),
    PLEX_NEGATIVE_CASE(
        "order-only-target-column", ORDER_ONLY_TAG_TYPE_EQ_SQL,
        "order by tag_type=6"
    ),
    PLEX_NEGATIVE_CASE(
        "left-bound-target-column", LEFT_BOUND_TAG_TYPE_EQ_SQL,
        "and 1 + tag_type=4"
    ),
    PLEX_NEGATIVE_CASE("boundary-plus-int", BOUNDARY_PLUS_INT_SQL, "tag_type=6+1"),
    {
        .label = "boundary-plus-int-result",
        .kind = RSH_CASE_EXACT_IDS,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.exact_ids = {
            .role = "candidate",
            .sql = BOUNDARY_PLUS_INT_SQL,
            .prepare = PLEX_PREPARE_V2,
            .expected_ids = plex_boundary_plus_int_id,
            .expected_id_count = 1
        }
    },
    PLEX_NEGATIVE_CASE(
        "boundary-plus-param", BOUNDARY_PLUS_PARAM_SQL, "tag_type=?+1"
    ),
    {
        .label = "boundary-plus-param-result",
        .kind = RSH_CASE_EXACT_IDS,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.exact_ids = {
            .role = "candidate",
            .sql = BOUNDARY_PLUS_PARAM_SQL,
            .prepare = PLEX_PREPARE_V2,
            .bind = rsh_bind_first_int,
            .bind_ctx = (void *)&plex_boundary_bind_six,
            .expected_ids = plex_boundary_plus_param_id,
            .expected_id_count = 1
        }
    },
    PLEX_NEGATIVE_CASE("boundary-hex", BOUNDARY_HEX_SQL, "tag_type=0x1f"),
    {
        .label = "boundary-hex-result",
        .kind = RSH_CASE_EXACT_IDS,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.exact_ids = {
            .role = "candidate",
            .sql = BOUNDARY_HEX_SQL,
            .prepare = PLEX_PREPARE_V2,
            .expected_ids = plex_boundary_hex_id,
            .expected_id_count = 1
        }
    },
    {
        .label = "nonmatch-prepare-denial",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_FAULT_MATRIX,
            .assert_custom = rsh_custom_adapter_nonmatch_prepare_denial,
        }
    }
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_nonmatch_phases, "nonmatch", plex_vendor_candidate_dbs,
    plex_nonmatch_cases
);

#define PLEX_ICU_NEGATIVE_CASE(case_label, source_sql, unique_needle) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_NEGATIVE, \
        .build_mask = RSH_BUILD_PLEX_LINKED, \
        .runtime_predicate = rsh_case_icu_available, \
        .data.negative = { \
            .source_kind = RSH_NEGATIVE_STATIC, \
            .sql = (source_sql), \
            .discriminating_needle = (unique_needle), \
            .prepare = PLEX_PREPARE_V2, \
            .vendor_role = "vendor", \
            .candidate_role = "candidate" \
        } \
    }

static plex_fts_tie_exception plex_positive_fts_tie;

static const rsh_case_spec plex_positive_cases[] = {
    PLEX_ICU_SQL_CASE_PAIR("v2-int", "candidate", MATCH_SQL_INT, MATCH_SQL_INT_REWRITTEN),
    PLEX_ICU_LEGACY_SQL_CASE_PAIR(
        "legacy-authorizer", "candidate", MATCH_SQL_INT,
        MATCH_SQL_INT_REWRITTEN
    ),
    {
        .label = "v3-int",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_available,
        .data.sql_exact = {
            .role = "candidate",
            .sql = MATCH_SQL_INT,
            .expected_sql = MATCH_SQL_INT_REWRITTEN,
            .prepare = {
                .kind = RSH_PREPARE_V3,
                .nbyte_kind = RSH_NBYTE_MINUS_ONE,
                .tail_kind = RSH_TAIL_FULL
            }
        }
    },
    {
        .label = "v3-int",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_unavailable,
        .data.sql_exact = {
            .role = "candidate",
            .sql = MATCH_SQL_INT,
            .expected_sql = MATCH_SQL_INT,
            .prepare = {
                .kind = RSH_PREPARE_V3,
                .nbyte_kind = RSH_NBYTE_MINUS_ONE,
                .tail_kind = RSH_TAIL_FULL
            }
        }
    },
    PLEX_ICU_SQL_CASE_PAIR(
        "lean-no-metadata-group-limit", "candidate", MATCH_SQL_LEAN,
        MATCH_SQL_LEAN_REWRITTEN
    ),
    PLEX_ICU_SQL_CASE_PAIR(
        "projected-tag-type", "candidate", PROJECTED_TAG_TYPE_SQL,
        PROJECTED_TAG_TYPE_SQL_REWRITTEN
    ),
    {
        .label = "nbyte-positive-nul",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_available,
        .data.sql_exact = {
            .role = "candidate",
            .sql = MATCH_SQL_INT,
            .expected_sql = MATCH_SQL_INT_REWRITTEN,
            .prepare = {
                .kind = RSH_PREPARE_V2,
                .nbyte_kind = RSH_NBYTE_LENGTH_WITH_NUL,
                .tail_kind = RSH_TAIL_FULL
            }
        }
    },
    {
        .label = "nbyte-positive-nul",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_unavailable,
        .data.sql_exact = {
            .role = "candidate",
            .sql = MATCH_SQL_INT,
            .expected_sql = MATCH_SQL_INT,
            .prepare = {
                .kind = RSH_PREPARE_V2,
                .nbyte_kind = RSH_NBYTE_LENGTH_WITH_NUL,
                .tail_kind = RSH_TAIL_FULL
            }
        }
    },
    {
        .label = "nbyte-positive-no-nul",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_available,
        .data.sql_exact = {
            .role = "candidate",
            .sql = MATCH_SQL_INT,
            .expected_sql = MATCH_SQL_INT_REWRITTEN,
            .prepare = {
                .kind = RSH_PREPARE_V2,
                .nbyte_kind = RSH_NBYTE_EXACT_LENGTH,
                .tail_kind = RSH_TAIL_FULL
            }
        }
    },
    {
        .label = "nbyte-positive-no-nul",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_unavailable,
        .data.sql_exact = {
            .role = "candidate",
            .sql = MATCH_SQL_INT,
            .expected_sql = MATCH_SQL_INT,
            .prepare = {
                .kind = RSH_PREPARE_V2,
                .nbyte_kind = RSH_NBYTE_EXACT_LENGTH,
                .tail_kind = RSH_TAIL_FULL
            }
        }
    },
    {
        .label = "pztail-null",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_available,
        .data.sql_exact = {
            .role = "candidate",
            .sql = MATCH_SQL_INT,
            .expected_sql = MATCH_SQL_INT_REWRITTEN,
            .prepare = {
                .kind = RSH_PREPARE_V2,
                .nbyte_kind = RSH_NBYTE_MINUS_ONE,
                .tail_kind = RSH_TAIL_NULL_OUT
            }
        }
    },
    {
        .label = "pztail-null",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_unavailable,
        .data.sql_exact = {
            .role = "candidate",
            .sql = MATCH_SQL_INT,
            .expected_sql = MATCH_SQL_INT,
            .prepare = {
                .kind = RSH_PREPARE_V2,
                .nbyte_kind = RSH_NBYTE_MINUS_ONE,
                .tail_kind = RSH_TAIL_NULL_OUT
            }
        }
    },
    PLEX_ICU_SQL_CASE_PAIR(
        "quoted", "candidate", MATCH_SQL_QUOTED, MATCH_SQL_QUOTED_REWRITTEN
    ),
    PLEX_ICU_SQL_CASE_PAIR(
        "param", "candidate", MATCH_SQL_PARAM, MATCH_SQL_PARAM_REWRITTEN
    ),
    {
        .label = "grouped-digest-identity",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom = rsh_custom_adapter_grouped_digest_identity,
            .immutable_data = "candidate"
        }
    },
    PLEX_SQL_CASE(
        "exec-miss", "candidate", "PRAGMA user_version;", "PRAGMA user_version;"
    ),
    {
        .label = "plex-fts-contract",
        .kind = RSH_CASE_CONTRACT_PARITY,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_available,
        .data.contract_parity = {
            .vendor_role = "vendor",
            .candidate_role = "candidate",
            .vendor_sql = MATCH_SQL_INT,
            .candidate_source_sql = MATCH_SQL_INT,
            .expected_candidate_sql = MATCH_SQL_INT_REWRITTEN,
            .prepare = PLEX_PREPARE_V2,
            .row_exception = accept_plex_fts_count_tie,
            .row_exception_ctx = &plex_positive_fts_tie,
            .minimum_rows = 1
        }
    },
    {
        .label = "plex-fts-contract-tied-order",
        .kind = RSH_CASE_CUSTOM,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_available,
        .data.custom = {
            .kind = RSH_CUSTOM_IDENTITY,
            .assert_custom = rsh_custom_adapter_plex_fts_tied_order,
            .immutable_data = &plex_positive_fts_tie
        }
    }
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_positive_phases, "positive", plex_vendor_candidate_dbs,
    plex_positive_cases
);

static guid_like_bind_values plex_guid_null_binds = {NULL, 3};
static guid_like_bind_values plex_guid_prefix_binds = {"plex://item/%", 3};

static const rsh_case_spec plex_guid_like_cases[] = {
    {
        .label = "guid-like-null-contract",
        .kind = RSH_CASE_CONTRACT_PARITY,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_available,
        .data.contract_parity = {
            .vendor_role = "vendor",
            .candidate_role = "candidate",
            .vendor_sql = GUID_LIKE_SQL,
            .candidate_source_sql = GUID_LIKE_SQL,
            .expected_candidate_sql = GUID_LIKE_SQL_REWRITTEN,
            .prepare = PLEX_PREPARE_V2,
            .bind = bind_guid_like_values,
            .bind_ctx = &plex_guid_null_binds,
            .minimum_rows = 0
        }
    },
    {
        .label = "guid-like-prefix-contract",
        .kind = RSH_CASE_CONTRACT_PARITY,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_available,
        .data.contract_parity = {
            .vendor_role = "vendor",
            .candidate_role = "candidate",
            .vendor_sql = GUID_LIKE_SQL,
            .candidate_source_sql = GUID_LIKE_SQL,
            .expected_candidate_sql = GUID_LIKE_SQL_REWRITTEN,
            .prepare = PLEX_PREPARE_V2,
            .bind = bind_guid_like_values,
            .bind_ctx = &plex_guid_prefix_binds,
            .row_exception = accept_guid_like_legal_difference,
            .minimum_rows = 1
        }
    },
    {
        .label = "guid-like-colon-named-contract",
        .kind = RSH_CASE_CONTRACT_PARITY,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_available,
        .data.contract_parity = {
            .vendor_role = "vendor",
            .candidate_role = "candidate",
            .vendor_sql = GUID_LIKE_COLON_NAMED_SQL,
            .candidate_source_sql = GUID_LIKE_COLON_NAMED_SQL,
            .expected_candidate_sql = GUID_LIKE_COLON_NAMED_SQL_REWRITTEN,
            .prepare = PLEX_PREPARE_V2,
            .bind = bind_guid_like_values,
            .bind_ctx = &plex_guid_prefix_binds,
            .row_exception = accept_guid_like_legal_difference,
            .minimum_rows = 1
        }
    },
    {
        .label = "guid-like-colon-named",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_available,
        .data.sql_exact = {
            .role = "candidate",
            .sql = GUID_LIKE_COLON_NAMED_SQL,
            .expected_sql = GUID_LIKE_COLON_NAMED_SQL_REWRITTEN,
            .prepare = PLEX_PREPARE_V2
        }
    },
    {
        .label = "guid-like-qmark-numbered",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_available,
        .data.sql_exact = {
            .role = "candidate",
            .sql = GUID_LIKE_QMARK_NUMBERED_SQL,
            .expected_sql = GUID_LIKE_QMARK_NUMBERED_SQL_REWRITTEN,
            .prepare = PLEX_PREPARE_V2
        }
    },
    {
        .label = "guid-like-at-named",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_available,
        .data.sql_exact = {
            .role = "candidate",
            .sql = GUID_LIKE_AT_NAMED_SQL,
            .expected_sql = GUID_LIKE_AT_NAMED_SQL_REWRITTEN,
            .prepare = PLEX_PREPARE_V2
        }
    },
    {
        .label = "guid-like-dollar-named",
        .kind = RSH_CASE_SQL_EXACT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_available,
        .data.sql_exact = {
            .role = "candidate",
            .sql = GUID_LIKE_DOLLAR_NAMED_SQL,
            .expected_sql = GUID_LIKE_DOLLAR_NAMED_SQL_REWRITTEN,
            .prepare = PLEX_PREPARE_V2
        }
    },
    PLEX_ICU_NEGATIVE_CASE(
        "guid-like-anonymous-bind-negative", GUID_LIKE_ANONYMOUS_SQL,
        "(mt.`guid` LIKE ?) LIMIT ?"
    ),
    PLEX_ICU_NEGATIVE_CASE(
        "guid-like-anonymous-pattern-negative", GUID_LIKE_ANONYMOUS_PATTERN_SQL,
        "(mt.`guid` LIKE ?) LIMIT ?2"
    ),
    PLEX_ICU_NEGATIVE_CASE(
        "guid-like-anonymous-limit-negative", GUID_LIKE_ANONYMOUS_LIMIT_SQL,
        "(mt.`guid` LIKE :C1) LIMIT ?"
    ),
    PLEX_ICU_NEGATIVE_CASE(
        "guid-like-shape-negative", GUID_LIKE_NEGATIVE_SQL,
        "WHERE mt.`guid` LIKE :1 LIMIT :2"
    )
};
DEFINE_PLEX_SCALAR_PHASE(
    plex_guid_like_phases, "guid-like", plex_vendor_candidate_dbs,
    plex_guid_like_cases
);

static const rsh_db_spec plex_tag_membership_main_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = PLEX_PROFILE_TAG_MEMBERSHIP_INDEX
    },
    {
        .role = "candidate",
        .relative_path = "com.plexapp.plugins.library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = PLEX_PROFILE_TAG_MEMBERSHIP_INDEX
    },
    {
        .role = "non-target",
        .relative_path = "library.db",
        .kind = RSH_DB_AUXILIARY,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = PLEX_PROFILE_TAG_MEMBERSHIP_INDEX
    }
};

static const rsh_case_spec plex_tag_membership_main_cases[] = {
    PLEX_ICU_SQL_CASE_PAIR(
        "tag-browse", "candidate", TAG_BROWSE_SQL, TAG_BROWSE_SQL_REWRITTEN
    ),
    PLEX_ICU_SQL_CASE_PAIR(
        "tag-count", "candidate", TAG_COUNT_SQL, TAG_COUNT_SQL_REWRITTEN
    ),
    PLEX_ICU_SQL_CASE_PAIR(
        "tag-limit", "candidate", TAG_LIMIT_SQL, TAG_LIMIT_SQL_REWRITTEN
    ),
    PLEX_ICU_SQL_CASE_PAIR(
        "tag-flipped-join", "candidate", TAG_FLIPPED_JOIN_SQL,
        TAG_FLIPPED_JOIN_SQL_REWRITTEN
    ),
    PLEX_NEGATIVE_CASE(
        "tag-already-rewritten", TAG_BROWSE_SQL_REWRITTEN, TAG_MEMBERSHIP_10
    ),
    PLEX_NEGATIVE_CASE(
        "tag-missing-join", TAG_MISSING_JOIN_SQL,
        "left join taggings on taggings.tag_id=10"
    ),
    PLEX_NEGATIVE_CASE(
        "tag-nested-join", TAG_NESTED_JOIN_SQL,
        "exists (select 1 from taggings where"
    ),
    PLEX_NEGATIVE_CASE(
        "tag-join-in-string", TAG_JOIN_IN_STRING_SQL,
        "'taggings.metadata_item_id=metadata_items.id taggings.tag_id=tags.id'"
    ),
    PLEX_NEGATIVE_CASE(
        "tag-join-in-comment", TAG_JOIN_IN_COMMENT_SQL,
        "/* taggings.metadata_item_id=metadata_items.id and taggings.tag_id=tags.id */"
    ),
    PLEX_NEGATIVE_CASE(
        "tag-duplicate-id", TAG_DUPLICATE_ID_SQL,
        "where tags.id=10 and tags.id=11"
    ),
    PLEX_NEGATIVE_CASE("tag-bound-id", TAG_BOUND_ID_SQL, "where tags.id=?"),
    PLEX_NEGATIVE_CASE("tag-named-id", TAG_NAMED_ID_SQL, "where tags.id=:tag_id"),
    PLEX_NEGATIVE_CASE("tag-string-id", TAG_STRING_ID_SQL, "where tags.id='10'"),
    PLEX_NEGATIVE_CASE(
        "tag-or-id", TAG_OR_ID_SQL,
        "(metadata_items.metadata_type=1 or tags.id=10)"
    ),
    PLEX_NEGATIVE_CASE("tag-not-id", TAG_NOT_ID_SQL, "not (tags.id=10)"),
    PLEX_NEGATIVE_CASE(
        "tag-value-compared-id", TAG_VALUE_COMPARED_ID_SQL, "(tags.id=10)=1"
    ),
    PLEX_NEGATIVE_CASE(
        "tag-column-rhs-only", TAG_COLUMN_RHS_ONLY_SQL,
        "left join tags on tags.id=taggings.tag_id"
    ),
    PLEX_NEGATIVE_CASE(
        "tag-metadata-fts", TAG_METADATA_FTS_SQL,
        "join fts4_metadata_titles_icu on metadata_items.id=fts4_metadata_titles_icu.rowid"
    ),
    PLEX_NEGATIVE_CASE(
        "tag-unrelated-fts", MATCH_SQL_INT,
        "fts4_tag_titles_icu.tag match 'Django*' and tag_type=6"
    ),
    PLEX_SQL_CASE(
        "tag-non-target", "non-target", TAG_BROWSE_SQL, TAG_BROWSE_SQL
    )
};

static const rsh_case_spec plex_tag_membership_missing_index_cases[] = {
    PLEX_NEGATIVE_CASE(
        "tag-missing-index", TAG_BROWSE_SQL, "limit 2 offset 0"
    )
};

static const rsh_phase_spec plex_tag_membership_phases[] = {
    {
        .label = "tag-membership-main",
        .dbs = plex_tag_membership_main_dbs,
        .db_count = sizeof(plex_tag_membership_main_dbs) /
                    sizeof(plex_tag_membership_main_dbs[0]),
        .cases = plex_tag_membership_main_cases,
        .case_count = sizeof(plex_tag_membership_main_cases) /
                      sizeof(plex_tag_membership_main_cases[0])
    },
    {
        .label = "tag-membership-missing-index",
        .dbs = plex_vendor_candidate_dbs,
        .db_count = sizeof(plex_vendor_candidate_dbs) /
                    sizeof(plex_vendor_candidate_dbs[0]),
        .cases = plex_tag_membership_missing_index_cases,
        .case_count = sizeof(plex_tag_membership_missing_index_cases) /
                      sizeof(plex_tag_membership_missing_index_cases[0])
    }
};

static ondeck_bind_values plex_ondeck_bind_none = {0, {0, 0, 0}};
static ondeck_bind_values plex_ondeck_bind_section = {1, {2, 0, 0}};
static ondeck_bind_values plex_ondeck_bind_account = {1, {42, 0, 0}};
static ondeck_bind_values plex_ondeck_bind_both = {2, {2, 42, 0}};
static ondeck_bind_values plex_ondeck_bind_reversed = {2, {42, 2, 0}};
static ondeck_bind_values plex_ondeck_bind_hole = {3, {0, 2, 42}};
static ondeck_bind_values plex_ondeck_bind_reuse = {1, {2, 0, 0}};
static ondeck_contract plex_ondeck_two_row_contract = {2, 3, 0};
static ondeck_contract plex_ondeck_one_row_account_two_contract = {1, 2, 1};
static ondeck_contract plex_ondeck_threshold_contract = {1, 1, 0};

static const rsh_db_spec plex_ondeck_main_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = PLEX_PROFILE_ONDECK_INDEX
    },
    {
        .role = "candidate",
        .relative_path = "com.plexapp.plugins.library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = PLEX_PROFILE_ONDECK_INDEX
    },
    {
        .role = "non-target",
        .relative_path = "library.db",
        .kind = RSH_DB_AUXILIARY,
        .storage = RSH_DB_RELATIVE,
        .setup_profile = PLEX_PROFILE_ONDECK_INDEX
    }
};

#define PLEX_ONDECK_PARITY_CASE( \
    case_label, source_sql, expected_sql, binds_ptr, contract_ptr) \
    { \
        .label = (case_label), \
        .kind = RSH_CASE_CONTRACT_PARITY, \
        .build_mask = RSH_BUILD_PLEX_LINKED, \
        .runtime_predicate = rsh_case_icu_available, \
        .data.contract_parity = { \
            .vendor_role = "vendor", \
            .candidate_role = "candidate", \
            .vendor_sql = (source_sql), \
            .candidate_source_sql = (source_sql), \
            .expected_candidate_sql = (expected_sql), \
            .prepare = PLEX_PREPARE_V2, \
            .bind = bind_ondeck_values, \
            .bind_ctx = (binds_ptr), \
            .row_exception = accept_ondeck_legal_difference, \
            .row_exception_ctx = (contract_ptr), \
            .minimum_rows = 1 \
        } \
    }

static const rsh_case_spec plex_ondeck_main_cases[] = {
    PLEX_ICU_SQL_CASE_PAIR(
        "ondeck-positive", "candidate", ONDECK_SQL, ONDECK_SQL_REWRITTEN
    ),
    PLEX_ICU_SQL_CASE_PAIR(
        "ondeck-variant-a-byte-identity", "candidate", ONDECK_SQL,
        ONDECK_SQL_REWRITTEN
    ),
    PLEX_ONDECK_PARITY_CASE(
        "ondeck-id-list-contract", ONDECK_SQL, ONDECK_SQL_REWRITTEN,
        &plex_ondeck_bind_none, &plex_ondeck_two_row_contract
    ),
    {
        .label = "ondeck-param-section",
        .kind = RSH_CASE_FIXTURE,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .runtime_predicate = rsh_case_icu_available,
        .data.fixture = {
            .source_path =
                "tests/fixtures/plex-fts-rewrite/ondeck-bound-section.sql",
            .expected_path =
                "tests/fixtures/plex-fts-rewrite/ondeck-bound-section.expected.sql",
            .strip_final_lf = 1,
            .assertion_kind = RSH_FIXTURE_CONTRACT_PARITY,
            .contract_parity = {
                .vendor_role = "vendor",
                .candidate_role = "candidate",
                .prepare = PLEX_PREPARE_V2,
                .bind = bind_ondeck_values,
                .bind_ctx = &plex_ondeck_bind_section,
                .row_exception = accept_ondeck_legal_difference,
                .row_exception_ctx = &plex_ondeck_two_row_contract,
                .minimum_rows = 1
            }
        }
    },
    PLEX_ONDECK_PARITY_CASE(
        "ondeck-param-account", ONDECK_PARAM_ACCOUNT_SQL, NULL,
        &plex_ondeck_bind_account, &plex_ondeck_two_row_contract
    ),
    PLEX_ONDECK_PARITY_CASE(
        "ondeck-param-both", ONDECK_PARAM_BOTH_SQL, NULL,
        &plex_ondeck_bind_both, &plex_ondeck_two_row_contract
    ),
    PLEX_ONDECK_PARITY_CASE(
        "ondeck-param-reversed", ONDECK_PARAM_REVERSED_SQL, NULL,
        &plex_ondeck_bind_reversed, &plex_ondeck_two_row_contract
    ),
    PLEX_ONDECK_PARITY_CASE(
        "ondeck-param-hole", ONDECK_PARAM_HOLE_SQL, NULL,
        &plex_ondeck_bind_hole, &plex_ondeck_two_row_contract
    ),
    PLEX_ONDECK_PARITY_CASE(
        "ondeck-param-reuse-forward", ONDECK_PARAM_REUSE_FORWARD_SQL, NULL,
        &plex_ondeck_bind_reuse, &plex_ondeck_one_row_account_two_contract
    ),
    PLEX_ONDECK_PARITY_CASE(
        "ondeck-param-reuse-reverse", ONDECK_PARAM_REUSE_REVERSE_SQL, NULL,
        &plex_ondeck_bind_reuse, &plex_ondeck_one_row_account_two_contract
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-left-join", ONDECK_LEFT_JOIN_SQL,
        "left join metadata_items as grandparents"
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-param-list", ONDECK_PARAM_LIST_SQL,
        "grandparents.id in (101,?)"
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-param-leading-zero", ONDECK_PARAM_LEADING_ZERO_SQL,
        "library_section_id=?01"
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-duplicate-account", ONDECK_DUP_ACCOUNT_SQL,
        "metadata_item_views.account_id=42 and metadata_item_views.account_id=43"
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-missing-viewcount", ONDECK_MISSING_VIEWCOUNT_SQL,
        "grandparents.id in (" ONDECK_IDS ")  and metadata_item_views.account_id=42"
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-missing-settings-guid", ONDECK_MISSING_SETTINGS_GUID_SQL,
        "on metadata_item_views.account_id=metadata_item_settings.account_id "
        "join metadata_item_settings as grandparentsSettings"
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-missing-settings-account", ONDECK_MISSING_SETTINGS_ACCOUNT_SQL,
        "metadata_item_settings.guid=metadata_item_views.guid join"
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-projection-drift", ONDECK_PROJECTION_DRIFT_SQL,
        "max(metadata_item_views.viewed_at)"
    ),
    PLEX_SQL_CASE(
        "ondeck-non-target", "non-target", ONDECK_SQL, ONDECK_SQL
    )
};

static const rsh_case_spec plex_ondeck_missing_index_cases[] = {
    PLEX_NEGATIVE_CASE(
        "ondeck-missing-index", ONDECK_SQL,
        "grandparents.id in (" ONDECK_IDS ")"
    )
};

static const rsh_phase_spec plex_ondeck_phases[] = {
    {
        .label = "ondeck-main",
        .dbs = plex_ondeck_main_dbs,
        .db_count = sizeof(plex_ondeck_main_dbs) /
                    sizeof(plex_ondeck_main_dbs[0]),
        .cases = plex_ondeck_main_cases,
        .case_count = sizeof(plex_ondeck_main_cases) /
                      sizeof(plex_ondeck_main_cases[0])
    },
    {
        .label = "ondeck-missing-index",
        .dbs = plex_vendor_candidate_dbs,
        .db_count = sizeof(plex_vendor_candidate_dbs) /
                    sizeof(plex_vendor_candidate_dbs[0]),
        .cases = plex_ondeck_missing_index_cases,
        .case_count = sizeof(plex_ondeck_missing_index_cases) /
                      sizeof(plex_ondeck_missing_index_cases[0])
    }
};

static const rsh_case_spec plex_ondeck_threshold_main_cases[] = {
    PLEX_ICU_SQL_CASE_PAIR(
        "ondeck-threshold-exact", "candidate", ONDECK_THRESHOLD_SQL,
        ONDECK_THRESHOLD_SQL_REWRITTEN
    ),
    PLEX_ONDECK_PARITY_CASE(
        "ondeck-threshold-contract", ONDECK_THRESHOLD_SQL,
        ONDECK_THRESHOLD_SQL_REWRITTEN, &plex_ondeck_bind_none,
        &plex_ondeck_threshold_contract
    ),
    PLEX_ONDECK_PARITY_CASE(
        "ondeck-threshold-inline", ONDECK_THRESHOLD_SQL, NULL,
        &plex_ondeck_bind_none, &plex_ondeck_threshold_contract
    ),
    PLEX_ONDECK_PARITY_CASE(
        "ondeck-threshold-param-section", ONDECK_THRESHOLD_PARAM_SECTION_SQL,
        NULL, &plex_ondeck_bind_section, &plex_ondeck_threshold_contract
    ),
    PLEX_ONDECK_PARITY_CASE(
        "ondeck-threshold-param-account", ONDECK_THRESHOLD_PARAM_ACCOUNT_SQL,
        NULL, &plex_ondeck_bind_account, &plex_ondeck_threshold_contract
    ),
    PLEX_ONDECK_PARITY_CASE(
        "ondeck-threshold-param-both", ONDECK_THRESHOLD_PARAM_BOTH_SQL,
        NULL, &plex_ondeck_bind_both, &plex_ondeck_threshold_contract
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-threshold-cross-product", ONDECK_THRESHOLD_CROSS_PRODUCT_SQL,
        ONDECK_AFTER_THRESHOLD ONDECK_THRESHOLD
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-threshold-bind", ONDECK_THRESHOLD_BIND_SQL,
        ONDECK_AFTER_THRESHOLD "?"
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-threshold-named", ONDECK_THRESHOLD_NAMED_SQL, ":threshold"
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-threshold-positive-sign", ONDECK_THRESHOLD_POSITIVE_SIGN_SQL,
        "+1500"
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-threshold-negative-sign", ONDECK_THRESHOLD_NEGATIVE_SIGN_SQL,
        "-1500"
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-threshold-decimal", ONDECK_THRESHOLD_DECIMAL_SQL, "1500.0"
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-threshold-expression", ONDECK_THRESHOLD_EXPRESSION_SQL,
        "1500+0"
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-threshold-overflow", ONDECK_THRESHOLD_OVERFLOW_SQL,
        "9223372036854775808"
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-threshold-no-selector", ONDECK_NO_SELECTOR_SQL,
        ONDECK_AFTER_IDS
    ),
    PLEX_NEGATIVE_CASE(
        "ondeck-threshold-wrong-tail", ONDECK_THRESHOLD_WRONG_TAIL_SQL,
        "order by grandparents.id"
    )
};

static const rsh_case_spec plex_ondeck_threshold_missing_index_cases[] = {
    PLEX_NEGATIVE_CASE(
        "ondeck-threshold-missing-index", ONDECK_THRESHOLD_SQL,
        ONDECK_AFTER_THRESHOLD ONDECK_THRESHOLD
    )
};

static const rsh_phase_spec plex_ondeck_threshold_phases[] = {
    {
        .label = "ondeck-threshold-main",
        .dbs = plex_ondeck_main_dbs,
        .db_count = sizeof(plex_ondeck_main_dbs) /
                    sizeof(plex_ondeck_main_dbs[0]),
        .cases = plex_ondeck_threshold_main_cases,
        .case_count = sizeof(plex_ondeck_threshold_main_cases) /
                      sizeof(plex_ondeck_threshold_main_cases[0])
    },
    {
        .label = "ondeck-threshold-missing-index",
        .dbs = plex_vendor_candidate_dbs,
        .db_count = sizeof(plex_vendor_candidate_dbs) /
                    sizeof(plex_vendor_candidate_dbs[0]),
        .cases = plex_ondeck_threshold_missing_index_cases,
        .case_count = sizeof(plex_ondeck_threshold_missing_index_cases) /
                      sizeof(plex_ondeck_threshold_missing_index_cases[0])
    }
};

#undef PLEX_ONDECK_PARITY_CASE
#undef DEFINE_PLEX_SCALAR_PHASE
#undef PLEX_ICU_NEGATIVE_CASE
#undef PLEX_NEGATIVE_CASE
#undef PLEX_ICU_LEGACY_SQL_CASE_PAIR
#undef PLEX_ICU_SQL_CASE_PAIR
#undef PLEX_SQL_CASE
#undef PLEX_PREPARE_V2

static const rsh_env_assignment plex_negative_control_env[] = {
    {
        .name = "SQLITE3_DISABLE_AUTOPRAGMA",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_RUNTIME_OPTIMIZE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_OBSERVABILITY",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_STMT_TRACE_SAMPLING",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_REWRITE_APPLIED_SQL",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_FTS_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "0"}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_GUID_LIKE_REWRITE",
        .value = {.state = RSH_ENV_UNSET}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_TAGGINGS_REWRITE",
        .value = {.state = RSH_ENV_VALUE, .value = "1"}
    },
    {
        .name = "SQLITE3_DISABLE_PLEX_ONDECK_REWRITE",
        .value = {.state = RSH_ENV_UNSET}
    }
};

static const rsh_db_spec plex_negative_control_dbs[] = {
    {
        .role = "vendor",
        .relative_path = "not-target.db",
        .kind = RSH_DB_VENDOR,
        .storage = RSH_DB_RELATIVE
    },
    {
        .role = "candidate",
        .relative_path = "com.plexapp.plugins.library.db",
        .kind = RSH_DB_CANDIDATE,
        .storage = RSH_DB_RELATIVE
    }
};

static const char plex_vendor_prepare_control_sql[] =
    "SELECT 'plex-negative-control-vendor-prepare' FROM";

static const rsh_case_spec plex_vendor_prepare_control_cases[] = {
    {
        .label = "negative-control-vendor-prepare",
        .kind = RSH_CASE_EXPECT_ABORT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.expect_abort.negative = {
            .source_kind = RSH_NEGATIVE_STATIC,
            .sql = plex_vendor_prepare_control_sql,
            .discriminating_needle = "plex-negative-control-vendor-prepare",
            .prepare = {
                .kind = RSH_PREPARE_V2,
                .nbyte_kind = RSH_NBYTE_MINUS_ONE,
                .tail_kind = RSH_TAIL_FULL
            },
            .vendor_role = "vendor",
            .candidate_role = "candidate"
        }
    }
};

static const rsh_case_spec plex_matcher_miss_control_cases[] = {
    {
        .label = "negative-control-matcher-miss",
        .kind = RSH_CASE_EXPECT_ABORT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .data.expect_abort.negative = {
            .source_kind = RSH_NEGATIVE_STATIC,
            .sql = MATCH_SQL_INT,
            .discriminating_needle = " and tag_type=6  and ",
            .prepare = {
                .kind = RSH_PREPARE_V2,
                .nbyte_kind = RSH_NBYTE_MINUS_ONE,
                .tail_kind = RSH_TAIL_FULL
            },
            .vendor_role = "vendor",
            .candidate_role = "candidate"
        }
    }
};

static const rsh_phase_spec plex_vendor_prepare_control_phases[] = {
    {
        .label = "negative-control-vendor-prepare",
        .dbs = plex_negative_control_dbs,
        .db_count = sizeof(plex_negative_control_dbs) /
                    sizeof(plex_negative_control_dbs[0]),
        .cases = plex_vendor_prepare_control_cases,
        .case_count = sizeof(plex_vendor_prepare_control_cases) /
                      sizeof(plex_vendor_prepare_control_cases[0])
    }
};

static const rsh_phase_spec plex_matcher_miss_control_phases[] = {
    {
        .label = "negative-control-matcher-miss",
        .dbs = plex_negative_control_dbs,
        .db_count = sizeof(plex_negative_control_dbs) /
                    sizeof(plex_negative_control_dbs[0]),
        .cases = plex_matcher_miss_control_cases,
        .case_count = sizeof(plex_matcher_miss_control_cases) /
                      sizeof(plex_matcher_miss_control_cases[0])
    }
};

static const char *const plex_vendor_prepare_earlier_labels[] = {
    "FAIL [negative-control-vendor-prepare/needle-unique]"
};

static const rsh_abort_expectation plex_vendor_prepare_abort_expectation = {
    .expected_exit = 1,
    .expected_stage_label =
        "FAIL [negative-control-vendor-prepare/vendor-prepare]",
    .earlier_stage_labels = plex_vendor_prepare_earlier_labels,
    .earlier_stage_label_count = sizeof(plex_vendor_prepare_earlier_labels) /
                                 sizeof(plex_vendor_prepare_earlier_labels[0])
};

static const char *const plex_matcher_miss_earlier_labels[] = {
    "FAIL [negative-control-matcher-miss/needle-unique]",
    "FAIL [negative-control-matcher-miss/vendor-prepare]",
    "FAIL [negative-control-matcher-miss/vendor-sql]",
    "FAIL [negative-control-matcher-miss/vendor-tail]",
    "FAIL [negative-control-matcher-miss/vendor-finalize]",
    "FAIL [negative-control-matcher-miss/matcher-prepare]"
};

static const rsh_abort_expectation plex_matcher_miss_abort_expectation = {
    .expected_exit = 1,
    .expected_stage_label =
        "FAIL [negative-control-matcher-miss/matcher-miss]",
    .earlier_stage_labels = plex_matcher_miss_earlier_labels,
    .earlier_stage_label_count = sizeof(plex_matcher_miss_earlier_labels) /
                                 sizeof(plex_matcher_miss_earlier_labels[0])
};

static const rsh_suite_spec plex_suite_spec = {
    .suite_name = "plex fts rewrite smoke passed",
    .temp_prefix = "/tmp/plex-fts-rewrite-smoke",
    .vendor_basename = "not-target.db",
    .target_basename = "com.plexapp.plugins.library.db",
    .controlled_env_gates = plex_controlled_env_gates,
    .controlled_env_gate_count =
        sizeof(plex_controlled_env_gates) / sizeof(plex_controlled_env_gates[0]),
    .prepare = rsh_public_prepare,
    .open = rsh_open,
    .base_seed = rsh_base_seed,
    .resolve_setup_profile = rsh_resolve_setup_profile,
    .failure = failf
};

static int rsh_icu_available(const rsh_run_spec *run, void *suite_ctx) {
    (void)run;
    (void)suite_ctx;
    return sqlite3_compileoption_used("ENABLE_ICU") != 0;
}

#define NATIVE_RUN(dispatch, env, phase_rows) \
    { \
        .dispatch_name = (dispatch), \
        .pass_label = (dispatch), \
        .process_kind = RSH_PROCESS_FORK, \
        .outcome = RSH_OUTCOME_SUCCESS, \
        .build_mask = RSH_BUILD_PLEX_LINKED, \
        .preload_env = (env), \
        .preload_env_count = sizeof(env) / sizeof((env)[0]), \
        .capture_scope = RSH_CAPTURE_NONE, \
        .phases = (phase_rows), \
        .phase_count = sizeof(phase_rows) / sizeof((phase_rows)[0]) \
    }

#define NATIVE_EXPECT_RUN( \
    dispatch, env, phase_rows, detail_value, else_literal_value, predicate_value) \
    { \
        .dispatch_name = (dispatch), \
        .pass_label = (dispatch), \
        .pass_detail = { \
            .literal = (detail_value), \
            .else_literal = (else_literal_value), \
            .predicate = (predicate_value) \
        }, \
        .process_kind = RSH_PROCESS_FORK, \
        .outcome = RSH_OUTCOME_SUCCESS, \
        .build_mask = RSH_BUILD_PLEX_LINKED, \
        .preload_env = (env), \
        .preload_env_count = sizeof(env) / sizeof((env)[0]), \
        .capture_scope = RSH_CAPTURE_NONE, \
        .phases = (phase_rows), \
        .phase_count = sizeof(phase_rows) / sizeof((phase_rows)[0]) \
    }

#define NATIVE_EXEC_LOG_RUN(dispatch, env, phase_rows, post_close_rows) \
    { \
        .dispatch_name = (dispatch), \
        .pass_label = (dispatch), \
        .skip_detail = "ENABLE_ICU=0", \
        .process_kind = RSH_PROCESS_EXEC, \
        .outcome = RSH_OUTCOME_SUCCESS, \
        .build_mask = RSH_BUILD_PLEX_LINKED, \
        .preload_env = (env), \
        .preload_env_count = sizeof(env) / sizeof((env)[0]), \
        .capture_scope = RSH_CAPTURE_STDERR, \
        .phases = (phase_rows), \
        .phase_count = sizeof(phase_rows) / sizeof((phase_rows)[0]), \
        .post_close_cases = (post_close_rows), \
        .post_close_case_count = \
            sizeof(post_close_rows) / sizeof((post_close_rows)[0]), \
        .runtime_predicate = rsh_icu_available, \
    }

static const rsh_run_spec plex_runs[] = {
    NATIVE_EXPECT_RUN(
        "positive", plex_fts_enabled_env, plex_positive_phases,
        "expect_rewrite=1", "expect_rewrite=0", rsh_icu_available
    ),
    NATIVE_EXPECT_RUN(
        "env-default", plex_fts_default_env, plex_env_default_phases,
        "expect_rewrite=1", "expect_rewrite=0", rsh_icu_available
    ),
    NATIVE_EXPECT_RUN(
        "env-one-disabled", plex_fts_disabled_env,
        plex_env_one_disabled_phases, "expect_rewrite=0", NULL, NULL
    ),
    NATIVE_EXPECT_RUN(
        "env-garbage-enabled", plex_fts_garbage_env,
        plex_env_garbage_enabled_phases,
        "expect_rewrite=1", "expect_rewrite=0", rsh_icu_available
    ),
    NATIVE_RUN(
        "path-negative", plex_fts_enabled_env, plex_path_negative_phases
    ),
    NATIVE_RUN("nonmatch", plex_fts_enabled_env, plex_nonmatch_phases),
    NATIVE_RUN("guid-like", plex_guid_like_enabled_env, plex_guid_like_phases),
    NATIVE_RUN(
        "guid-like-env-default", plex_all_rewrites_disabled_env,
        plex_guid_like_env_default_phases
    ),
    NATIVE_RUN(
        "guid-like-env-one-disabled", plex_guid_like_disabled_env,
        plex_guid_like_env_one_disabled_phases
    ),
    NATIVE_RUN(
        "guid-like-env-garbage-disabled", plex_guid_like_garbage_env,
        plex_guid_like_env_garbage_disabled_phases
    ),
    NATIVE_EXPECT_RUN(
        "tag-membership", plex_tag_enabled_env, plex_tag_membership_phases,
        "expect_rewrite=1", "expect_rewrite=0", rsh_icu_available
    ),
    NATIVE_EXPECT_RUN(
        "tag-row-parity", plex_tag_enabled_env, plex_tag_row_parity_phases,
        "expect_rewrite=1", "expect_rewrite=0", rsh_icu_available
    ),
    NATIVE_EXPECT_RUN(
        "tag-env-default", plex_tag_default_env, plex_tag_env_default_phases,
        "expect_rewrite=1", "expect_rewrite=0", rsh_icu_available
    ),
    NATIVE_EXPECT_RUN(
        "tag-env-zero-enabled", plex_tag_enabled_env,
        plex_tag_env_zero_enabled_phases,
        "expect_rewrite=1", "expect_rewrite=0", rsh_icu_available
    ),
    NATIVE_EXPECT_RUN(
        "tag-env-one-disabled", plex_all_rewrites_disabled_env,
        plex_tag_env_one_disabled_phases,
        "expect_rewrite=0", NULL, NULL
    ),
    NATIVE_EXPECT_RUN(
        "tag-env-garbage-enabled", plex_tag_garbage_env,
        plex_tag_env_garbage_enabled_phases,
        "expect_rewrite=1", "expect_rewrite=0", rsh_icu_available
    ),
    NATIVE_EXPECT_RUN(
        "ondeck", plex_ondeck_enabled_env, plex_ondeck_phases,
        "expect_rewrite=1", "expect_rewrite=0", rsh_icu_available
    ),
    NATIVE_EXPECT_RUN(
        "ondeck-threshold", plex_ondeck_enabled_env,
        plex_ondeck_threshold_phases,
        "expect_rewrite=1", "expect_rewrite=0", rsh_icu_available
    ),
    NATIVE_RUN(
        "ondeck-threshold-fail-open", plex_ondeck_enabled_env,
        plex_ondeck_threshold_fail_open_phases
    ),
    NATIVE_EXPECT_RUN(
        "ondeck-env-default", plex_ondeck_default_env,
        plex_ondeck_env_default_phases,
        "expect_rewrite=0", NULL, NULL
    ),
    NATIVE_EXPECT_RUN(
        "ondeck-env-one-disabled", plex_all_rewrites_disabled_env,
        plex_ondeck_env_one_disabled_phases,
        "expect_rewrite=0", NULL, NULL
    ),
    NATIVE_EXPECT_RUN(
        "ondeck-env-garbage-disabled", plex_ondeck_garbage_env,
        plex_ondeck_env_garbage_disabled_phases,
        "expect_rewrite=0", NULL, NULL
    ),
    NATIVE_EXEC_LOG_RUN(
        "skip-log", plex_exec_fts_env, plex_skip_log_phases,
        plex_skip_log_post_close_cases
    ),
    NATIVE_EXEC_LOG_RUN(
        "guid-like-applied-sampling", plex_exec_guid_like_env,
        plex_guid_like_sampling_phases,
        plex_guid_like_sampling_post_close_cases
    ),
    NATIVE_EXEC_LOG_RUN(
        "tag-applied-log", plex_exec_tag_visible_env,
        plex_tag_visible_phases, plex_tag_visible_post_close_cases
    ),
    NATIVE_EXEC_LOG_RUN(
        "tag-applied-sql-suppressed", plex_exec_tag_suppressed_env,
        plex_tag_suppressed_phases,
        plex_tag_suppressed_post_close_cases
    ),
    NATIVE_EXEC_LOG_RUN(
        "tag-index-log-dedup", plex_exec_tag_visible_env,
        plex_tag_index_log_phases, plex_tag_index_log_post_close_cases
    ),
    NATIVE_EXEC_LOG_RUN(
        "ondeck-capture-miss-log", plex_exec_ondeck_env,
        plex_ondeck_capture_miss_phases,
        plex_ondeck_capture_miss_post_close_cases
    ),
    {
        .dispatch_name = "negative-control-vendor-prepare",
        .pass_label = "negative-control-vendor-prepare",
        .process_kind = RSH_PROCESS_FORK,
        .outcome = RSH_OUTCOME_EXPECT_ABORT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .preload_env = plex_negative_control_env,
        .preload_env_count = sizeof(plex_negative_control_env) /
                             sizeof(plex_negative_control_env[0]),
        .capture_scope = RSH_CAPTURE_NONE,
        .phases = plex_vendor_prepare_control_phases,
        .phase_count = sizeof(plex_vendor_prepare_control_phases) /
                       sizeof(plex_vendor_prepare_control_phases[0]),
        .abort_expectation = &plex_vendor_prepare_abort_expectation
    },
    {
        .dispatch_name = "negative-control-matcher-miss",
        .pass_label = "negative-control-matcher-miss",
        .skip_detail = "ENABLE_ICU=0",
        .process_kind = RSH_PROCESS_EXEC,
        .outcome = RSH_OUTCOME_EXPECT_ABORT,
        .build_mask = RSH_BUILD_PLEX_LINKED,
        .preload_env = plex_negative_control_env,
        .preload_env_count = sizeof(plex_negative_control_env) /
                             sizeof(plex_negative_control_env[0]),
        .capture_scope = RSH_CAPTURE_NONE,
        .phases = plex_matcher_miss_control_phases,
        .phase_count = sizeof(plex_matcher_miss_control_phases) /
                       sizeof(plex_matcher_miss_control_phases[0]),
        .abort_expectation = &plex_matcher_miss_abort_expectation,
        .runtime_predicate = rsh_icu_available
    }
};

#undef NATIVE_EXEC_LOG_RUN
#undef NATIVE_EXPECT_RUN
#undef NATIVE_RUN

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--child") == 0) {
        return rsh_run_child(
            &plex_suite_spec, plex_runs,
            sizeof(plex_runs) / sizeof(plex_runs[0]),
            RSH_BUILD_PLEX_LINKED, argv[2]
        );
    }
    if (argc != 1) failf("FATAL: unexpected arguments");
    return rsh_run_all(
        &plex_suite_spec, plex_runs,
        sizeof(plex_runs) / sizeof(plex_runs[0]), argv[0],
        RSH_BUILD_PLEX_LINKED
    );
}
