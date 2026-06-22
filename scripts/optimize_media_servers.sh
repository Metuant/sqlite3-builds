#!/usr/bin/env bash

set -ex
set -o pipefail

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

run_post_swap_sql() {
    local binary="${1}"
    local db_path="${2}"
    local step_name="${3}"
    local sql="${4}"

    if ! "${binary}" "${db_path}" "${sql}"; then
        echo "WARNING: ${step_name} failed for ${db_path}; DB was already validated and swapped; continuing" >&2
    fi
}

rebuild_db_vacuum_into() {
    local binary="${1}"
    local db_path="${2}"
    local backup_dir="${3}"
    local page_size="${4}"
    local auto_vacuum_mode="${5}"
    local sanity_sql="${6}"
    local optimize_sql="${7}"
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

    mv "${staged_db}" "${db_path}"
    rm -f "${db_path}-wal" "${db_path}-shm" "${staged_db}-wal" "${staged_db}-shm"
    if [ -n "${optimize_sql}" ]; then
        run_post_swap_sql "${binary}" "${db_path}" "post-swap maintenance SQL" "${optimize_sql}"
    fi
}

optimize_plex_db() {
    local db_file="${1}"
    local sanity_sql="${2}"
    local fts_sql="${3}"
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
    if [ -n "${fts_sql}" ]; then
        optimize_sql="${optimize_sql} ${fts_sql}"
    fi

    rebuild_db_vacuum_into \
      "${PLEX_BINARY}" \
      "${db_path}" \
      "${backup_dir}" \
      "${PAGE_SIZE}" \
      "NONE" \
      "${sanity_sql}" \
      "${optimize_sql}"
}


# mkdir -p ${HOME}/plex-sql/
# rm -vrf ${HOME}/plex-sql/lib/
# docker cp plex:"/usr/lib/plexmediaserver/lib/" ${HOME}/plex-sql/lib/
# WHY: If lib/ came from a modded container, restore Plex's bundled SQLite
# library beside Plex SQLite so its source-id guard matches; the deployed
# runtime libsqlite3.so in the container remains untouched.
# cp "${HOME}/plex-sql/lib/libsqlite3.so.bundled.bak" "${HOME}/plex-sql/lib/libsqlite3.so"
# docker cp plex:"/usr/lib/plexmediaserver/Plex SQLite" ${HOME}/plex-sql/
# docker cp plex:"/usr/lib/plexmediaserver/Plex Media Server" ${HOME}/plex-sql/


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

    # docker stop ${PLEX_INSTANCE}

    echo "Cleaning Up Plex Cache: ${PLEX_PATH}"
    rm -rf "${PLEX_PATH}/Cache/PhotoTranscoder/"*
    rm -rf "${PLEX_PATH}/Crash Reports/"*
    rm -rf "${PLEX_PATH}/Codecs/"*
    rm -rf "${PLEX_PATH}/Plug-in Support/Caches/"*

    optimize_plex_db \
      "${PLEX_DB}" \
      "SELECT 1 FROM versioned_metadata_items LIMIT 1;" \
      "INSERT INTO fts4_metadata_titles(fts4_metadata_titles) VALUES('optimize'); INSERT INTO fts4_metadata_titles_icu(fts4_metadata_titles_icu) VALUES('optimize'); INSERT INTO fts4_tag_titles(fts4_tag_titles) VALUES('optimize'); INSERT INTO fts4_tag_titles_icu(fts4_tag_titles_icu) VALUES('optimize');"

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


for EMBY_INSTANCE in "${EMBY_INSTANCES[@]}"
do
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

    # docker stop ${EMBY_INSTANCE}

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
      "REINDEX; PRAGMA analysis_limit=0; PRAGMA optimize=0x10002; INSERT INTO fts_search9(fts_search9) VALUES('optimize');"

    # docker start ${EMBY_INSTANCE}
done


set +ex
