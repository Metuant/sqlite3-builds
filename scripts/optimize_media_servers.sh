#!/usr/bin/env bash

export SQLITE3_DISABLE_AUTOPRAGMA=1
export SQLITE3_DISABLE_OBSERVABILITY=1

# Maintenance assumes planned downtime. For each configured instance with a
# present database directory, the script stops the container, verifies it is
# stopped before database work, starts it afterward, and verifies it reaches
# running.
# NOTE: Jellyfin maintenance is dormant in this cycle.
# WHY: JF maintenance stays absent until schema, page-size, FTS, and stopped-container
# gates are re-validated.
# This script intentionally covers only Plex and Emby.
[ -n "${_PAGE_SIZE+x}" ] || _PAGE_SIZE="16384"
readonly _PAGE_SIZE
# Only runbook-validated indexes belong here; adding one is a future spec
# decision and requires planner adoption, measured benefit, and landmine review.
if [ -z "${_EMBY_INDEXES+x}" ]; then
    _EMBY_INDEXES=(
        "CREATE INDEX IF NOT EXISTS idx_dshadow_mediaitems_parent_type ON MediaItems (ParentId, Type);"
        "CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_gk_dc ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) WHERE Type = 8;"
        "CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_episodes_dcn_gk ON MediaItems ((DateCreated IS NULL), DateCreated DESC, coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), Id, UserDataKeyId) WHERE Type = 8;"
        "CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_movies_dcn_puk ON MediaItems ((DateCreated IS NULL), DateCreated DESC, PresentationUniqueKey, Id, UserDataKeyId) WHERE Type = 5;"
        "CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_movies_puk_dc_cover ON MediaItems (PresentationUniqueKey, DateCreated, Id, UserDataKeyId) WHERE Type = 5;"
        "CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_mixed_dcn_gk ON MediaItems ((DateCreated IS NULL), DateCreated DESC, coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), Id, UserDataKeyId) WHERE Type IN (8,5);"
        "CREATE INDEX IF NOT EXISTS idx_dshadow_emby_latest_mixed_gk_dc ON MediaItems (coalesce(SeriesPresentationUniqueKey, PresentationUniqueKey), DateCreated DESC, Id, UserDataKeyId) WHERE Type IN (8,5);"
    )
fi
readonly -a _EMBY_INDEXES
if [ -z "${_PLEX_INDEXES+x}" ]; then
    _PLEX_INDEXES=(
        "CREATE INDEX IF NOT EXISTS idx_dshadow_taggings_tag_id_metadata_item_id ON taggings (tag_id, metadata_item_id);"
        "CREATE INDEX IF NOT EXISTS idx_dshadow_mis_account_updated_guid_cover ON metadata_item_settings (account_id, updated_at DESC, guid, view_offset, last_viewed_at);"
        "CREATE INDEX IF NOT EXISTS idx_dshadow_metadata_items_section_added ON metadata_items (library_section_id, added_at);"
        "CREATE INDEX IF NOT EXISTS idx_dshadow_metadata_items_guid_nocase ON metadata_items (guid COLLATE NOCASE);"
        "CREATE INDEX IF NOT EXISTS idx_dshadow_metadata_item_views_account_grandparent_guid ON metadata_item_views (account_id, grandparent_guid);"
        "CREATE INDEX IF NOT EXISTS idx_dshadow_metadata_items_section_id_type ON metadata_items (library_section_id, id, metadata_type);"
    )
fi
readonly -a _PLEX_INDEXES
if [ -z "${_PLEX_STAT4_LEADER_INDEXES+x}" ]; then
    _PLEX_STAT4_LEADER_INDEXES=(
        "idx_dshadow_taggings_tag_id_metadata_item_id"
        "idx_dshadow_mis_account_updated_guid_cover"
        "idx_dshadow_metadata_items_section_added"
        "idx_dshadow_metadata_items_guid_nocase"
        "idx_dshadow_metadata_item_views_account_grandparent_guid"
        "idx_dshadow_metadata_items_section_id_type"
    )
fi
readonly -a _PLEX_STAT4_LEADER_INDEXES

[ -n "${_PLEX_DB+x}" ] || _PLEX_DB="com.plexapp.plugins.library.db"
readonly _PLEX_DB
[ -n "${_PLEX_BLOB_DB+x}" ] || _PLEX_BLOB_DB="com.plexapp.plugins.library.blobs.db"
readonly _PLEX_BLOB_DB
[ -n "${_EMBY_DB+x}" ] || _EMBY_DB="library.db"
readonly _EMBY_DB

build_emby_optimize_sql() {
    local emby_index_sql=""
    local ddl

    for ddl in "${_EMBY_INDEXES[@]}"; do
        emby_index_sql+="${ddl} "
    done
    printf '%s' "PRAGMA cache_size=-1048576; PRAGMA temp_store=2; PRAGMA threads=8; ${emby_index_sql}REINDEX; PRAGMA analysis_limit=0; ANALYZE; PRAGMA optimize=0x10002;"
}

build_plex_optimize_sql() {
    local plex_index_sql=""
    local ddl

    for ddl in "${_PLEX_INDEXES[@]}"; do
        plex_index_sql+="${ddl} "
    done
    printf '%s' "PRAGMA cache_size=-1048576; PRAGMA temp_store=2; PRAGMA threads=8; ${plex_index_sql}REINDEX; PRAGMA analysis_limit=0; ANALYZE; PRAGMA optimize=0x10002;"
}

cleanup_staged_db() {
    local staged_db="${1}"

    rm -f "${staged_db}" "${staged_db}-wal" "${staged_db}-shm"
}

quote_sql_ident() {
    local ident="${1}"

    printf '"%s"' "${ident//\"/\"\"}"
}

sqlite_source_id_pin_value() {
    local script_dir
    local pin_file
    local encoded_source_id

    script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
    pin_file="${script_dir}/../pins/versions.env"
    if ! encoded_source_id="$(awk -F= '
        $1 == "SQLITE_SOURCE_ID" {
            value = substr($0, index($0, "=") + 1)
            count++
        }
        END {
            if (count == 1 && value != "") print value
            else exit 1
        }
    ' "${pin_file}" 2>/dev/null)"; then
        echo "ERROR: canonical SQLite source-id pin missing or ambiguous: ${pin_file}" >&2
        return 1
    fi
    if [[ ! "${encoded_source_id}" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}%20[0-9]{2}:[0-9]{2}:[0-9]{2}%20[0-9a-f]{64}$ ]]; then
        echo "ERROR: canonical SQLite source-id pin is malformed: ${pin_file}" >&2
        return 1
    fi
    printf '%s\n' "${encoded_source_id//%20/ }"
}

plex_patched_engine_preflight() {
    local binary="${1}"
    local expected_source_id
    local source_id

    if ! expected_source_id="$(sqlite_source_id_pin_value)"; then
        return 1
    fi

    if ! source_id=$("${binary}" ":memory:" "SELECT sqlite_source_id();" 2>&1); then
        echo "ERROR: patched Plex maintenance engine pre-flight failed for ${binary}" >&2
        echo "ERROR: pre-flight output: ${source_id}" >&2
        return 1
    fi
    source_id="${source_id//$'\r'/}"
    if [ "${source_id}" != "${expected_source_id}" ]; then
        echo "ERROR: ${binary} sqlite_source_id() mismatch" >&2
        echo "ERROR: expected: ${expected_source_id}" >&2
        echo "ERROR: observed: ${source_id}" >&2
        return 1
    fi

    return 0
}

plex_stat4_preflight() {
    local binary="${1}"
    local probe_out
    local stat4_enabled

    if ! probe_out=$("${binary}" ":memory:" "SELECT 1;" 2>&1); then
        echo "WARNING: Plex STAT4 pre-flight failed for ${binary}; skipping Plex STAT4 pass" >&2
        echo "WARNING: pre-flight output: ${probe_out}" >&2
        return 1
    fi
    if ! stat4_enabled=$("${binary}" ":memory:" "SELECT sqlite_compileoption_used('ENABLE_STAT4');" 2>&1 | tr -d '\r\n'); then
        echo "WARNING: Plex STAT4 ENABLE_STAT4 probe failed for ${binary}; skipping Plex STAT4 pass" >&2
        echo "WARNING: pre-flight output: ${stat4_enabled}" >&2
        return 1
    fi
    if [ "${stat4_enabled}" != "1" ]; then
        echo "WARNING: Plex STAT4 binary ${binary} does not report ENABLE_STAT4; skipping Plex STAT4 pass" >&2
        return 1
    fi

    return 0
}

discover_plex_stat4_analyze_targets() {
    local binary="${1}"
    local db_path="${2}"
    local field_sep=$'\t'
    local query

    query="WITH RECURSIVE ctl(n) AS (VALUES(1) UNION ALL SELECT n + 1 FROM ctl WHERE n < 31), ordinary_tables AS (SELECT s.name FROM sqlite_master AS s WHERE s.type = 'table' AND s.sql IS NOT NULL AND s.name NOT GLOB 'sqlite_*' AND NOT EXISTS (SELECT 1 FROM ctl WHERE instr(s.name, char(n)) > 0) AND lower(ltrim(replace(replace(replace(s.sql, char(9), ' '), char(10), ' '), char(13), ' '))) NOT LIKE 'create virtual table%'), table_indexes AS (SELECT t.name AS table_name, il.name AS index_name, il.origin AS origin FROM ordinary_tables AS t, pragma_index_list(t.name) AS il), index_safety AS (SELECT ti.table_name, ti.index_name, ti.origin, max(CASE WHEN ix.key = 1 AND upper(coalesce(ix.coll, 'BINARY')) NOT IN ('BINARY', 'NOCASE', 'RTRIM', 'ICU_ROOT') THEN 1 ELSE 0 END) AS has_unsafe_collation, max(CASE WHEN ix.key = 1 AND upper(coalesce(ix.coll, 'BINARY')) = 'ICU_ROOT' THEN 1 ELSE 0 END) AS has_icu_root_collation FROM table_indexes AS ti, pragma_index_xinfo(ti.index_name) AS ix GROUP BY ti.table_name, ti.index_name, ti.origin), safe_tables AS (SELECT t.name AS target_name FROM ordinary_tables AS t WHERE NOT EXISTS (SELECT 1 FROM index_safety AS s WHERE s.table_name = t.name AND (s.has_unsafe_collation = 1 OR s.has_icu_root_collation = 1))), safe_indexes AS (SELECT s.index_name AS target_name FROM index_safety AS s JOIN sqlite_master AS schema_idx ON schema_idx.type = 'index' AND schema_idx.name = s.index_name WHERE s.has_unsafe_collation = 0 AND s.origin = 'c' AND s.index_name NOT GLOB 'sqlite_*' AND NOT EXISTS (SELECT 1 FROM ctl WHERE instr(s.index_name, char(n)) > 0) AND EXISTS (SELECT 1 FROM index_safety AS split WHERE split.table_name = s.table_name AND (split.has_unsafe_collation = 1 OR split.has_icu_root_collation = 1))) SELECT target_kind, target_name FROM (SELECT 'T' AS target_kind, target_name FROM safe_tables UNION ALL SELECT 'I' AS target_kind, target_name FROM safe_indexes) ORDER BY target_kind DESC, target_name;"

    "${binary}" -batch -noheader -separator "${field_sep}" "${db_path}" "${query}"
}

run_plex_stat4_analyze() {
    local binary="${1}"
    local db_path="${2}"
    local field_sep=$'\t'
    local targets
    local row
    local target_kind
    local target_name
    local extra
    local target_ident
    local analyze_sql
    local target_total=0
    local analyzed_total=0
    local failed_total=0
    local stat4_exists
    local stat4_rows
    local leader_index
    local leader_rows

    if ! targets="$(discover_plex_stat4_analyze_targets "${binary}" "${db_path}")"; then
        echo "WARNING: Plex STAT4 worklist discovery failed for ${db_path}; skipping Plex STAT4 pass" >&2
        return 0
    fi

    while IFS= read -r row
    do
        [ -n "${row}" ] || continue
        target_total=$((target_total + 1))
    done <<< "${targets}"

    echo "Plex STAT4 analyze targets: ${target_total} for ${db_path}"
    if [ "${target_total}" = "0" ]; then
        echo "WARNING: Plex STAT4 worklist is empty for ${db_path}; skipping Plex STAT4 pass" >&2
        return 0
    fi

    while IFS= read -r row
    do
        [ -n "${row}" ] || continue
        IFS="${field_sep}" read -r target_kind target_name extra <<< "${row}"
        if [ -n "${extra}" ] || [ -z "${target_name}" ] || has_control_chars "${target_name}"; then
            echo "WARNING: unsafe Plex STAT4 worklist row for ${db_path}; skipping one target" >&2
            failed_total=$((failed_total + 1))
            continue
        fi
        case "${target_kind}" in
            T|I)
                ;;
            *)
                echo "WARNING: unknown Plex STAT4 worklist target kind '${target_kind}' for ${db_path}; skipping ${target_name}" >&2
                failed_total=$((failed_total + 1))
                continue
                ;;
        esac

        target_ident="$(quote_sql_ident "${target_name}")"
        analyze_sql="PRAGMA cache_size=-1048576; PRAGMA temp_store=2; PRAGMA threads=8; PRAGMA analysis_limit=0; ANALYZE ${target_ident};"
        if "${binary}" "${db_path}" "${analyze_sql}"; then
            analyzed_total=$((analyzed_total + 1))
        else
            echo "WARNING: Plex STAT4 ANALYZE failed for ${target_name} in ${db_path}; continuing" >&2
            failed_total=$((failed_total + 1))
        fi
    done <<< "${targets}"

    echo "Plex STAT4 analyze complete: analyzed=${analyzed_total} failed=${failed_total} for ${db_path}"

    if ! stat4_exists=$("${binary}" "${db_path}" "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'sqlite_stat4';" | tr -d '\r\n'); then
        echo "WARNING: Plex STAT4 final sqlite_stat4 existence check failed for ${db_path}; continuing" >&2
        return 0
    fi
    if [ "${stat4_exists}" != "1" ]; then
        echo "WARNING: Plex STAT4 did not create sqlite_stat4 for ${db_path}; continuing" >&2
        return 0
    fi
    if ! stat4_rows=$("${binary}" "${db_path}" "SELECT COUNT(*) FROM sqlite_stat4;" | tr -d '\r\n'); then
        echo "WARNING: Plex STAT4 final row-count check failed for ${db_path}; continuing" >&2
        return 0
    fi
    case "${stat4_rows}" in
        ''|*[!0-9]*)
            echo "WARNING: Plex STAT4 final row count was not numeric for ${db_path}: ${stat4_rows}; continuing" >&2
            return 0
            ;;
    esac
    if [ "${stat4_rows}" -gt 0 ]; then
        echo "Plex STAT4 sqlite_stat4 rows: ${stat4_rows} for ${db_path}"
    else
        echo "WARNING: Plex STAT4 produced zero sqlite_stat4 rows for ${db_path}; continuing" >&2
    fi

    for leader_index in "${_PLEX_STAT4_LEADER_INDEXES[@]}"; do
        if ! leader_rows=$("${binary}" "${db_path}" "SELECT COUNT(*) FROM sqlite_stat4 WHERE idx = '${leader_index}';" | tr -d '\r\n'); then
            echo "WARNING: Plex STAT4 leader index check failed for ${leader_index} in ${db_path}; continuing" >&2
            continue
        fi
        case "${leader_rows}" in
            ''|*[!0-9]*)
                echo "WARNING: Plex STAT4 leader index row count was not numeric for ${leader_index} in ${db_path}: ${leader_rows}; continuing" >&2
                continue
                ;;
        esac
        if [ "${leader_rows}" -gt 0 ]; then
            echo "Plex STAT4 ${leader_index} rows: ${leader_rows}"
        else
            echo "WARNING: Plex STAT4 produced zero rows for ${leader_index} in ${db_path}; continuing" >&2
        fi
    done

    return 0
}

has_control_chars() {
    local value="${1}"

    case "${value}" in
        *[[:cntrl:]]*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

trim_sql_ws() {
    local value="${1}"
    local last_index

    while [ -n "${value}" ]; do
        case "${value:0:1}" in
            " "|$'\t'|$'\r'|$'\n')
                value="${value:1}"
                ;;
            *)
                break
                ;;
        esac
    done
    while [ -n "${value}" ]; do
        last_index=$((${#value} - 1))
        case "${value:${last_index}:1}" in
            " "|$'\t'|$'\r'|$'\n')
                value="${value:0:${last_index}}"
                ;;
            *)
                break
                ;;
        esac
    done
    printf '%s' "${value}"
}

extract_parenthesized_content() {
    local value="${1}"
    local depth=1
    local quote=""
    local out=""
    local char
    local next_char
    local i

    for ((i = 0; i < ${#value}; i++)); do
        char="${value:i:1}"
        if [ -n "${quote}" ]; then
            out="${out}${char}"
            if [ "${char}" = "${quote}" ]; then
                next_char="${value:i+1:1}"
                if [ "${next_char}" = "${quote}" ]; then
                    out="${out}${next_char}"
                    i=$((i + 1))
                else
                    quote=""
                fi
            fi
            continue
        fi

        case "${char}" in
            "'")
                quote="'"
                out="${out}${char}"
                ;;
            '"')
                quote='"'
                out="${out}${char}"
                ;;
            "(")
                depth=$((depth + 1))
                out="${out}${char}"
                ;;
            ")")
                depth=$((depth - 1))
                if [ "${depth}" = "0" ]; then
                    printf '%s' "${out}"
                    return 0
                fi
                out="${out}${char}"
                ;;
            *)
                out="${out}${char}"
                ;;
        esac
    done

    return 1
}

classify_fts5_option_content() {
    local option
    local key
    local value
    local char
    local quote=""
    local depth=0
    local eq_index=-1
    local next_char
    local i

    option="$(trim_sql_ws "${1}")"
    [ -n "${option}" ] || return 0

    for ((i = 0; i < ${#option}; i++)); do
        char="${option:i:1}"
        if [ -n "${quote}" ]; then
            if [ "${char}" = "${quote}" ]; then
                next_char="${option:i+1:1}"
                if [ "${next_char}" = "${quote}" ]; then
                    i=$((i + 1))
                else
                    quote=""
                fi
            fi
            continue
        fi

        case "${char}" in
            "'")
                quote="'"
                ;;
            '"')
                quote='"'
                ;;
            "(")
                depth=$((depth + 1))
                ;;
            ")")
                if [ "${depth}" -gt 0 ]; then
                    depth=$((depth - 1))
                fi
                ;;
            "=")
                if [ "${depth}" = "0" ]; then
                    eq_index="${i}"
                    break
                fi
                ;;
        esac
    done

    [ "${eq_index}" -ge 0 ] || return 0
    key="$(trim_sql_ws "${option:0:${eq_index}}")"
    value="$(trim_sql_ws "${option:$((eq_index + 1))}")"

    case "${key}" in
        [Cc][Oo][Nn][Tt][Ee][Nn][Tt])
            if [ "${value}" = "''" ] || [ "${value}" = '""' ]; then
                printf '%s' "contentless"
            elif [ -n "${value}" ]; then
                printf '%s' "external"
            else
                return 1
            fi
            ;;
    esac
}

classify_fts5_content_mode() {
    local arglist="${1}"
    local option=""
    local mode
    local char
    local quote=""
    local depth=0
    local next_char
    local i

    for ((i = 0; i <= ${#arglist}; i++)); do
        if [ "${i}" -eq "${#arglist}" ]; then
            char=","
        else
            char="${arglist:i:1}"
        fi

        if [ -n "${quote}" ]; then
            option="${option}${char}"
            if [ "${char}" = "${quote}" ]; then
                next_char="${arglist:i+1:1}"
                if [ "${next_char}" = "${quote}" ]; then
                    option="${option}${next_char}"
                    i=$((i + 1))
                else
                    quote=""
                fi
            fi
            continue
        fi

        case "${char}" in
            "'")
                quote="'"
                option="${option}${char}"
                ;;
            '"')
                quote='"'
                option="${option}${char}"
                ;;
            "(")
                depth=$((depth + 1))
                option="${option}${char}"
                ;;
            ")")
                if [ "${depth}" -gt 0 ]; then
                    depth=$((depth - 1))
                fi
                option="${option}${char}"
                ;;
            ",")
                if [ "${depth}" = "0" ]; then
                    if ! mode="$(classify_fts5_option_content "${option}")"; then
                        return 1
                    fi
                    if [ -n "${mode}" ]; then
                        printf '%s' "${mode}"
                        return 0
                    fi
                    option=""
                else
                    option="${option}${char}"
                fi
                ;;
            *)
                option="${option}${char}"
                ;;
        esac
    done

    printf '%s' "normal"
}

classify_fts_table_sql() {
    local sql_text="${1}"
    local using_token_re='[Uu][Ss][Ii][Nn][Gg][[:space:]]+([[:alpha:]_][[:alnum:]_]*)'
    local using_re='[Uu][Ss][Ii][Nn][Gg][[:space:]]+([[:alpha:]_][[:alnum:]_]*)[[:space:]]*\('
    local matched
    local module_token
    local module
    local rest
    local arglist
    local content_mode

    if ! [[ "${sql_text}" =~ ^[[:space:]]*[Cc][Rr][Ee][Aa][Tt][Ee][[:space:]]+[Vv][Ii][Rr][Tt][Uu][Aa][Ll][[:space:]]+[Tt][Aa][Bb][Ll][Ee][[:space:]]+ ]]; then
        return 2
    fi
    if ! [[ "${sql_text}" =~ ${using_token_re} ]]; then
        return 1
    fi

    module_token="${BASH_REMATCH[1]}"
    case "${module_token}" in
        [Ff][Tt][Ss]3)
            if ! [[ "${sql_text}" =~ ${using_re} ]]; then
                return 1
            fi
            module="fts3"
            content_mode="normal"
            ;;
        [Ff][Tt][Ss]4)
            if ! [[ "${sql_text}" =~ ${using_re} ]]; then
                return 1
            fi
            module="fts4"
            content_mode="normal"
            ;;
        [Ff][Tt][Ss]5)
            if ! [[ "${sql_text}" =~ ${using_re} ]]; then
                return 1
            fi
            matched="${BASH_REMATCH[0]}"
            module="fts5"
            rest="${sql_text#*"${matched}"}"
            if ! arglist="$(extract_parenthesized_content "${rest}")"; then
                return 1
            fi
            if ! content_mode="$(classify_fts5_content_mode "${arglist}")"; then
                return 1
            fi
            ;;
        [Ff][Tt][Ss][0-9]*)
            return 2
            ;;
        *)
            return 2
            ;;
    esac

    printf '%s\t%s' "${module}" "${content_mode}"
}

validate_fts_metadata() {
    local table_name="${1}"
    local module="${2}"
    local content_mode="${3}"

    if [ -z "${table_name}" ] || has_control_chars "${table_name}"; then
        echo "ERROR: unsafe FTS table name discovered; aborting before maintenance" >&2
        return 1
    fi
    if has_control_chars "${module}" || has_control_chars "${content_mode}"; then
        echo "ERROR: unsafe FTS metadata discovered for ${table_name}; aborting before maintenance" >&2
        return 1
    fi
    case "${module}" in
        fts3|fts4|fts5)
            ;;
        *)
            echo "ERROR: unclassified FTS module '${module}' for ${table_name}; aborting before maintenance" >&2
            return 1
            ;;
    esac
    case "${content_mode}" in
        normal|external|contentless)
            ;;
        *)
            echo "ERROR: unclassified FTS content mode '${content_mode}' for ${table_name}; aborting before maintenance" >&2
            return 1
            ;;
    esac
}

# discover_fts_tables <binary> <db_path>
# Emits: <name><tab><module><tab><content_mode>
discover_fts_tables() {
    local binary="${1}"
    local db_path="${2}"
    local field_sep=$'\t'
    local query
    local rows
    local row
    local table_name
    local sql_text
    local extra
    local classification
    local rc
    local module
    local content_mode
    local metadata_extra

    query="SELECT name, replace(replace(replace(sql, char(9), ' '), char(10), ' '), char(13), ' ') FROM sqlite_master WHERE type = 'table' AND sql IS NOT NULL AND lower(ltrim(replace(replace(replace(sql, char(9), ' '), char(10), ' '), char(13), ' '))) LIKE 'create virtual table%' AND name NOT GLOB 'sqlite_*' ORDER BY name;"
    if ! rows=$("${binary}" -batch -noheader -separator "${field_sep}" "${db_path}" "${query}"); then
        echo "ERROR: FTS discovery query failed for ${db_path}" >&2
        return 1
    fi

    while IFS= read -r row
    do
        [ -n "${row}" ] || continue
        IFS="${field_sep}" read -r table_name sql_text extra <<< "${row}"
        if [ -n "${extra}" ] || [ -z "${sql_text}" ]; then
            echo "ERROR: unsafe or malformed FTS metadata discovered in ${db_path}" >&2
            return 1
        fi

        if classification="$(classify_fts_table_sql "${sql_text}")"; then
            IFS="${field_sep}" read -r module content_mode metadata_extra <<< "${classification}"
            if [ -n "${metadata_extra}" ]; then
                echo "ERROR: malformed FTS classification for ${table_name}; aborting before maintenance" >&2
                return 1
            fi
            if ! validate_fts_metadata "${table_name}" "${module}" "${content_mode}"; then
                return 1
            fi
            printf '%s\t%s\t%s\n' "${table_name}" "${module}" "${content_mode}"
        else
            rc=$?
            if [ "${rc}" = "2" ]; then
                continue
            fi
            echo "ERROR: unclassified FTS metadata for ${table_name} in ${db_path}" >&2
            return 1
        fi
    done <<< "${rows}"
}

run_fts_integrity_gate() {
    local binary="${1}"
    local db_path="${2}"
    local phase="${3}"
    local mode="${4:-hard}"
    local field_sep=$'\t'
    local fts_rows
    local row
    local table_name
    local module
    local content_mode
    local extra
    local table_ident
    local sql

    if ! fts_rows="$(discover_fts_tables "${binary}" "${db_path}")"; then
        echo "ERROR: ${phase} FTS discovery failed for ${db_path}" >&2
        return 1
    fi

    while IFS= read -r row
    do
        [ -n "${row}" ] || continue
        IFS="${field_sep}" read -r table_name module content_mode extra <<< "${row}"
        if [ -n "${extra}" ] || ! validate_fts_metadata "${table_name}" "${module}" "${content_mode}"; then
            echo "ERROR: ${phase} FTS metadata validation failed for ${db_path}" >&2
            return 1
        fi
        table_ident="$(quote_sql_ident "${table_name}")"
        if [ "${module}" = "fts5" ] && [ "${content_mode}" = "external" ]; then
            sql="INSERT INTO ${table_ident}(${table_ident}, rank) VALUES('integrity-check', 1);"
        else
            sql="INSERT INTO ${table_ident}(${table_ident}) VALUES('integrity-check');"
        fi
        if ! "${binary}" "${db_path}" "${sql}"; then
            if [ "${mode}" = "warn" ]; then
                echo "WARNING: ${phase} FTS integrity-check failed for ${table_name} in ${db_path}; continuing" >&2
                continue
            else
                echo "ERROR: ${phase} FTS integrity-check failed for ${table_name} in ${db_path}" >&2
                return 1
            fi
        fi
    done <<< "${fts_rows}"

    return 0
}

run_source_integrity_gate() {
    local binary="${1}"
    local db_path="${2}"
    local begin_marker="__sqlite3_builds_source_integrity_begin_v1__"
    local end_marker="__sqlite3_builds_source_integrity_end_v1__"
    local integrity_sql
    local integrity_output
    local state="before"
    local line
    local payload_count=0
    local ok_count=0
    local framing_error=0
    local hard_findings=""
    local saw_curated_fts4=0
    local saw_deferred_fts=0
    local saw_checker_hiccup=0

    integrity_sql="SELECT '${begin_marker}'; PRAGMA integrity_check; SELECT '${end_marker}';"
    if ! integrity_output=$("${binary}" "${db_path}" "${integrity_sql}"); then
        echo "WARNING: source integrity_check failed to run for ${db_path}; continuing because corruption was not demonstrated" >&2
        return 0
    fi
    integrity_output="${integrity_output//$'\r'/}"

    while IFS= read -r line
    do
        case "${state}" in
            before)
                if [ "${line}" != "${begin_marker}" ]; then
                    framing_error=1
                    break
                fi
                state="payload"
                ;;
            payload)
                if [ "${line}" = "${end_marker}" ]; then
                    state="done"
                    continue
                fi
                if [ -z "${line}" ]; then
                    framing_error=1
                    break
                fi
                payload_count=$((payload_count + 1))
                case "${line}" in
                    ok)
                        ok_count=$((ok_count + 1))
                        ;;
                    "malformed inverted index for FTS4 table main.fts4_tag_titles_icu")
                        saw_curated_fts4=1
                        ;;
                    malformed\ inverted\ index\ for\ FTS[345]\ table\ *)
                        saw_deferred_fts=1
                        ;;
                    unable\ to\ validate\ the\ inverted\ index\ for\ FTS[345]\ table\ *)
                        saw_checker_hiccup=1
                        ;;
                    *)
                        if [ -n "${hard_findings}" ]; then
                            hard_findings+=$'\n'
                        fi
                        hard_findings+="${line}"
                        ;;
                esac
                ;;
            done)
                framing_error=1
                break
                ;;
        esac
    done <<< "${integrity_output}"

    if [ "${framing_error}" -ne 0 ] || [ "${state}" != "done" ] || [ "${payload_count}" -eq 0 ]; then
        echo "WARNING: source integrity_check returned uninterpretable output for ${db_path}; continuing because corruption was not demonstrated" >&2
        return 0
    fi
    if [ "${ok_count}" -ne 0 ]; then
        if [ "${ok_count}" -eq 1 ] && [ "${payload_count}" -eq 1 ]; then
            return 0
        fi
        echo "WARNING: source integrity_check returned contradictory output for ${db_path}; continuing because corruption was not demonstrated" >&2
        return 0
    fi
    if [ -n "${hard_findings}" ]; then
        echo "ERROR: source DB failed integrity_check for ${db_path}; live DB has NOT been touched" >&2
        printf '%s\n' "${hard_findings}" >&2
        return 1
    fi
    if [ "${saw_curated_fts4}" -eq 1 ]; then
        echo "WARNING: source integrity_check accepted curated FTS4 exception for main.fts4_tag_titles_icu in ${db_path}" >&2
    fi
    if [ "${saw_deferred_fts}" -eq 1 ]; then
        echo "WARNING: source integrity_check deferred FTS xIntegrity findings to the source FTS warn-and-rebuild gate for ${db_path}" >&2
    fi
    if [ "${saw_checker_hiccup}" -eq 1 ]; then
        echo "WARNING: source integrity_check could not validate at least one FTS index in ${db_path}; continuing because source b-tree corruption was not demonstrated" >&2
    fi

    return 0
}

run_fts_rebuild_hard() {
    local binary="${1}"
    local db_path="${2}"
    local field_sep=$'\t'
    local fts_rows
    local row
    local table_name
    local module
    local content_mode
    local extra
    local table_ident
    local rebuild_sql

    if ! fts_rows="$(discover_fts_tables "${binary}" "${db_path}")"; then
        echo "ERROR: FTS discovery failed before rebuild for ${db_path}" >&2
        return 1
    fi

    while IFS= read -r row
    do
        [ -n "${row}" ] || continue
        IFS="${field_sep}" read -r table_name module content_mode extra <<< "${row}"
        if [ -n "${extra}" ] || ! validate_fts_metadata "${table_name}" "${module}" "${content_mode}"; then
            echo "ERROR: FTS metadata validation failed before rebuild for ${db_path}" >&2
            return 1
        fi

        if [ "${module}" = "fts5" ] && [ "${content_mode}" = "contentless" ]; then
            continue
        fi

        table_ident="$(quote_sql_ident "${table_name}")"
        rebuild_sql="INSERT INTO ${table_ident}(${table_ident}) VALUES('rebuild');"
        if ! "${binary}" "${db_path}" "${rebuild_sql}"; then
            echo "ERROR: FTS rebuild failed for ${table_name} in ${db_path}" >&2
            return 1
        fi
    done <<< "${fts_rows}"

    return 0
}

run_fts_recurate() {
    local binary="${1}"
    local db_path="${2}"
    local fts_table="fts4_tag_titles_icu"
    local content_table="tags"
    local content_id_column="id"
    local content_indexed_column="tag"
    local fts_ident
    local content_ident
    local content_id_ident
    local content_indexed_ident
    local docsize_ident
    local table_count
    local bad_count
    local delete_sql
    local postcondition_sql

    fts_ident="$(quote_sql_ident "${fts_table}")"
    content_ident="$(quote_sql_ident "${content_table}")"
    content_id_ident="$(quote_sql_ident "${content_id_column}")"
    content_indexed_ident="$(quote_sql_ident "${content_indexed_column}")"
    docsize_ident="$(quote_sql_ident "${fts_table}_docsize")"

    if ! table_count=$("${binary}" "${db_path}" "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='fts4_tag_titles_icu';" | tr -d '\r\n'); then
        echo "ERROR: FTS re-curation target check failed for ${db_path}; live DB has NOT been touched" >&2
        return 1
    fi
    if [ "${table_count}" = "0" ]; then
        return 0
    fi

    delete_sql="DELETE FROM ${fts_ident} WHERE docid IN (SELECT rowid FROM ${content_ident} WHERE ${content_indexed_ident} GLOB '*://*' OR ${content_indexed_ident} IS NULL OR trim(${content_indexed_ident})='');"
    if ! "${binary}" "${db_path}" "${delete_sql}"; then
        echo "ERROR: FTS re-curation DELETE failed for ${fts_table} in ${db_path}; live DB has NOT been touched" >&2
        return 1
    fi

    postcondition_sql="SELECT count(*) FROM ${docsize_ident} d JOIN ${content_ident} t ON t.${content_id_ident}=d.docid WHERE t.${content_indexed_ident} GLOB '*://*' OR t.${content_indexed_ident} IS NULL OR trim(t.${content_indexed_ident})='';"
    if ! bad_count=$("${binary}" "${db_path}" "${postcondition_sql}" | tr -d '\r\n'); then
        echo "ERROR: FTS re-curation postcondition query failed for ${fts_table} in ${db_path}; live DB has NOT been touched" >&2
        return 1
    fi
    if [ "${bad_count}" != "0" ]; then
        echo "ERROR: FTS re-curation postcondition failed for ${fts_table} in ${db_path}: ${bad_count} URI/empty docs remain; live DB has NOT been touched" >&2
        return 1
    fi

    return 0
}

run_fts_recurate_shadow_integrity_gate() {
    local binary="${1}"
    local db_path="${2}"
    local shadow_query
    local shadow_tables
    local shadow_table
    local shadow_ident
    local shadow_integrity
    local shadow_table_count=0

    shadow_query="SELECT name FROM sqlite_master WHERE type='table' AND name GLOB 'fts4_tag_titles_icu_*' ORDER BY name;"
    if ! shadow_tables=$("${binary}" -batch -noheader "${db_path}" "${shadow_query}"); then
        echo "ERROR: post-re-curation FTS shadow-table discovery failed for ${db_path}; live DB has NOT been touched" >&2
        return 1
    fi

    while IFS= read -r shadow_table
    do
        [ -n "${shadow_table}" ] || continue
        if has_control_chars "${shadow_table}"; then
            echo "ERROR: unsafe post-re-curation FTS shadow-table name discovered in ${db_path}; live DB has NOT been touched" >&2
            return 1
        fi
        shadow_table_count=$((shadow_table_count + 1))
        shadow_ident="$(quote_sql_ident "${shadow_table}")"
        if ! shadow_integrity=$("${binary}" "${db_path}" "PRAGMA integrity_check(${shadow_ident});" < /dev/null | tr -d '\r\n'); then
            echo "ERROR: post-re-curation FTS shadow integrity_check failed to run for ${shadow_table}; live DB has NOT been touched" >&2
            return 1
        fi
        if [ "${shadow_integrity}" != "ok" ]; then
            echo "ERROR: post-re-curation FTS shadow table ${shadow_table} failed integrity_check: ${shadow_integrity}; live DB has NOT been touched" >&2
            return 1
        fi
    done <<< "${shadow_tables}"

    if [ "${shadow_table_count}" = "0" ]; then
        echo "ERROR: no post-re-curation FTS shadow tables were discovered in ${db_path}; live DB has NOT been touched" >&2
        return 1
    fi

    return 0
}

run_foreign_key_check_warn() {
    local binary="${1}"
    local db_path="${2}"
    local fk_rows

    if ! fk_rows=$("${binary}" "${db_path}" "PRAGMA foreign_key_check;" | tr -d '\r'); then
        echo "WARNING: foreign_key_check failed to run for ${db_path}; continuing after source FTS gates" >&2
        return 0
    fi
    if [ -n "${fk_rows}" ]; then
        echo "WARNING: foreign_key_check returned rows for ${db_path}; continuing after source FTS gates" >&2
        printf '%s\n' "${fk_rows}" >&2
    fi
    return 0
}

run_staged_maintenance_sql() {
    local binary="${1}"
    local db_path="${2}"
    local sql="${3}"

    if ! "${binary}" "${db_path}" "${sql}"; then
        echo "WARNING: staged maintenance SQL failed for ${db_path}; continuing with rebuilt staged DB" >&2
    fi
}

run_staged_sql_warn() {
    local binary="${1}"
    local db_path="${2}"
    local step_name="${3}"
    local sql="${4}"

    if ! "${binary}" "${db_path}" "${sql}"; then
        echo "WARNING: ${step_name} failed for ${db_path}; continuing with rebuilt staged DB" >&2
    fi
}

run_plex_metadata_date_repairs_warn() {
    local binary="${1}"
    local db_path="${2}"

    run_staged_sql_warn "${binary}" "${db_path}" "Plex originally_available_at lower-bound repair" "UPDATE metadata_items SET originally_available_at = STRFTIME('%s', 'now', '-7 days') WHERE originally_available_at < STRFTIME('%s', 'now', '-200 years');"
    run_staged_sql_warn "${binary}" "${db_path}" "Plex originally_available_at upper-bound repair" "UPDATE metadata_items SET originally_available_at = STRFTIME('%s', 'now', '-3 days') WHERE originally_available_at > STRFTIME('%s', 'now');"
    run_staged_sql_warn "${binary}" "${db_path}" "Plex added_at lower-bound repair" "UPDATE metadata_items SET added_at = originally_available_at WHERE added_at <> originally_available_at AND originally_available_at IS NOT NULL AND added_at < STRFTIME('%s', 'now', '-200 years');"
    run_staged_sql_warn "${binary}" "${db_path}" "Plex added_at upper-bound repair" "UPDATE metadata_items SET added_at = originally_available_at WHERE added_at <> originally_available_at AND originally_available_at IS NOT NULL AND added_at > STRFTIME('%s', 'now');"
}

run_plex_main_post_maintenance() {
    local stat4_binary="${1}"
    local staged_db="${2}"

    echo "Fixing Plex metadata dates in staged DB: ${staged_db}"
    run_plex_metadata_date_repairs_warn "${PLEX_BINARY}" "${staged_db}"

    # WHY: reads main()'s dynamic-scoped local (sole entry is main "$@"); :-0 keeps STAT4 fail-safe off if ever out of scope.
    if [ "${plex_stat4_enabled:-0}" = "1" ]; then
        run_plex_stat4_analyze "${stat4_binary}" "${staged_db}"
    fi

    return 0
}

run_post_swap_fts_maintenance() {
    local binary="${1}"
    local db_path="${2}"
    local field_sep=$'\t'
    local fts_rows
    local row
    local table_name
    local module
    local content_mode
    local extra
    local table_ident
    local optimize_sql

    if ! fts_rows="$(discover_fts_tables "${binary}" "${db_path}")"; then
        echo "WARNING: post-swap FTS discovery failed for ${db_path}; DB was already validated and swapped; continuing" >&2
        return 0
    fi

    while IFS= read -r row
    do
        [ -n "${row}" ] || continue
        IFS="${field_sep}" read -r table_name module content_mode extra <<< "${row}"
        if [ -n "${extra}" ] || ! validate_fts_metadata "${table_name}" "${module}" "${content_mode}"; then
            echo "WARNING: post-swap FTS metadata validation failed for ${db_path}; DB was already validated and swapped; continuing" >&2
            continue
        fi

        table_ident="$(quote_sql_ident "${table_name}")"
        optimize_sql="INSERT INTO ${table_ident}(${table_ident}) VALUES('optimize');"

        if ! "${binary}" "${db_path}" "${optimize_sql}"; then
            echo "WARNING: post-swap FTS optimize failed for ${table_name} in ${db_path}; continuing" >&2
        fi
    done <<< "${fts_rows}"

    return 0
}

try_deflate_plex_statistics_bandwidth() {
    local binary="${1}"
    local staged_db="${2}"
    local retain_days="${3}"
    local table_exists
    local delete_sql

    if ! table_exists=$("${binary}" "${staged_db}" "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'statistics_bandwidth' LIMIT 1;" | tr -d '\r\n'); then
        echo "WARNING: could not check for statistics_bandwidth in ${staged_db}; skipping Plex bandwidth deflate" >&2
        return 0
    fi
    if [ "${table_exists}" != "1" ]; then
        echo "WARNING: statistics_bandwidth is absent in ${staged_db}; skipping Plex bandwidth deflate" >&2
        return 0
    fi
    case "${retain_days}" in
        ""|*[!0-9]*)
            echo "WARNING: invalid statistics_bandwidth retention '${retain_days}'; skipping Plex bandwidth deflate" >&2
            return 0
            ;;
    esac

    echo "Deflating Plex statistics_bandwidth in ${staged_db}; retaining ${retain_days} days"
    delete_sql="DELETE FROM statistics_bandwidth WHERE account_id IS NULL OR at < STRFTIME('%s','now','-' || ${retain_days} || ' days');"
    if ! "${binary}" "${staged_db}" "${delete_sql}"; then
        echo "WARNING: statistics_bandwidth deflate DELETE failed for ${staged_db}; continuing with validated staged DB" >&2
        return 0
    fi
    if ! "${binary}" "${staged_db}" \
      "PRAGMA threads=8;" \
      "VACUUM;"; then
        echo "WARNING: post-deflate VACUUM failed for ${staged_db}; continuing to final integrity gate" >&2
    fi

    return 0
}

try_trim_plex_finished_season_blobs() (
    local binary="${1}"
    local staged_db="${2}"
    local main_db="${plex_databases_path}/${_PLEX_DB}"
    local trim_work_dir=""
    local finished_ids_framed_file
    local finished_ids_file
    local finished_ids_sql
    local trim_out
    local deleted_count=""

    if ! trim_work_dir=$(mktemp -d "${staged_db}.trim.XXXXXX"); then
        echo "WARNING: could not create Plex blob trim scratch beside ${staged_db}; skipping finished-season blob trim" >&2
        return 0
    fi
    trap 'rm -rf -- "${trim_work_dir}"' EXIT
    finished_ids_framed_file="${trim_work_dir}/finished-season-media-part-ids.framed.txt"
    finished_ids_file="${trim_work_dir}/finished-season-media-part-ids.txt"

    finished_ids_sql="SELECT DISTINCT 'ID|' || mp.id
FROM media_parts AS mp
JOIN media_items AS mi
  ON mi.id=mp.media_item_id
JOIN metadata_items AS ep
  ON ep.id=mi.metadata_item_id
 AND ep.metadata_type=4
JOIN metadata_items AS season
  ON season.id=ep.parent_id
 AND season.metadata_type=3
JOIN (
  SELECT parent_id AS season_id,
         max(coalesce(originally_available_at, added_at)) AS last_air
  FROM metadata_items
  WHERE metadata_type=4
  GROUP BY parent_id
) AS sl
  ON sl.season_id=season.id
WHERE typeof(sl.last_air)='integer'
  AND sl.last_air <= CAST(strftime('%s','now','-24 months') AS INTEGER)
ORDER BY mp.id;"

    if ! "${binary}" -init /dev/null -readonly -batch -noheader "${main_db}" "${finished_ids_sql}" > "${finished_ids_framed_file}"; then
        echo "WARNING: could not read finished-season media_part ids from ${main_db}; skipping Plex finished-season blob trim" >&2
        return 0
    fi
    if ! awk '
      $0 == "" { next }
      $0 ~ /^ID[|][0-9]+$/ { print substr($0, 4); next }
      { exit 1 }
    ' "${finished_ids_framed_file}" > "${finished_ids_file}"; then
        echo "WARNING: non-conforming finished-season media_part id output from ${main_db}; skipping Plex finished-season blob trim" >&2
        return 0
    fi

    if ! trim_out=$("${binary}" -batch -noheader "${staged_db}" <<SQL
.bail on
CREATE TEMP TABLE finished_season_media_part_ids(
  id INTEGER PRIMARY KEY
) WITHOUT ROWID;
.mode list
.import '${finished_ids_file}' finished_season_media_part_ids

BEGIN IMMEDIATE;
CREATE TEMP TABLE trim_candidates(
  id INTEGER PRIMARY KEY
) WITHOUT ROWID;
INSERT INTO trim_candidates(id)
SELECT b.id
FROM blobs AS b
JOIN finished_season_media_part_ids AS f
  ON f.id=b.linked_id
WHERE b.blob_type=5
  AND b.linked_type='media_part';

CREATE TEMP TABLE trim_counts(
  label TEXT PRIMARY KEY,
  value INTEGER NOT NULL
) WITHOUT ROWID;
INSERT INTO trim_counts VALUES
  ('candidate',(SELECT count(*) FROM trim_candidates)),
  ('total',(SELECT count(*) FROM blobs)),
  ('target',(SELECT count(*) FROM blobs
             WHERE blob_type=5 AND linked_type='media_part'));

DELETE FROM blobs
WHERE blob_type=5
  AND linked_type='media_part'
  AND linked_id IN (SELECT id FROM finished_season_media_part_ids);
INSERT INTO trim_counts VALUES ('deleted',changes());
INSERT INTO trim_counts VALUES
  ('post_total',(SELECT count(*) FROM blobs)),
  ('post_target',(SELECT count(*) FROM blobs
                  WHERE blob_type=5 AND linked_type='media_part')),
  ('post_candidate',(SELECT count(*) FROM blobs
                     WHERE id IN (SELECT id FROM trim_candidates)));

CREATE TEMP TABLE invariant_guard(
  ok INTEGER NOT NULL CHECK(ok=1)
);
INSERT INTO invariant_guard
SELECT (SELECT value FROM trim_counts WHERE label='deleted')
     = (SELECT value FROM trim_counts WHERE label='candidate');
INSERT INTO invariant_guard
SELECT (SELECT value FROM trim_counts WHERE label='post_candidate')=0;
INSERT INTO invariant_guard
SELECT (SELECT value FROM trim_counts WHERE label='post_total')
     + (SELECT value FROM trim_counts WHERE label='deleted')
     = (SELECT value FROM trim_counts WHERE label='total');
INSERT INTO invariant_guard
SELECT (SELECT value FROM trim_counts WHERE label='post_target')
     + (SELECT value FROM trim_counts WHERE label='deleted')
     = (SELECT value FROM trim_counts WHERE label='target');

SELECT 'DELETED|' || value FROM trim_counts WHERE label='deleted';
COMMIT;
SQL
); then
        echo "WARNING: Plex finished-season blob trim DELETE failed for ${staged_db}; continuing with validated staged DB" >&2
        return 0
    fi

    if ! deleted_count=$(awk -F '|' '
      $1 == "DELETED" { count++; value=$2 }
      END {
        if (count == 1 && value ~ /^[0-9]+$/) print value;
        else exit 1;
      }
    ' <<< "${trim_out}"); then
        echo "WARNING: could not parse Plex finished-season deleted count for ${staged_db}; running VACUUM conservatively" >&2
        deleted_count=""
    fi

    if [ "${deleted_count}" = "0" ]; then
        echo "Plex finished-season blob trim deleted 0 rows from ${staged_db}; skipping trim VACUUM"
    else
        if ! "${binary}" "${staged_db}" \
          "PRAGMA threads=8;" \
          "VACUUM;"; then
            echo "WARNING: post-trim VACUUM failed for ${staged_db}; continuing to final integrity gate" >&2
        fi
    fi

    return 0
)

rebuild_db_vacuum_into() {
    local binary="${1}"
    local db_path="${2}"
    local backup_dir="${3}"
    local page_size="${4}"
    local auto_vacuum_mode="${5}"
    local sanity_sql="${6}"
    local optimize_sql="${7}"
    local pre_swap_hook="${8:-}"
    local post_maintenance_hook="${9:-}"
    local post_maintenance_binary="${10:-}"
    local final_pre_publish_hook="${11:-}"
    local db_file="${db_path##*/}"
    local backup_path
    local backup_tmp
    local staged_db="${db_path}.new"
    local auto_vacuum_expected=""
    local auto_vacuum_actual=""
    local backup_date
    local new_mode
    local needs_fixup=0
    local final_integrity
    local source_user_version
    local staged_user_version
    local source_application_id
    local staged_application_id
    local staged_page_size
    local wal_checkpoint_result
    local wal_checkpoint_busy
    local table_query
    local table_list
    local table_name
    local table_ident
    local source_count
    local staged_count

    case "${auto_vacuum_mode}" in
        "")
            auto_vacuum_expected=""
            ;;
        NONE|none|0)
            auto_vacuum_expected="0"
            ;;
        FULL|full|1)
            auto_vacuum_expected="1"
            ;;
        INCREMENTAL|incremental|2)
            auto_vacuum_expected="2"
            ;;
        *)
            echo "ERROR: unsupported auto_vacuum mode '${auto_vacuum_mode}' for ${db_path}" >&2
            exit 1
            ;;
    esac

    if [[ "${staged_db}" == *"'"* || "${staged_db}" =~ [[:cntrl:]] ]]; then
        echo "ERROR: database path contains a single quote or control character and cannot be safely used with VACUUM INTO: ${staged_db}" >&2
        exit 1
    fi

    if ! source_user_version=$("${binary}" "${db_path}" "PRAGMA user_version;" | tr -d '\r\n'); then
        echo "ERROR: source user_version check failed for ${db_path}; live DB has NOT been touched" >&2
        exit 1
    fi
    if ! source_application_id=$("${binary}" "${db_path}" "PRAGMA application_id;" | tr -d '\r\n'); then
        echo "ERROR: source application_id check failed for ${db_path}; live DB has NOT been touched" >&2
        exit 1
    fi

    if [ -n "${sanity_sql}" ]; then
        "${binary}" "${db_path}" "${sanity_sql}"
    fi
    if ! run_fts_integrity_gate "${binary}" "${db_path}" "source" "warn"; then
        echo "ERROR: source FTS metadata is unsafe or unclassified for ${db_path}; live DB has NOT been touched" >&2
        exit 1
    fi
    if ! run_source_integrity_gate "${binary}" "${db_path}"; then
        exit 1
    fi
    run_foreign_key_check_warn "${binary}" "${db_path}"

    # Fold WAL data into the main file before the byte-for-byte filesystem backup.
    if wal_checkpoint_result=$("${binary}" -batch -noheader -separator '|' "${db_path}" "PRAGMA wal_checkpoint(TRUNCATE);" | tr -d '\r'); then
        IFS='|' read -r wal_checkpoint_busy _ <<< "${wal_checkpoint_result}"
        if [ -z "${wal_checkpoint_busy}" ]; then
            echo "WARNING: wal_checkpoint(TRUNCATE) returned no busy column for ${db_path}; continuing to journal_mode=DELETE" >&2
        elif [ "${wal_checkpoint_busy}" != "0" ]; then
            echo "WARNING: wal_checkpoint(TRUNCATE) reported busy=${wal_checkpoint_busy} for ${db_path}; continuing to journal_mode=DELETE" >&2
        fi
    else
        echo "WARNING: wal_checkpoint(TRUNCATE) failed for ${db_path}; continuing to journal_mode=DELETE" >&2
    fi
    if ! new_mode=$("${binary}" "${db_path}" "PRAGMA journal_mode=DELETE;" | tr -d '\r\n'); then
        echo "ERROR: could not switch ${db_path} to DELETE journal mode; aborting before backup/rebuild to avoid orphaning WAL data" >&2
        exit 1
    fi
    if [ "${new_mode}" != "delete" ]; then
        echo "ERROR: could not switch ${db_path} to DELETE journal mode (got '${new_mode}'); aborting before backup/rebuild to avoid orphaning WAL data" >&2
        exit 1
    fi

    backup_date="$(date '+%Y-%m-%d')"
    backup_path="${backup_dir}/${db_file}-${backup_date}.original"
    if ! backup_tmp=$(mktemp "${backup_path}.tmp.XXXXXX"); then
        echo "ERROR: could not create a temporary backup file for ${db_path}; live DB has NOT been touched" >&2
        exit 1
    fi
    if ! cp "${db_path}" "${backup_tmp}"; then
        echo "ERROR: backup copy failed for ${db_path}; live DB has NOT been touched" >&2
        rm -f "${backup_tmp}"
        exit 1
    fi
    if ! mv "${backup_tmp}" "${backup_path}"; then
        echo "ERROR: could not publish backup ${backup_path}; live DB has NOT been touched" >&2
        rm -f "${backup_tmp}"
        exit 1
    fi

    # Inert fallback if VACUUM INTO cannot be used.
    # WHY: console-stdout MODE_TTY makes a plain .dump emit jsonb(...).
    # "${binary}" -batch -init /dev/null "${db_path}" \
    #   ".mode insert --textjsonb off --multiinsert 0" \
    #   ".output \"${staged_db}.dump.sql\"" \
    #   ".dump"
    # "${binary}" -batch -init /dev/null "${staged_db}" \
    #   "PRAGMA page_size=${page_size};" \
    #   ".read \"${staged_db}.dump.sql\""

    cleanup_staged_db "${staged_db}"
    if [ -n "${auto_vacuum_mode}" ]; then
        if ! "${binary}" "${db_path}" \
          "PRAGMA threads=8;" \
          "PRAGMA page_size=${page_size};" \
          "PRAGMA auto_vacuum=${auto_vacuum_mode};" \
          "VACUUM INTO '${staged_db}';"; then
            echo "ERROR: VACUUM INTO failed for ${db_path}; live DB has NOT been touched" >&2
            cleanup_staged_db "${staged_db}"
            exit 1
        fi
    else
        if ! "${binary}" "${db_path}" \
          "PRAGMA threads=8;" \
          "PRAGMA page_size=${page_size};" \
          "VACUUM INTO '${staged_db}';"; then
            echo "ERROR: VACUUM INTO failed for ${db_path}; live DB has NOT been touched" >&2
            cleanup_staged_db "${staged_db}"
            exit 1
        fi
    fi

    if ! staged_page_size=$("${binary}" "${staged_db}" "PRAGMA page_size;" | tr -d '\r\n'); then
        echo "ERROR: staged page_size check failed for ${staged_db}; live DB has NOT been touched" >&2
        cleanup_staged_db "${staged_db}"
        exit 1
    fi
    if [ "${staged_page_size}" != "${page_size}" ]; then
        needs_fixup=1
    fi
    if [ -n "${auto_vacuum_mode}" ]; then
        if ! auto_vacuum_actual=$("${binary}" "${staged_db}" "PRAGMA auto_vacuum;" | tr -d '\r\n'); then
            echo "ERROR: staged auto_vacuum check failed for ${staged_db}; live DB has NOT been touched" >&2
            cleanup_staged_db "${staged_db}"
            exit 1
        fi
        if [ "${auto_vacuum_actual}" != "${auto_vacuum_expected}" ]; then
            needs_fixup=1
        fi
    fi

    if [ "${needs_fixup}" = "1" ]; then
        if [ -n "${auto_vacuum_mode}" ]; then
            if ! "${binary}" "${staged_db}" \
              "PRAGMA threads=8;" \
              "PRAGMA page_size=${page_size};" \
              "PRAGMA auto_vacuum=${auto_vacuum_mode};" \
              "VACUUM;"; then
                echo "ERROR: staged page_size/auto_vacuum fix-up failed for ${staged_db}; live DB has NOT been touched" >&2
                cleanup_staged_db "${staged_db}"
                exit 1
            fi
        else
            if ! "${binary}" "${staged_db}" \
              "PRAGMA threads=8;" \
              "PRAGMA page_size=${page_size};" \
              "VACUUM;"; then
                echo "ERROR: staged page_size fix-up failed for ${staged_db}; live DB has NOT been touched" >&2
                cleanup_staged_db "${staged_db}"
                exit 1
            fi
        fi

        if ! staged_page_size=$("${binary}" "${staged_db}" "PRAGMA page_size;" | tr -d '\r\n'); then
            echo "ERROR: staged page_size re-check failed for ${staged_db}; live DB has NOT been touched" >&2
            cleanup_staged_db "${staged_db}"
            exit 1
        fi
        if [ -n "${auto_vacuum_mode}" ]; then
            if ! auto_vacuum_actual=$("${binary}" "${staged_db}" "PRAGMA auto_vacuum;" | tr -d '\r\n'); then
                echo "ERROR: staged auto_vacuum re-check failed for ${staged_db}; live DB has NOT been touched" >&2
                cleanup_staged_db "${staged_db}"
                exit 1
            fi
        fi
    fi

    if [ "${staged_page_size}" != "${page_size}" ]; then
        echo "WARNING: page_size is ${staged_page_size} (expected ${page_size}) on ${staged_db}; continuing with validated data and unchanged live DB until swap" >&2
    fi
    if [ -n "${auto_vacuum_mode}" ] && [ "${auto_vacuum_actual}" != "${auto_vacuum_expected}" ]; then
        echo "ERROR: auto_vacuum is ${auto_vacuum_actual} (expected ${auto_vacuum_expected}/${auto_vacuum_mode}) on ${staged_db}; live DB has NOT been touched" >&2
        cleanup_staged_db "${staged_db}"
        exit 1
    fi

    # WHY: The exhaustive source-vs-staged row-count sweep is intentional
    # defense-in-depth before publishing the rebuilt DB.
    table_query="SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' ORDER BY name;"
    if ! table_list=$("${binary}" "${db_path}" "${table_query}" | tr -d '\r'); then
        echo "ERROR: source table-list check failed for ${db_path}; live DB has NOT been touched" >&2
        cleanup_staged_db "${staged_db}"
        exit 1
    fi
    while IFS= read -r table_name
    do
        [ -n "${table_name}" ] || continue
        table_ident="${table_name//\"/\"\"}"
        if ! source_count=$("${binary}" "${db_path}" "SELECT COUNT(*) FROM \"${table_ident}\";" | tr -d '\r\n'); then
            echo "ERROR: source row-count check failed for ${table_name}; live DB has NOT been touched" >&2
            cleanup_staged_db "${staged_db}"
            exit 1
        fi
        if ! staged_count=$("${binary}" "${staged_db}" "SELECT COUNT(*) FROM \"${table_ident}\";" | tr -d '\r\n'); then
            echo "ERROR: staged row-count check failed for ${table_name}; live DB has NOT been touched" >&2
            cleanup_staged_db "${staged_db}"
            exit 1
        fi
        if [ "${source_count}" != "${staged_count}" ]; then
            echo "ERROR: row-count mismatch for ${table_name}: source=${source_count}, staged=${staged_count}; live DB has NOT been touched" >&2
            cleanup_staged_db "${staged_db}"
            exit 1
        fi
    done <<< "${table_list}"

    if ! run_fts_rebuild_hard "${binary}" "${staged_db}"; then
        echo "ERROR: staged FTS rebuild failed for ${staged_db}; live DB has NOT been touched" >&2
        cleanup_staged_db "${staged_db}"
        exit 1
    fi
    if ! run_fts_integrity_gate "${binary}" "${staged_db}" "staged"; then
        echo "ERROR: staged FTS integrity gate failed for ${staged_db}; live DB has NOT been touched" >&2
        cleanup_staged_db "${staged_db}"
        exit 1
    fi

    if [ -n "${pre_swap_hook}" ]; then
        if ! "${pre_swap_hook}" "${binary}" "${staged_db}" "${STATS_BANDWIDTH_RETAIN_DAYS}"; then
            echo "ERROR: pre-swap hook failed for ${staged_db}; live DB has NOT been touched" >&2
            cleanup_staged_db "${staged_db}"
            exit 1
        fi
    fi

    if [ -n "${optimize_sql}" ]; then
        run_staged_maintenance_sql "${binary}" "${staged_db}" "${optimize_sql}"
    fi
    if [ -n "${post_maintenance_hook}" ]; then
        if ! "${post_maintenance_hook}" "${post_maintenance_binary}" "${staged_db}"; then
            echo "ERROR: post-maintenance hook failed for ${staged_db}; live DB has NOT been touched" >&2
            cleanup_staged_db "${staged_db}"
            exit 1
        fi
    fi
    if [ -n "${final_pre_publish_hook}" ]; then
        if ! "${final_pre_publish_hook}" "${binary}" "${staged_db}"; then
            echo "ERROR: final pre-publication hook failed for ${staged_db}; live DB has NOT been touched" >&2
            cleanup_staged_db "${staged_db}"
            exit 1
        fi
    fi
    if ! final_integrity=$("${binary}" "${staged_db}" "PRAGMA integrity_check;" | tr -d '\r\n'); then
        echo "ERROR: final staged integrity_check failed to run for ${staged_db}; live DB has NOT been touched" >&2
        cleanup_staged_db "${staged_db}"
        exit 1
    fi
    if [ "${final_integrity}" != "ok" ]; then
        echo "ERROR: final staged DB failed integrity_check: ${final_integrity}; live DB has NOT been touched" >&2
        cleanup_staged_db "${staged_db}"
        exit 1
    fi

    # Temporarily disabled pending the patched-ICU FTS re-curation rework.
    # if ! run_fts_recurate "${binary}" "${staged_db}"; then
    #     echo "ERROR: staged FTS re-curation failed for ${staged_db}; live DB has NOT been touched" >&2
    #     cleanup_staged_db "${staged_db}"
    #     exit 1
    # fi
    # if ! run_fts_recurate_shadow_integrity_gate "${binary}" "${staged_db}"; then
    #     echo "ERROR: staged post-re-curation FTS shadow structural integrity gate failed for ${staged_db}; live DB has NOT been touched" >&2
    #     cleanup_staged_db "${staged_db}"
    #     exit 1
    # fi

    if ! staged_user_version=$("${binary}" "${staged_db}" "PRAGMA user_version;" | tr -d '\r\n'); then
        echo "ERROR: staged user_version check failed for ${staged_db}; live DB has NOT been touched" >&2
        cleanup_staged_db "${staged_db}"
        exit 1
    fi
    if [ "${staged_user_version}" != "${source_user_version}" ]; then
        echo "ERROR: staged user_version mismatch for ${staged_db}: source=${source_user_version}, staged=${staged_user_version}; live DB has NOT been touched" >&2
        cleanup_staged_db "${staged_db}"
        exit 1
    fi
    if ! staged_application_id=$("${binary}" "${staged_db}" "PRAGMA application_id;" | tr -d '\r\n'); then
        echo "ERROR: staged application_id check failed for ${staged_db}; live DB has NOT been touched" >&2
        cleanup_staged_db "${staged_db}"
        exit 1
    fi
    if [ "${staged_application_id}" != "${source_application_id}" ]; then
        echo "ERROR: staged application_id mismatch for ${staged_db}: source=${source_application_id}, staged=${staged_application_id}; live DB has NOT been touched" >&2
        cleanup_staged_db "${staged_db}"
        exit 1
    fi

    mv "${staged_db}" "${db_path}"
    rm -f "${db_path}-wal" "${db_path}-shm" "${staged_db}-wal" "${staged_db}-shm"
    run_post_swap_fts_maintenance "${binary}" "${db_path}"
}

optimize_plex_db() {
    local db_file="${1}"
    local sanity_sql="${2}"
    local pre_swap_hook="${3:-}"
    local final_pre_publish_hook="${4:-}"
    local db_path="${plex_databases_path}/${db_file}"
    local backup_dir
    local optimize_sql
    local post_maintenance_hook=""
    local post_maintenance_binary=""

    echo "Optimizing Plex Database: ${db_path}"
    if [ -d "${BACKUP_PATH}" ]; then
      mkdir -p "${BACKUP_PATH}/${plex_instance}"
      backup_dir="${BACKUP_PATH}/${plex_instance}"
    else
      backup_dir="${plex_databases_path}"
    fi

    # NOTE: The 0x10000 bit of PRAGMA optimize=0x10002 is honored on the patched
    # Plex SQLite 3.53.3 and was inert on the bundled 3.39.4; it size-checks every
    # table rather than only recently used ones. The mask omits 0x00010 on purpose:
    # the preceding analysis_limit=0 makes the ANALYZE deliberately unbounded.
    optimize_sql="PRAGMA cache_size=-1048576; PRAGMA temp_store=2; PRAGMA threads=8; REINDEX; PRAGMA analysis_limit=0; ANALYZE; PRAGMA optimize=0x10002;"
    if [ "${db_file}" = "${_PLEX_DB}" ]; then
        optimize_sql="$(build_plex_optimize_sql)"
        post_maintenance_hook="run_plex_main_post_maintenance"
        post_maintenance_binary="${PLEX_BINARY}"
    fi

    rebuild_db_vacuum_into \
      "${PLEX_BINARY}" \
      "${db_path}" \
      "${backup_dir}" \
      "${_PAGE_SIZE}" \
      "NONE" \
      "${sanity_sql}" \
      "${optimize_sql}" \
      "${pre_swap_hook}" \
      "${post_maintenance_hook}" \
      "${post_maintenance_binary}" \
      "${final_pre_publish_hook}"
}

plex_preferences_file() {
    local instance="${1}"

    printf '/opt/%s/Library/Application Support/Plex Media Server/Preferences.xml\n' "${instance}"
}

extract_plex_online_token() {
    local preferences_file="${1}"
    local token=""
    local seen=0
    local extracted

    [ -r "${preferences_file}" ] || return 1

    while IFS= read -r extracted
    do
        seen=$((seen + 1))
        if [ "${seen}" -eq 1 ]; then
            token="${extracted}"
        fi
    done < <(sed -n 's/.*PlexOnlineToken="\([^"]*\)".*/\1/p' "${preferences_file}")

    [ "${seen}" -eq 1 ] || return 1
    [ -n "${token}" ] || return 1
    case "${token}" in
        *[!ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-]*)
            return 1
            ;;
    esac

    printf '%s' "${token}"
}

resolve_plex_container_ip() {
    local instance="${1}"
    local inspect_out
    local ip

    if ! inspect_out="$(docker inspect --format '{{range .NetworkSettings.Networks}}{{println .IPAddress}}{{end}}' "${instance}" 2>/dev/null)"; then
        return 1
    fi

    while IFS= read -r ip
    do
        [ -n "${ip}" ] || continue
        printf '%s' "${ip}"
        return 0
    done <<< "${inspect_out}"

    return 1
}

plex_http_request() {
    local method="${1}"
    local url="${2}"
    local token="${3:-}"
    local max_time="${4:-30}"
    local marker="__PLEX_OPTIMIZE_HTTP_STATUS__:"
    local response
    local status
    local body
    local curl_rc
    local restore_errexit=0

    case "${max_time}" in
        ""|*[!0-9]*)
            max_time=30
            ;;
    esac
    if [ "${max_time}" -lt 1 ]; then
        max_time=1
    fi

    case "$-" in
        *e*)
            restore_errexit=1
            set +e
            ;;
    esac

    if [ -n "${token}" ]; then
        response="$(curl \
            --silent \
            --show-error \
            --output - \
            --write-out "${marker}%{http_code}" \
            --config - <<EOF_CURL_CONFIG
request = "${method}"
url = "${url}"
header = "X-Plex-Token: ${token}"
connect-timeout = 5
max-time = ${max_time}
EOF_CURL_CONFIG
)"
    else
        response="$(curl \
            --silent \
            --show-error \
            --output - \
            --write-out "${marker}%{http_code}" \
            --config - <<EOF_CURL_CONFIG
request = "${method}"
url = "${url}"
connect-timeout = 5
max-time = ${max_time}
EOF_CURL_CONFIG
)"
    fi
    curl_rc=$?

    if [ "${restore_errexit}" -eq 1 ]; then
        set -e
    fi

    status="${response##*${marker}}"
    if [ "${status}" = "${response}" ]; then
        status="000"
        body="${response}"
    else
        body="${response%${marker}${status}}"
    fi

    printf '%s\n%s' "${status}" "${body}"
    return "${curl_rc}"
}

plex_http_status() {
    local response="${1}"

    printf '%s' "${response%%$'\n'*}"
}

plex_http_body() {
    local response="${1}"

    case "${response}" in
        *$'\n'*)
            printf '%s' "${response#*$'\n'}"
            ;;
        *)
            printf ''
            ;;
    esac
}

plex_activities_has_optimize() {
    local body="${1}"

    case "${body}" in
        *'type="general.db.optimize"'*)
            return 0
            ;;
    esac

    return 1
}

plex_optimize_activity_key() {
    local body="${1}"
    local activity
    local key

    while IFS= read -r activity
    do
        case "${activity}" in
            *'type="general.db.optimize"'*)
                key="$(printf '%s\n' "${activity}" | sed -n 's/.*uuid="\([^"]*\)".*/uuid:\1/p')"
                if [ -z "${key}" ]; then
                    key="$(printf '%s\n' "${activity}" | sed -n 's/.*id="\([^"]*\)".*/id:\1/p')"
                fi
                [ -n "${key}" ] || return 1
                printf '%s' "${key}"
                return 0
                ;;
        esac
    done < <(printf '%s' "${body}" | tr '>' '\n')

    return 1
}

plex_optimize_now() {
    date '+%s'
}

plex_optimize_sleep() {
    local seconds="${1}"

    sleep "${seconds}"
}

plex_deadline_remaining() {
    local deadline="${1}"
    local now

    if ! now="$(plex_optimize_now)"; then
        return 1
    fi
    case "${now}" in
        ""|*[!0-9]*)
            return 1
            ;;
    esac

    printf '%s' $((deadline - now))
}

plex_http_max_time_for_remaining() {
    local remaining="${1}"
    local request_cap=30

    if [ "${remaining}" -lt "${request_cap}" ]; then
        printf '%s' "${remaining}"
    else
        printf '%s' "${request_cap}"
    fi
}

plex_optimize_sleep_before_deadline() {
    local deadline="${1}"
    local poll_seconds="${2}"
    local remaining
    local sleep_seconds

    if ! remaining="$(plex_deadline_remaining "${deadline}")"; then
        return 1
    fi
    if [ "${remaining}" -le 0 ]; then
        return 1
    fi

    sleep_seconds="${poll_seconds}"
    if [ "${remaining}" -lt "${sleep_seconds}" ]; then
        sleep_seconds="${remaining}"
    fi
    if [ "${sleep_seconds}" -le 0 ]; then
        return 1
    fi

    plex_optimize_sleep "${sleep_seconds}"
}

plex_optimize_result() {
    local state="${1}"
    local warned="${2}"

    printf '__PLEX_OPTIMIZE_RESULT__:%s:%s\n' "${state}" "${warned}"
}

mark_plex_optimize_skipped() {
    local instance="${1}"
    local reason="${2}"

    echo "WARNING: Plex optimize ${reason} for ${instance}; skipped" >&2
    plex_optimize_result "skipped" "1"
}

wait_for_plex_identity() {
    local base_url="${1}"
    local identity_url="${base_url}/identity"
    local poll_seconds=2
    local ready_timeout_seconds=60
    local deadline
    local now
    local remaining
    local max_time
    local response
    local status

    if ! now="$(plex_optimize_now)"; then
        return 1
    fi
    deadline=$((now + ready_timeout_seconds))

    while :
    do
        if ! remaining="$(plex_deadline_remaining "${deadline}")"; then
            return 1
        fi
        if [ "${remaining}" -le 0 ]; then
            break
        fi
        max_time="$(plex_http_max_time_for_remaining "${remaining}")"

        if response="$(plex_http_request "GET" "${identity_url}" "" "${max_time}")"; then
            status="$(plex_http_status "${response}")"
            [ "${status}" = "200" ] && return 0
        fi

        if ! plex_optimize_sleep_before_deadline "${deadline}" "${poll_seconds}"; then
            break
        fi
    done

    return 1
}

wait_for_plex_optimize_completion() {
    local instance="${1}"
    local base_url="${2}"
    local token="${3}"
    local expected_activity_key="${4:-}"
    local activities_url="${base_url}/activities"
    local poll_seconds=2
    local timeout_seconds=300
    local startup_polls=3
    local completion_absent_polls_required=2
    local deadline
    local now
    local remaining
    local max_time
    local response
    local status
    local body
    local seen=0
    local startup_absent_polls=0
    local completion_absent_polls=0
    local activity_key=""

    if ! now="$(plex_optimize_now)"; then
        echo "WARNING: Plex optimize accepted but did not start for ${instance}" >&2
        echo "Plex optimize accepted but did not start: ${instance}"
        plex_optimize_result "accepted-but-never-started" "1"
        return 0
    fi
    deadline=$((now + timeout_seconds))

    while :
    do
        if ! plex_optimize_sleep_before_deadline "${deadline}" "${poll_seconds}"; then
            break
        fi
        if ! remaining="$(plex_deadline_remaining "${deadline}")"; then
            break
        fi
        if [ "${remaining}" -le 0 ]; then
            break
        fi
        max_time="$(plex_http_max_time_for_remaining "${remaining}")"

        if ! response="$(plex_http_request "GET" "${activities_url}" "${token}" "${max_time}")"; then
            continue
        fi
        status="$(plex_http_status "${response}")"
        [ "${status}" = "200" ] || continue
        body="$(plex_http_body "${response}")"

        if plex_activities_has_optimize "${body}"; then
            activity_key=""
            if plex_optimize_activity_key "${body}" >/dev/null; then
                activity_key="$(plex_optimize_activity_key "${body}")"
            fi
            if [ -n "${expected_activity_key}" ]; then
                if [ "${activity_key}" = "${expected_activity_key}" ]; then
                    seen=1
                    startup_absent_polls=0
                    completion_absent_polls=0
                fi
                continue
            else
                seen=1
                startup_absent_polls=0
                completion_absent_polls=0
                continue
            fi
        fi

        if [ "${seen}" -eq 1 ]; then
            completion_absent_polls=$((completion_absent_polls + 1))
            if [ "${completion_absent_polls}" -ge "${completion_absent_polls_required}" ]; then
                echo "Plex optimize completed: ${instance}"
                plex_optimize_result "completed" "0"
                return 0
            fi
            continue
        fi

        startup_absent_polls=$((startup_absent_polls + 1))
        if [ "${startup_absent_polls}" -ge "${startup_polls}" ]; then
            echo "WARNING: Plex optimize accepted but did not start for ${instance}" >&2
            echo "Plex optimize accepted but did not start: ${instance}"
            plex_optimize_result "accepted-but-never-started" "1"
            return 0
        fi
    done

    if [ "${seen}" -eq 1 ]; then
        echo "WARNING: Plex optimize completion unconfirmed for ${instance}" >&2
        echo "Plex optimize started but completion unconfirmed: ${instance}"
        plex_optimize_result "started-but-completion-unconfirmed" "1"
        return 0
    fi

    echo "WARNING: Plex optimize accepted but did not start for ${instance}" >&2
    echo "Plex optimize accepted but did not start: ${instance}"
    plex_optimize_result "accepted-but-never-started" "1"
    return 0
}

trigger_plex_optimize_instance_impl() {
    local instance="${1}"
    local preferences_file
    local token
    local container_ip
    local base_url
    local optimize_port=32400
    local activities_url
    local optimize_url
    local response
    local status
    local body
    local expected_activity_key=""

    if ! container_ip="$(resolve_plex_container_ip "${instance}")"; then
        mark_plex_optimize_skipped "${instance}" "container IP unavailable"
        return 0
    fi

    base_url="http://${container_ip}:${optimize_port}"
    if ! wait_for_plex_identity "${base_url}"; then
        mark_plex_optimize_skipped "${instance}" "identity not ready"
        return 0
    fi

    preferences_file="$(plex_preferences_file "${instance}")"
    if ! token="$(extract_plex_online_token "${preferences_file}")"; then
        mark_plex_optimize_skipped "${instance}" "token missing"
        return 0
    fi

    activities_url="${base_url}/activities"
    optimize_url="${base_url}/library/optimize?async=1"

    if ! response="$(plex_http_request "GET" "${activities_url}" "${token}")"; then
        mark_plex_optimize_skipped "${instance}" "activities preflight unreachable"
        return 0
    fi
    status="$(plex_http_status "${response}")"
    body="$(plex_http_body "${response}")"
    if [ "${status}" != "200" ]; then
        mark_plex_optimize_skipped "${instance}" "activities preflight HTTP ${status}"
        return 0
    fi

    if plex_activities_has_optimize "${body}"; then
        echo "Plex optimize already running: ${instance}"
        plex_optimize_result "already-running" "0"
        return 0
    fi

    if ! response="$(plex_http_request "PUT" "${optimize_url}" "${token}")"; then
        mark_plex_optimize_skipped "${instance}" "trigger request failed"
        return 0
    fi
    status="$(plex_http_status "${response}")"
    body="$(plex_http_body "${response}")"
    if [ "${status}" != "200" ]; then
        mark_plex_optimize_skipped "${instance}" "trigger HTTP ${status}"
        return 0
    fi
    if plex_optimize_activity_key "${body}" >/dev/null; then
        expected_activity_key="$(plex_optimize_activity_key "${body}")"
    fi

    echo "Plex optimize accepted: ${instance}"
    wait_for_plex_optimize_completion "${instance}" "${base_url}" "${token}" "${expected_activity_key}"
}

trigger_plex_optimize_instance() {
    local restore_xtrace=0
    local rc

    case "$-" in
        *x*)
            restore_xtrace=1
            set +x
            ;;
    esac

    trigger_plex_optimize_instance_impl "$@"
    rc=$?

    if [ "${restore_xtrace}" -eq 1 ]; then
        set -x
    fi

    return "${rc}"
}

stop_container_for_maintenance() {
    local kind="${1}"
    local instance="${2}"
    local running

    if ! docker stop "${instance}"; then
        echo "WARNING: docker stop failed for ${kind} ${instance}; skipped" >&2
        return 2
    fi

    # WHY: Capturing Docker output outside a pipeline preserves query failure
    # as a planned-downtime gate while still allowing restart after a stop.
    if ! running=$(docker ps --filter "name=^${instance}$" --filter "status=running" --format '{{.Names}}'); then
        echo "ERROR: cannot verify ${instance} is stopped (docker query failed)" >&2
        return 3
    fi
    if [[ -n "${running}" ]]; then
        echo "ERROR: ${instance} appears to be running -- planned-downtime gate failed" >&2
        return 4
    fi

    return 0
}

start_container_after_maintenance() {
    local kind="${1}"
    local instance="${2}"
    local running

    if ! docker start "${instance}"; then
        echo "ERROR: docker start failed for ${kind} ${instance}" >&2
        return 1
    fi
    if ! running=$(docker ps --filter "name=^${instance}$" --filter "status=running" --format '{{.Names}}'); then
        echo "ERROR: cannot verify ${instance} is running after start (docker query failed)" >&2
        return 1
    fi
    if [[ -z "${running}" ]]; then
        echo "ERROR: ${instance} failed to reach running after start" >&2
        return 1
    fi

    return 0
}

run_plex_maintenance_safely() {
    local rc
    local restore_errexit=0
    local blob_pre_swap_hook

    case "$-" in
        *e*)
            restore_errexit=1
            set +e
            ;;
    esac

    (
        set -e

        echo "Cleaning Up Plex Cache: ${plex_path}"
        rm -rf "${plex_path}/Cache/PhotoTranscoder/"*
        rm -rf "${plex_path}/Crash Reports/"*
        rm -rf "${plex_path}/Codecs/"*
        rm -rf "${plex_path}/Plug-in Support/Caches/"*

        optimize_plex_db \
          "${_PLEX_DB}" \
          "SELECT 1 FROM versioned_metadata_items LIMIT 1;" \
          "try_deflate_plex_statistics_bandwidth"

        if [ "${PLEX_PROCESS_BLOB_DB:-0}" = "1" ]; then
            blob_pre_swap_hook=""
            if [ "${PLEX_TRIM_FINISHED_SEASON_BLOBS:-0}" = "1" ]; then
                blob_pre_swap_hook="try_trim_plex_finished_season_blobs"
            fi
            optimize_plex_db "${_PLEX_BLOB_DB}" "" "${blob_pre_swap_hook}" ""
        fi
    )
    rc=$?

    if [ "${restore_errexit}" -eq 1 ]; then
        set -e
    fi

    return "${rc}"
}

run_emby_maintenance_safely() {
    local emby_backup_dir
    local optimize_sql
    local rc
    local restore_errexit=0

    case "$-" in
        *e*)
            restore_errexit=1
            set +e
            ;;
    esac

    (
        set -e

        echo "Optimizing Emby Database: ${emby_path}/${_EMBY_DB}"
        if [ -d "${BACKUP_PATH}" ]; then
          mkdir -p "${BACKUP_PATH}/${emby_instance}/Databases"
          emby_backup_dir="${BACKUP_PATH}/${emby_instance}/Databases"
        else
          emby_backup_dir="${emby_path}"
        fi

        optimize_sql="$(build_emby_optimize_sql)"

        rebuild_db_vacuum_into \
          "${GENERIC_SQLITE_BINARY}" \
          "${emby_path}/${_EMBY_DB}" \
          "${emby_backup_dir}" \
          "${_PAGE_SIZE}" \
          "NONE" \
          "SELECT 1 FROM SyncJobs2 LIMIT 1;" \
          "${optimize_sql}" \
          ""
    )
    rc=$?

    if [ "${restore_errexit}" -eq 1 ]; then
        set -e
    fi

    return "${rc}"
}


print_usage() {
    cat <<'USAGE'
Usage: ./scripts/optimize_media_servers.sh [--help]

Run with no arguments to maintain the Plex and Emby instances configured in:
  scripts/optimize_media_servers.conf

Override the config path with:
  OPTIMIZE_MEDIA_SERVERS_CONF=/path/to/optimize_media_servers.conf

Create the config by copying:
  scripts/optimize_media_servers.conf.example

The config is sourced as Bash and owns:
  PLEX_INSTANCES
  EMBY_INSTANCES
  PLEX_BINARY
  GENERIC_SQLITE_BINARY
  BACKUP_PATH
  PLEX_OPTIMIZE_API
  PLEX_PROCESS_BLOB_DB
  PLEX_TRIM_FINISHED_SEASON_BLOBS
  STATS_BANDWIDTH_RETAIN_DAYS

Each PLEX_INSTANCES or EMBY_INSTANCES entry is both the Docker container name
and the /opt/<instance> filesystem stem. Only literal PLEX_OPTIMIZE_API=1
triggers the post-start Plex optimize API. Only literal PLEX_PROCESS_BLOB_DB=1
enables the Plex blob database rebuild pass. Both literal
PLEX_PROCESS_BLOB_DB=1 and PLEX_TRIM_FINISHED_SEASON_BLOBS=1 enable the fixed
24-month finished-season blob trim in the staged pre-swap hook.

Stage consumed binaries before running:
  release/cli/sqlite3 -> ${HOME}/bin/sqlite3
  patched Plex SQLite plus the matching patched libsqlite3.so runtime copy ->
    ${HOME}/plex-sql/ (PLEX_BINARY points at the patched Plex SQLite)

PLEX_BINARY must resolve the matching patched libsqlite3.so and report the
patched sqlite_source_id() required by this script.
USAGE
}

load_optimize_config() {
    local conf_file="${1}"
    local rc=0
    local failure_kind=""
    local restore_errexit=0
    local restore_xtrace=0
    local restore_functrace=0
    local previous_debug_trap=""
    local config_command_rc=0
    local first_config_failure_rc=0

    case "$-" in
        *e*) restore_errexit=1 ;;
    esac
    case "$-" in
        *x*) restore_xtrace=1 ;;
    esac
    case "$-" in
        *T*) restore_functrace=1 ;;
    esac
    previous_debug_trap="$(trap -p DEBUG)"

    set +ex
    if [ ! -f "${conf_file}" ]; then
        rc=1
        failure_kind="absent"
    else
        set -T
        trap 'config_command_rc=$?; case "${config_command_rc}:${first_config_failure_rc:-0}" in 0:*) ;; *:0) first_config_failure_rc="${config_command_rc}" ;; esac' DEBUG
        # shellcheck source=/dev/null
        . "${conf_file}"
        rc=$?
        trap - DEBUG
        if [ "${restore_functrace}" -eq 1 ]; then
            set -T
        else
            set +T
        fi
        case "${previous_debug_trap}" in
            "")
                ;;
            *)
                eval "${previous_debug_trap}"
                ;;
        esac
        if [ "${first_config_failure_rc}" -ne 0 ]; then
            rc="${first_config_failure_rc}"
            failure_kind="source"
        elif [ "${rc}" -ne 0 ]; then
            failure_kind="source"
        fi
    fi
    set +ex

    if [ "${restore_errexit}" -eq 1 ]; then
        set -e
    fi
    if [ "${restore_xtrace}" -eq 1 ]; then
        set -x
    fi

    case "${failure_kind}" in
        absent)
            echo "ERROR: required config file not found: ${conf_file}" >&2
            echo "ERROR: copy scripts/optimize_media_servers.conf.example to that path, or set OPTIMIZE_MEDIA_SERVERS_CONF; see --help" >&2
            return "${rc}"
            ;;
        source)
            echo "ERROR: failed to source config file: ${conf_file}" >&2
            echo "ERROR: copy scripts/optimize_media_servers.conf.example to that path, or set OPTIMIZE_MEDIA_SERVERS_CONF; see --help" >&2
            return "${rc}"
            ;;
    esac

    return 0
}


main() {
    local final_rc=0
    local maintenance_failure_status=1
    local plex_preflight_drop_status=2
    local emby_preflight_drop_status=4
    local unsupported_argument_status=64
    local plex_optimize_attempted=0
    local plex_optimize_successful=0
    local plex_optimize_accepted=0
    local plex_optimize_already_running=0
    local plex_optimize_completed=0
    local plex_optimize_skipped=0
    local plex_optimize_warned=0
    local plex_stat4_enabled=0
    local stop_rc
    local maintenance_rc
    local trigger_output
    local trigger_rc
    local trigger_result
    local trigger_state
    local trigger_warned
    local trigger_line
    local script_dir
    local conf_file
    local config_rc
    local emby_preflight_out
    local plex_instance
    local plex_path
    local plex_databases_path
    local emby_instance
    local emby_path
    local arg

    if [ "$#" -gt 0 ]; then
        for arg in "$@"; do
            case "${arg}" in
            -h|--help)
                ;;
            *)
                echo "ERROR: unknown argument: ${arg}" >&2
                print_usage >&2
                return "${unsupported_argument_status}"
                ;;
            esac
        done
        print_usage
        return 0
    fi

    script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
    conf_file="${OPTIMIZE_MEDIA_SERVERS_CONF:-${script_dir}/optimize_media_servers.conf}"
    if load_optimize_config "${conf_file}"; then
        config_rc=0
    else
        config_rc=$?
    fi
    if [ "${config_rc}" -ne 0 ]; then
        return "${config_rc}"
    fi

    if [ "${#PLEX_INSTANCES[@]}" -eq 0 ] && [ "${#EMBY_INSTANCES[@]}" -eq 0 ]; then
        echo "WARNING: no Plex or Emby instances configured; nothing to do" >&2
        return 0
    fi

    set -ex
    set -o pipefail

if [ "${#PLEX_INSTANCES[@]}" -gt 0 ]; then
    if ! plex_patched_engine_preflight "${PLEX_BINARY}"; then
        echo "WARNING: patched Plex maintenance engine prerequisite failed; skipping all Plex maintenance" >&2
        echo "WARNING: the per-instance stopped-container gate was NOT evaluated for Plex this run" >&2
        final_rc=$((final_rc | plex_preflight_drop_status))
        PLEX_INSTANCES=()
    elif plex_stat4_preflight "${GENERIC_SQLITE_BINARY}"; then
        plex_stat4_enabled=1
    else
        plex_stat4_enabled=0
    fi
fi

for plex_instance in "${PLEX_INSTANCES[@]}"
do
    plex_path="/opt/${plex_instance}/Library/Application Support/Plex Media Server"
    plex_databases_path="${plex_path}/Plug-in Support/Databases"

    if [ ! -d "${plex_databases_path}" ]; then
        echo "Skipped Missing Plex Instance: ${plex_instance}"
        continue
    fi

    set +e
    stop_container_for_maintenance "Plex" "${plex_instance}"
    stop_rc=$?
    set -e
    case "${stop_rc}" in
        0)
            ;;
        3)
            final_rc=$((final_rc | maintenance_failure_status))
            if ! start_container_after_maintenance "Plex" "${plex_instance}"; then
                final_rc=$((final_rc | maintenance_failure_status))
            fi
            continue
            ;;
        *)
            final_rc=$((final_rc | maintenance_failure_status))
            continue
            ;;
    esac

    set +e
    run_plex_maintenance_safely
    maintenance_rc=$?
    set -e
    if [ "${maintenance_rc}" -ne 0 ]; then
        echo "WARNING: Plex maintenance failed for ${plex_instance}; restarting container and continuing" >&2
        final_rc=$((final_rc | maintenance_failure_status))
    fi

    if ! start_container_after_maintenance "Plex" "${plex_instance}"; then
        final_rc=$((final_rc | maintenance_failure_status))
        continue
    fi

    if [ "${maintenance_rc}" -ne 0 ]; then
        continue
    fi

    if [ "${PLEX_OPTIMIZE_API:-0}" = "1" ]; then
        plex_optimize_attempted=$((plex_optimize_attempted + 1))
        set +e
        trigger_output="$(trigger_plex_optimize_instance "${plex_instance}")"
        trigger_rc=$?
        set -e
        trigger_result=""
        if [ -n "${trigger_output}" ]; then
            while IFS= read -r trigger_line
            do
                case "${trigger_line}" in
                    __PLEX_OPTIMIZE_RESULT__:*)
                        trigger_result="${trigger_line#__PLEX_OPTIMIZE_RESULT__:}"
                        ;;
                    *)
                        printf '%s\n' "${trigger_line}"
                        ;;
                esac
            done <<< "${trigger_output}"
        fi
        if [ "${trigger_rc}" -ne 0 ]; then
            trigger_state="skipped"
            trigger_warned=1
            echo "WARNING: Plex optimize internal failure for ${plex_instance}; skipped" >&2
        elif [ -n "${trigger_result}" ]; then
            trigger_state="${trigger_result%%:*}"
            trigger_warned="${trigger_result#*:}"
            case "${trigger_warned}" in
                0|1)
                    ;;
                *)
                    trigger_state="skipped"
                    trigger_warned=1
                    echo "WARNING: Plex optimize malformed terminal state for ${plex_instance}; skipped" >&2
                    ;;
            esac
        else
            trigger_state="skipped"
            trigger_warned=1
            echo "WARNING: Plex optimize missing terminal state for ${plex_instance}; skipped" >&2
        fi

        if [ "${trigger_warned}" -eq 1 ]; then
            plex_optimize_warned=$((plex_optimize_warned + 1))
        fi
        case "${trigger_state}" in
            completed)
                plex_optimize_completed=$((plex_optimize_completed + 1))
                plex_optimize_successful=$((plex_optimize_successful + 1))
                ;;
            already-running)
                plex_optimize_already_running=$((plex_optimize_already_running + 1))
                plex_optimize_successful=$((plex_optimize_successful + 1))
                ;;
            accepted-but-never-started|started-but-completion-unconfirmed)
                plex_optimize_accepted=$((plex_optimize_accepted + 1))
                plex_optimize_successful=$((plex_optimize_successful + 1))
                ;;
            skipped|"")
                plex_optimize_skipped=$((plex_optimize_skipped + 1))
                ;;
            *)
                plex_optimize_skipped=$((plex_optimize_skipped + 1))
                plex_optimize_warned=$((plex_optimize_warned + 1))
                echo "WARNING: Plex optimize unknown terminal state for ${plex_instance}; skipped" >&2
                ;;
        esac
    fi
done

if [ "${PLEX_OPTIMIZE_API:-0}" = "1" ] && [ "${plex_optimize_attempted}" -gt 0 ]; then
    echo "Plex optimize summary: accepted=${plex_optimize_accepted} already-running=${plex_optimize_already_running} completed=${plex_optimize_completed} skipped=${plex_optimize_skipped} warned=${plex_optimize_warned}"
    if [ "${plex_optimize_successful}" -eq 0 ]; then
        final_rc=$((final_rc | maintenance_failure_status))
    fi
fi


if [ "${#EMBY_INSTANCES[@]}" -gt 0 ]; then
    if ! emby_preflight_out=$("${GENERIC_SQLITE_BINARY}" ":memory:" "SELECT 1;" 2>&1); then
        echo "WARNING: ${GENERIC_SQLITE_BINARY} pre-flight failed; skipping all Emby maintenance" >&2
        echo "WARNING: pre-flight output: ${emby_preflight_out}" >&2
        echo "WARNING: the per-instance stopped-container gate was NOT evaluated for Emby this run" >&2
        final_rc=$((final_rc | emby_preflight_drop_status))
        EMBY_INSTANCES=()
    fi
fi

for emby_instance in "${EMBY_INSTANCES[@]}"
do
    emby_path="/opt/${emby_instance}/data"
    if [ ! -d "${emby_path}" ]; then
        echo "Skipped Missing Emby Instance: ${emby_instance}"
        continue
    fi

    set +e
    stop_container_for_maintenance "Emby" "${emby_instance}"
    stop_rc=$?
    set -e
    case "${stop_rc}" in
        0)
            ;;
        3)
            final_rc=$((final_rc | maintenance_failure_status))
            if ! start_container_after_maintenance "Emby" "${emby_instance}"; then
                final_rc=$((final_rc | maintenance_failure_status))
            fi
            continue
            ;;
        *)
            final_rc=$((final_rc | maintenance_failure_status))
            continue
            ;;
    esac

    set +e
    run_emby_maintenance_safely
    maintenance_rc=$?
    set -e
    if [ "${maintenance_rc}" -ne 0 ]; then
        echo "WARNING: Emby maintenance failed for ${emby_instance}; restarting container and continuing" >&2
        final_rc=$((final_rc | maintenance_failure_status))
    fi

    if ! start_container_after_maintenance "Emby" "${emby_instance}"; then
        final_rc=$((final_rc | maintenance_failure_status))
        continue
    fi
done


set +ex
return "${final_rc}"
}

if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    main "$@"
fi
