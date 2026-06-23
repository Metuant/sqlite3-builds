#!/usr/bin/env bash

export SQLITE3_DISABLE_AUTOPRAGMA=1

# Maintenance assumes planned downtime. Stop each container explicitly before
# running this script; the pre-flight gates verify that state, while container
# start/stop stays operator-owned.
# NOTE: Jellyfin maintenance is dormant in this cycle.
# WHY: JF maintenance stays absent until schema, page-size, FTS, and stopped-container
# gates are re-validated.
# This script intentionally covers only Plex and Emby.
PAGE_SIZE="16384"
BACKUP_PATH="/mnt/media-backup"
STATS_BANDWIDTH_RETAIN_DAYS="90"

declare -a PLEX_INSTANCES=()
declare -a EMBY_INSTANCES=()

PLEX_BINARY="${HOME}/plex-sql/Plex SQLite"
PLEX_DB="com.plexapp.plugins.library.db"
PLEX_BLOB_DB="com.plexapp.plugins.library.blobs.db"
PLEX_PROCESS_BLOB_DB="${PLEX_PROCESS_BLOB_DB:-0}"

EMBY_BINARY="${HOME}/bin/sqlite3"
EMBY_DB="library.db"

cleanup_staged_db() {
    local staged_db="${1}"

    rm -f "${staged_db}" "${staged_db}-wal" "${staged_db}-shm"
}

quote_sql_ident() {
    local ident="${1}"

    printf '"%s"' "${ident//\"/\"\"}"
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

run_foreign_key_check_warn() {
    local binary="${1}"
    local db_path="${2}"
    local fk_rows

    if ! fk_rows=$("${binary}" "${db_path}" "PRAGMA foreign_key_check;" | tr -d '\r'); then
        echo "WARNING: foreign_key_check failed to run for ${db_path}; continuing after source integrity and FTS gates" >&2
        return 0
    fi
    if [ -n "${fk_rows}" ]; then
        echo "WARNING: foreign_key_check returned rows for ${db_path}; continuing after source integrity and FTS gates" >&2
        printf '%s\n' "${fk_rows}" >&2
    fi
    return 0
}

run_post_swap_sql() {
    local binary="${1}"
    local db_path="${2}"
    local step_name="${3}"
    local sql="${4}"

    if ! "${binary}" "${db_path}" "${sql}"; then
        echo "WARNING: ${step_name} failed for ${db_path}; DB was already validated and swapped; continuing" >&2
    fi
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
    local rebuild_sql
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
        rebuild_sql="INSERT INTO ${table_ident}(${table_ident}) VALUES('rebuild');"
        optimize_sql="INSERT INTO ${table_ident}(${table_ident}) VALUES('optimize');"

        if ! { [ "${module}" = "fts5" ] && [ "${content_mode}" = "contentless" ]; }; then
            if ! "${binary}" "${db_path}" "${rebuild_sql}"; then
                echo "WARNING: post-swap FTS rebuild failed for ${table_name} in ${db_path}; continuing" >&2
                continue
            fi
        fi
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
    local post_integrity

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
    if ! "${binary}" "${staged_db}" "VACUUM;"; then
        echo "WARNING: post-deflate VACUUM failed for ${staged_db}; continuing to post-deflate integrity gate" >&2
    fi
    if ! post_integrity=$("${binary}" "${staged_db}" "PRAGMA integrity_check(1);" | tr -d '\r\n'); then
        echo "ERROR: post-deflate integrity_check(1) failed to run for ${staged_db}; live DB has NOT been touched" >&2
        return 1
    fi
    if [ "${post_integrity}" != "ok" ]; then
        echo "ERROR: post-deflate DB failed integrity_check(1): ${post_integrity}; live DB has NOT been touched" >&2
        return 1
    fi

    return 0
}

rebuild_db_vacuum_into() {
    local binary="${1}"
    local db_path="${2}"
    local backup_dir="${3}"
    local page_size="${4}"
    local auto_vacuum_mode="${5}"
    local sanity_sql="${6}"
    local optimize_sql="${7}"
    local pre_swap_hook="${8:-}"
    local db_file="${db_path##*/}"
    local backup_path
    local backup_tmp
    local staged_db="${db_path}.new"
    local auto_vacuum_expected=""
    local auto_vacuum_actual=""
    local backup_date
    local new_mode
    local needs_fixup=0
    local source_integrity
    local staged_integrity
    local staged_page_size
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

    if [ -n "${sanity_sql}" ]; then
        "${binary}" "${db_path}" "${sanity_sql}"
    fi
    if ! source_integrity=$("${binary}" "${db_path}" "PRAGMA integrity_check(1);" | tr -d '\r\n'); then
        echo "ERROR: source integrity_check(1) failed to run for ${db_path}; live DB has NOT been touched" >&2
        exit 1
    fi
    if [ "${source_integrity}" != "ok" ]; then
        echo "ERROR: source DB failed integrity_check(1): ${source_integrity}; live DB has NOT been touched" >&2
        exit 1
    fi
    if ! run_fts_integrity_gate "${binary}" "${db_path}" "source" "warn"; then
        echo "ERROR: source FTS metadata is unsafe or unclassified for ${db_path}; live DB has NOT been touched" >&2
        exit 1
    fi
    run_foreign_key_check_warn "${binary}" "${db_path}"

    # Fold WAL data into the main file before the byte-for-byte filesystem backup.
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
          "PRAGMA page_size=${page_size};" \
          "PRAGMA auto_vacuum=${auto_vacuum_mode};" \
          "VACUUM INTO '${staged_db}';"; then
            echo "ERROR: VACUUM INTO failed for ${db_path}; live DB has NOT been touched" >&2
            cleanup_staged_db "${staged_db}"
            exit 1
        fi
    else
        if ! "${binary}" "${db_path}" \
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
              "PRAGMA page_size=${page_size};" \
              "PRAGMA auto_vacuum=${auto_vacuum_mode};" \
              "VACUUM;"; then
                echo "ERROR: staged page_size/auto_vacuum fix-up failed for ${staged_db}; live DB has NOT been touched" >&2
                cleanup_staged_db "${staged_db}"
                exit 1
            fi
        else
            if ! "${binary}" "${staged_db}" \
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

    if ! staged_integrity=$("${binary}" "${staged_db}" "PRAGMA integrity_check(1);" | tr -d '\r\n'); then
        echo "ERROR: integrity check failed to run on staged DB; live DB has NOT been touched" >&2
        cleanup_staged_db "${staged_db}"
        exit 1
    fi
    if [ "${staged_integrity}" != "ok" ]; then
        echo "ERROR: staged DB failed integrity_check(1): ${staged_integrity}; live DB has NOT been touched" >&2
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

    mv "${staged_db}" "${db_path}"
    rm -f "${db_path}-wal" "${db_path}-shm" "${staged_db}-wal" "${staged_db}-shm"
    if [ -n "${optimize_sql}" ]; then
        run_post_swap_sql "${binary}" "${db_path}" "post-swap maintenance SQL" "${optimize_sql}"
    fi
    run_post_swap_fts_maintenance "${binary}" "${db_path}"
}

optimize_plex_db() {
    local db_file="${1}"
    local sanity_sql="${2}"
    local pre_swap_hook="${3:-}"
    local db_path="${PLEX_DATABASES_PATH}/${db_file}"
    local backup_dir
    local optimize_sql

    echo "Optimizing Plex Database: ${db_path}"
    if [ -d "${BACKUP_PATH}" ]; then
      mkdir -p "${BACKUP_PATH}/${PLEX_INSTANCE}"
      backup_dir="${BACKUP_PATH}/${PLEX_INSTANCE}"
    else
      backup_dir="${PLEX_DATABASES_PATH}"
    fi

    # NOTE: The 0x10000 bit of PRAGMA optimize=0x10002 is inert on Plex SQLite 3.39.4.
    optimize_sql="PRAGMA cache_size=-1048576; PRAGMA temp_store=2; PRAGMA threads=8; REINDEX; PRAGMA analysis_limit=0; PRAGMA optimize=0x10002;"

    rebuild_db_vacuum_into \
      "${PLEX_BINARY}" \
      "${db_path}" \
      "${backup_dir}" \
      "${PAGE_SIZE}" \
      "NONE" \
      "${sanity_sql}" \
      "${optimize_sql}" \
      "${pre_swap_hook}"
}


# mkdir -p ${HOME}/plex-sql/
# rm -vrf ${HOME}/plex-sql/lib/
# docker cp plex:"/usr/lib/plexmediaserver/lib/" ${HOME}/plex-sql/lib/
# # WHY: If lib/ came from a modded container, restore Plex's bundled SQLite
# # library beside Plex SQLite so its source-id guard matches; the deployed
# # runtime libsqlite3.so in the container remains untouched.
# cp -vf "${HOME}/plex-sql/lib/libsqlite3.so.bundled.bak" "${HOME}/plex-sql/lib/libsqlite3.so"
# docker cp plex:"/usr/lib/plexmediaserver/Plex SQLite" ${HOME}/plex-sql/
# docker cp plex:"/usr/lib/plexmediaserver/Plex Media Server" ${HOME}/plex-sql/


main() {
    set -ex
    set -o pipefail

if [ "${#PLEX_INSTANCES[@]}" -gt 0 ]; then
    if ! plex_preflight_out=$("${PLEX_BINARY}" ":memory:" "SELECT 1;" 2>&1); then
        echo "WARNING: ${PLEX_BINARY} pre-flight failed; skipping all Plex maintenance" >&2
        echo "WARNING: pre-flight output: ${plex_preflight_out}" >&2
        echo "WARNING: likely cause: Plex SQLite/libsqlite3.so source-id mismatch in ${HOME}/plex-sql/lib" >&2
        echo "WARNING: if so, copy ${HOME}/plex-sql/lib/libsqlite3.so.bundled.bak to ${HOME}/plex-sql/lib/libsqlite3.so in the staging copy and retry; the deployed container runtime lib is unchanged" >&2
        echo "WARNING: the per-instance stopped-container gate was NOT evaluated for Plex this run" >&2
        PLEX_INSTANCES=()
    fi
fi

for PLEX_INSTANCE in "${PLEX_INSTANCES[@]}"
do
    # docker stop ${PLEX_INSTANCE}

    # WHY: Capturing Docker output outside a pipeline preserves query failure
    # as a hard planned-downtime gate.
    if ! running=$(docker ps --filter "name=^${PLEX_INSTANCE}$" --filter "status=running" --format '{{.Names}}'); then
        echo "ERROR: cannot verify ${PLEX_INSTANCE} is stopped (docker query failed)" >&2
        exit 1
    fi
    if [[ -n "${running}" ]]; then
        echo "ERROR: ${PLEX_INSTANCE} appears to be running -- planned-downtime gate failed" >&2
        exit 1
    fi

    PLEX_PATH="/opt/${PLEX_INSTANCE}/Library/Application Support/Plex Media Server"
    PLEX_DATABASES_PATH="${PLEX_PATH}/Plug-in Support/Databases"

    if [ ! -d "${PLEX_DATABASES_PATH}" ]; then
        echo "Skipped Missing Plex Instance: ${PLEX_INSTANCE}"
        continue
    fi

    echo "Cleaning Up Plex Cache: ${PLEX_PATH}"
    rm -rf "${PLEX_PATH}/Cache/PhotoTranscoder/"*
    rm -rf "${PLEX_PATH}/Crash Reports/"*
    rm -rf "${PLEX_PATH}/Codecs/"*
    rm -rf "${PLEX_PATH}/Plug-in Support/Caches/"*

    optimize_plex_db \
      "${PLEX_DB}" \
      "SELECT 1 FROM versioned_metadata_items LIMIT 1;" \
      "try_deflate_plex_statistics_bandwidth"

    echo "Fixing Recently Added"
    run_post_swap_sql "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "Plex originally_available_at lower-bound repair" "UPDATE metadata_items SET originally_available_at = STRFTIME('%s', 'now', '-7 days') WHERE originally_available_at < STRFTIME('%s', 'now', '-200 years');"
    run_post_swap_sql "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "Plex originally_available_at upper-bound repair" "UPDATE metadata_items SET originally_available_at = STRFTIME('%s', 'now', '-3 days') WHERE originally_available_at > STRFTIME('%s', 'now');"
    run_post_swap_sql "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "Plex added_at lower-bound repair" "UPDATE metadata_items SET added_at = originally_available_at WHERE added_at <> originally_available_at AND originally_available_at IS NOT NULL AND added_at < STRFTIME('%s', 'now', '-200 years');"
    run_post_swap_sql "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "Plex added_at upper-bound repair" "UPDATE metadata_items SET added_at = originally_available_at WHERE added_at <> originally_available_at AND originally_available_at IS NOT NULL AND added_at > STRFTIME('%s', 'now');"

    if [ "${PLEX_PROCESS_BLOB_DB}" = "1" ]; then
        optimize_plex_db "${PLEX_BLOB_DB}" "" ""
    fi

    # docker start ${PLEX_INSTANCE}
done


if [ "${#EMBY_INSTANCES[@]}" -gt 0 ]; then
    if ! emby_preflight_out=$("${EMBY_BINARY}" ":memory:" "SELECT 1;" 2>&1); then
        echo "WARNING: ${EMBY_BINARY} pre-flight failed; skipping all Emby maintenance" >&2
        echo "WARNING: pre-flight output: ${emby_preflight_out}" >&2
        echo "WARNING: the per-instance stopped-container gate was NOT evaluated for Emby this run" >&2
        EMBY_INSTANCES=()
    fi
fi

for EMBY_INSTANCE in "${EMBY_INSTANCES[@]}"
do
    # docker stop ${EMBY_INSTANCE}

    if ! running=$(docker ps --filter "name=^${EMBY_INSTANCE}$" --filter "status=running" --format '{{.Names}}'); then
        echo "ERROR: cannot verify ${EMBY_INSTANCE} is stopped (docker query failed)" >&2
        exit 1
    fi
    if [[ -n "${running}" ]]; then
        echo "ERROR: ${EMBY_INSTANCE} appears to be running -- planned-downtime gate failed" >&2
        exit 1
    fi

    EMBY_PATH="/opt/${EMBY_INSTANCE}/data"
    if [ ! -d "${EMBY_PATH}" ]; then
        echo "Skipped Missing Emby Instance: ${EMBY_INSTANCE}"
        continue
    fi

    echo "Optimizing Emby Database: ${EMBY_PATH}/${EMBY_DB}"
    if [ -d "${BACKUP_PATH}" ]; then
      mkdir -p "${BACKUP_PATH}/${EMBY_INSTANCE}/Databases"
      emby_backup_dir="${BACKUP_PATH}/${EMBY_INSTANCE}/Databases"
    else
      emby_backup_dir="${EMBY_PATH}"
    fi
    rebuild_db_vacuum_into \
      "${EMBY_BINARY}" \
      "${EMBY_PATH}/${EMBY_DB}" \
      "${emby_backup_dir}" \
      "${PAGE_SIZE}" \
      "NONE" \
      "SELECT 1 FROM SyncJobs2 LIMIT 1;" \
      "REINDEX; PRAGMA analysis_limit=0; PRAGMA optimize=0x10002;" \
      ""

    # docker start ${EMBY_INSTANCE}
done


set +ex
}

if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    main "$@"
fi
