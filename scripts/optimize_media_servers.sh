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
BACKUP_SQL="dump.sql"
PAGE_SIZE="16384"
BACKUP_PATH="/mnt/media-backup"

# declare -a PLEX_INSTANCES=("plex-metadata" "plex-int-metadata" "plex-misc-metadata" "plex-1" "plex-int-1" "plex-misc-1" "plex-audiobooks-1" "plex-2" "plex-int-2" "plex-misc-2" "plex-audiobooks-2" "plex-adult" "plex-backup-1" "plex-backup-2" "plex-backup-3" "plex-backup-metadata")
declare -a PLEX_INSTANCES=()
# declare -a EMBY_INSTANCES=("emby-metadata" "emby-int-metadata" "emby-misc-metadata" "emby-1" "emby-misc-1" "emby-2" "emby-misc-2" "emby-trial" "emby-misc-trial" "emby-backup-1" "emby-backup-2" "emby-backup-3" "emby-backup-metadata")
declare -a EMBY_INSTANCES=()

PLEX_BINARY="/home/darthshadow/plex-sql/Plex SQLite"
HOST_SQLITE3="${HOST_SQLITE3:-${HOME}/bin/sqlite3}"
PLEX_DB="com.plexapp.plugins.library.db"
# Blob DB maintenance remains dormant with the commented block below.
# shellcheck disable=SC2034
PLEX_BLOB_DB="com.plexapp.plugins.library.blobs.db"

EMBY_BINARY="/home/darthshadow/bin/sqlite3"
EMBY_DB="library.db"


# mkdir -p /home/darthshadow/plex-sql/
# rm -vrf /home/darthshadow/plex-sql/lib/

# docker cp plex-metadata:"/usr/lib/plexmediaserver/lib/" /home/darthshadow/plex-sql/lib/
# docker cp plex-1:"/usr/lib/plexmediaserver/lib/" /home/darthshadow/plex-sql/lib/

# docker cp plex-metadata:"/usr/lib/plexmediaserver/Plex SQLite" /home/darthshadow/plex-sql/
# docker cp plex-1:"/usr/lib/plexmediaserver/Plex SQLite" /home/darthshadow/plex-sql/

# docker cp plex-metadata:"/usr/lib/plexmediaserver/Plex Media Server" /home/darthshadow/plex-sql/
# docker cp plex-1:"/usr/lib/plexmediaserver/Plex Media Server" /home/darthshadow/plex-sql/


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

    echo "Optimizing Plex Database: ${PLEX_DATABASES_PATH}/${PLEX_DB}"
    "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "SELECT 1 FROM versioned_metadata_items LIMIT 1;"
    "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "PRAGMA integrity_check(1);"
    if [ -d "${BACKUP_PATH}" ]; then
      mkdir -p "${BACKUP_PATH}/${PLEX_INSTANCE}"
      cp "${PLEX_DATABASES_PATH}/${PLEX_DB}" "${BACKUP_PATH}/${PLEX_INSTANCE}/${PLEX_DB}-$(date '+%Y-%m-%d').original"
    else
      cp "${PLEX_DATABASES_PATH}/${PLEX_DB}" "${PLEX_DATABASES_PATH}/${PLEX_DB}-$(date '+%Y-%m-%d').original"
    fi
    # "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "DROP INDEX IF EXISTS 'index_title_sort_naturalsort';"
    # "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "DELETE from schema_migrations where version='20180501000000';"
    "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" ".output \"${PLEX_DATABASES_PATH}/${BACKUP_SQL}\"" ".dump"
    # "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" ".output \"${PLEX_DATABASES_PATH}/${BACKUP_SQL}\"" ".recover"
    if grep -q '^ROLLBACK;' "${PLEX_DATABASES_PATH}/${BACKUP_SQL}"; then
        echo "ERROR: dump ended in ROLLBACK -- original DB has NOT been touched"
        grep -B5 '^ROLLBACK;' "${PLEX_DATABASES_PATH}/${BACKUP_SQL}" | tail -20
        echo "Inspect the dump and rerun the dump step; do NOT proceed to rm/recreate"
        exit 1
    fi

    rm -f "${PLEX_DATABASES_PATH}/${PLEX_DB}"
    "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" ".read \"${PLEX_DATABASES_PATH}/${BACKUP_SQL}\""
    DB="${PLEX_DATABASES_PATH}/${PLEX_DB}"

    if [[ ! -x "${HOST_SQLITE3}" ]]; then
        echo "WARNING: ${HOST_SQLITE3} not found or not executable; skipping page-size migration" >&2
        page_size_status="skipped-missing-host-sqlite3"
    else
        page_size_status="ok"
        # WHY: Page-size migration is optional; integrity, REINDEX, optimize,
        # and FTS maintenance still run if this sub-step fails.
        if ! (
            set -e

            # Capture the original journal_mode so we can restore it after the migration.
            ORIG_MODE=$("${HOST_SQLITE3}" "${DB}" "PRAGMA journal_mode;" | tr -d '\n')

            # PRAGMA page_size requires the DB to NOT be in WAL. The host CLI does not run
            # PMS init, so this switch is not re-overridden by `Plex SQLite`'s per-connect
            # WAL setter.
            "${HOST_SQLITE3}" "${DB}" "PRAGMA journal_mode=DELETE;" >/dev/null

            # Page-size and auto_vacuum changes both require a full VACUUM to take effect;
            # combine them in one rebuild.
            "${HOST_SQLITE3}" "${DB}" \
              "PRAGMA page_size=16384; PRAGMA auto_vacuum=INCREMENTAL; VACUUM;"

            # Verify post-condition. If page_size didn't take, warn loudly but continue.
            NEW_PS=$("${HOST_SQLITE3}" "${DB}" "PRAGMA page_size;" | tr -d '\n')
            if [ "${NEW_PS}" != "16384" ]; then
                echo "WARNING: page_size migration silently no-op'd on ${DB}"
                echo "         current page_size=${NEW_PS} (expected 16384)"
                echo "         the rest of maintenance will continue; re-run page-size"
                echo "         migration manually after investigating"
            fi

            # Restore the original journal_mode (which is what the application expects).
            "${HOST_SQLITE3}" "${DB}" "PRAGMA journal_mode=${ORIG_MODE};" >/dev/null
        ); then
            # Status is kept for operator diagnostics while the script continues.
            # shellcheck disable=SC2034
            page_size_status="failed"
            echo "WARNING: Plex page-size migration failed; continuing with remaining maintenance" >&2
            echo "WARNING: remaining maintenance runs at the existing page size; investigate before retrying page-size migration" >&2
        fi
    fi

    "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "PRAGMA integrity_check(1);"
    "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "REINDEX; PRAGMA analysis_limit=0; PRAGMA optimize=0x10002; INSERT INTO fts4_metadata_titles(fts4_metadata_titles) VALUES('optimize'); INSERT INTO fts4_metadata_titles_icu(fts4_metadata_titles_icu) VALUES('optimize'); INSERT INTO fts4_tag_titles(fts4_tag_titles) VALUES('optimize'); INSERT INTO fts4_tag_titles_icu(fts4_tag_titles_icu) VALUES('optimize');"
    rm -f "${PLEX_DATABASES_PATH}/${BACKUP_SQL}"

    # echo "Optimizing Plex BLOB Database: ${PLEX_DATABASES_PATH}/${PLEX_BLOB_DB}"
    # if [ -d "${BACKUP_PATH}" ]; then
    #   mkdir -p "${BACKUP_PATH}/${PLEX_INSTANCE}"
    #   cp "${PLEX_DATABASES_PATH}/${PLEX_BLOB_DB}" "${BACKUP_PATH}/${PLEX_INSTANCE}/${PLEX_BLOB_DB}-$(date '+%Y-%m-%d').original"
    # else
    #   cp "${PLEX_DATABASES_PATH}/${PLEX_BLOB_DB}" "${PLEX_DATABASES_PATH}/${PLEX_BLOB_DB}-$(date '+%Y-%m-%d').original"
    # fi
    # "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_BLOB_DB}" "PRAGMA integrity_check(1);"
    # "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_BLOB_DB}" ".output \"${PLEX_DATABASES_PATH}/${BACKUP_SQL}\"" ".dump"
    # # "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_BLOB_DB}" ".output \"${PLEX_DATABASES_PATH}/${BACKUP_SQL}\"" ".recover"
    # rm -f "${PLEX_DATABASES_PATH}/${PLEX_BLOB_DB}"
    # "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_BLOB_DB}" "PRAGMA page_size; PRAGMA page_size=${PAGE_SIZE}; VACUUM; PRAGMA page_size;"
    # sed -i -e 's/ROLLBACK;/COMMIT;/' "${PLEX_DATABASES_PATH}/${BACKUP_SQL}"
    # "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_BLOB_DB}" ".read \"${PLEX_DATABASES_PATH}/${BACKUP_SQL}\""
    # "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_BLOB_DB}" "PRAGMA integrity_check(1);"
    # "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_BLOB_DB}" "PRAGMA analysis_limit=0; ANALYZE; PRAGMA optimize; REINDEX;"
    # rm -f "${PLEX_DATABASES_PATH}/${BACKUP_SQL}"

    echo "Fixing Recently Added"
    "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "UPDATE metadata_items SET originally_available_at = STRFTIME('%s', 'now', '-7 days') WHERE originally_available_at < STRFTIME('%s', 'now', '-200 years');"
    "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "UPDATE metadata_items SET originally_available_at = STRFTIME('%s', 'now', '-3 days') WHERE originally_available_at > STRFTIME('%s', 'now');"
    "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "UPDATE metadata_items SET added_at = originally_available_at WHERE added_at <> originally_available_at AND originally_available_at IS NOT NULL AND added_at < STRFTIME('%s', 'now', '-200 years');"
    "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "UPDATE metadata_items SET added_at = originally_available_at WHERE added_at <> originally_available_at AND originally_available_at IS NOT NULL AND added_at > STRFTIME('%s', 'now');"

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
    "${EMBY_BINARY}" "${EMBY_PATH}/${EMBY_DB}" "SELECT 1 FROM SyncJobs2 LIMIT 1;"
    "${EMBY_BINARY}" "${EMBY_PATH}/${EMBY_DB}" "PRAGMA integrity_check(1);"
    if [ -d "${BACKUP_PATH}" ]; then
      mkdir -p "${BACKUP_PATH}/${EMBY_INSTANCE}/Databases"
      cp "${EMBY_PATH}/${EMBY_DB}" "${BACKUP_PATH}/${EMBY_INSTANCE}/Databases/${EMBY_DB}-$(date '+%Y-%m-%d').original"
    else
      cp "${EMBY_PATH}/${EMBY_DB}" "${EMBY_PATH}/${EMBY_DB}-$(date '+%Y-%m-%d').original"
    fi
    "${EMBY_BINARY}" "${EMBY_PATH}/${EMBY_DB}" ".output \"${EMBY_PATH}/${BACKUP_SQL}\"" .dump
    if grep -q '^ROLLBACK;' "${EMBY_PATH}/${BACKUP_SQL}"; then
        echo "ERROR: dump ended in ROLLBACK -- original DB has NOT been touched"
        grep -B5 '^ROLLBACK;' "${EMBY_PATH}/${BACKUP_SQL}" | tail -20
        echo "Inspect the dump and rerun the dump step; do NOT proceed to rm/recreate"
        exit 1
    fi

    rm -f "${EMBY_PATH}/${EMBY_DB}"
    "${EMBY_BINARY}" "${EMBY_PATH}/${EMBY_DB}" "PRAGMA page_size; PRAGMA page_size=${PAGE_SIZE}; VACUUM; PRAGMA page_size;"
    "${EMBY_BINARY}" "${EMBY_PATH}/${EMBY_DB}" ".read \"${EMBY_PATH}/${BACKUP_SQL}\""
    "${EMBY_BINARY}" "${EMBY_PATH}/${EMBY_DB}" "PRAGMA integrity_check(1);"
    "${EMBY_BINARY}" "${EMBY_PATH}/${EMBY_DB}" "REINDEX; PRAGMA analysis_limit=0; PRAGMA optimize=0x10002; INSERT INTO fts_search9(fts_search9) VALUES('optimize');"
    rm -f "${EMBY_PATH}/${BACKUP_SQL}"

    # docker start ${EMBY_INSTANCE}
done


set +ex
