#!/usr/bin/env bash

set -ex


BACKUP_SQL="dump.sql"
PAGE_SIZE="16384"
BACKUP_PATH="/mnt/media-backup"

# declare -a PLEX_INSTANCES=("plex-metadata" "plex-int-metadata" "plex-misc-metadata" "plex-1" "plex-int-1" "plex-misc-1" "plex-audiobooks-1" "plex-2" "plex-int-2" "plex-misc-2" "plex-audiobooks-2" "plex-adult" "plex-backup-1" "plex-backup-2" "plex-backup-3" "plex-backup-metadata")
declare -a PLEX_INSTANCES=()
# declare -a EMBY_INSTANCES=("emby-metadata" "emby-int-metadata" "emby-misc-metadata" "emby-1" "emby-misc-1" "emby-2" "emby-misc-2" "emby-trial" "emby-misc-trial" "emby-backup-1" "emby-backup-2" "emby-backup-3" "emby-backup-metadata")
declare -a EMBY_INSTANCES=()

PLEX_BINARY="/home/darthshadow/plex-sql/Plex SQLite"
PLEX_DB="com.plexapp.plugins.library.db"
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
    "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "SELECT * from versioned_metadata_items;"
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
    rm -f "${PLEX_DATABASES_PATH}/${PLEX_DB}"
    "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "PRAGMA page_size; PRAGMA page_size=${PAGE_SIZE}; VACUUM; PRAGMA page_size;"
    sed -i -e 's/ROLLBACK;/COMMIT;/' "${PLEX_DATABASES_PATH}/${BACKUP_SQL}"
    "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" ".read \"${PLEX_DATABASES_PATH}/${BACKUP_SQL}\""
    "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "PRAGMA integrity_check(1);"
    "${PLEX_BINARY}" "${PLEX_DATABASES_PATH}/${PLEX_DB}" "PRAGMA analysis_limit=0; ANALYZE; PRAGMA optimize; REINDEX;"
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
    EMBY_PATH="/opt/${EMBY_INSTANCE}/data"
    if [ ! -d "${EMBY_PATH}" ]; then
        echo "Skipped Missing Emby Instance: ${EMBY_INSTANCE}"
        continue
    fi

    # docker stop ${EMBY_INSTANCE}

    echo "Optimizing Emby Database: ${EMBY_PATH}/${EMBY_DB}"
    "${EMBY_BINARY}" "${EMBY_PATH}/${EMBY_DB}" "SELECT * from SyncJobs2;"
    "${EMBY_BINARY}" "${EMBY_PATH}/${EMBY_DB}" "PRAGMA integrity_check(1);"
    if [ -d "${BACKUP_PATH}" ]; then
      mkdir -p "${BACKUP_PATH}/${EMBY_INSTANCE}/Databases"
      cp "${EMBY_PATH}/${EMBY_DB}" "${BACKUP_PATH}/${EMBY_INSTANCE}/Databases/${EMBY_DB}-$(date '+%Y-%m-%d').original"
    else
      cp "${EMBY_PATH}/${EMBY_DB}" "${EMBY_PATH}/${EMBY_DB}-$(date '+%Y-%m-%d').original"
    fi
    "${EMBY_BINARY}" "${EMBY_PATH}/${EMBY_DB}" ".output \"${EMBY_PATH}/${BACKUP_SQL}\"" .dump
    rm -f "${EMBY_PATH}/${EMBY_DB}"
    "${EMBY_BINARY}" "${EMBY_PATH}/${EMBY_DB}" "PRAGMA page_size; PRAGMA page_size=${PAGE_SIZE}; VACUUM; PRAGMA page_size;"
    "${EMBY_BINARY}" "${EMBY_PATH}/${EMBY_DB}" ".read \"${EMBY_PATH}/${BACKUP_SQL}\""
    "${EMBY_BINARY}" "${EMBY_PATH}/${EMBY_DB}" "PRAGMA integrity_check(1);"
    "${EMBY_BINARY}" "${EMBY_PATH}/${EMBY_DB}" "PRAGMA analysis_limit=0; ANALYZE; PRAGMA optimize; REINDEX;"
    rm -f "${EMBY_PATH}/${BACKUP_SQL}"

    # docker start ${EMBY_INSTANCE}
done


set +ex
