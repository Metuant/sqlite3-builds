#!/bin/bash -e

# Variables for file paths
sqlite_library="/mnt/scripts/sqlite-library/libsqlite3.so"
sqlite_library_expected_checksum="924644849"
emby_sqlite_library="/app/emby/lib/libsqlite3.so.3.49.2"
emby_sqlite_library_expected_checksum="1932395985"

# Check if files exist
if [ ! -f "$sqlite_library" ]; then
  echo "${sqlite_library} does not exist."
  exit 1
fi

if [ ! -f "$emby_sqlite_library" ]; then
  echo "${emby_sqlite_library} does not exist."
  exit 1
fi

# Calculate checksums
sqlite_library_checksum=$(cksum $sqlite_library | awk '{ print $1 }')
emby_sqlite_library_checksum=$(cksum $emby_sqlite_library | awk '{ print $1 }')

# Compare checksums and replace emby_sqlite_library with sqlite_library if they're different
if [ "$emby_sqlite_library_checksum" = "$emby_sqlite_library_expected_checksum" ] && [ "$sqlite_library_checksum" = "$sqlite_library_expected_checksum" ] && [ "$emby_sqlite_library_checksum" != "$sqlite_library_checksum" ]
then
  echo "Checksums are different, replacing ${emby_sqlite_library} with ${sqlite_library}."

  # Backup emby_sqlite_library
  cp -v -f $emby_sqlite_library ${emby_sqlite_library}.backup
  if [ $? -ne 0 ]; then
    echo "Backup failed, aborting."
    exit 1
  fi

  # Replace emby_sqlite_library with sqlite_library
  cp -v -f $sqlite_library $emby_sqlite_library
  if [ $? -ne 0 ]; then
    echo "File replacement failed, rolling back."
    cp -v -f ${emby_sqlite_library}.backup $emby_sqlite_library
    if [ $? -ne 0 ]; then
      echo "Rollback failed, please manually check files."
      exit 1

    else
      echo "Rollback successful."
    fi

  else
    echo "File replacement successful, replaced ${emby_sqlite_library} with ${sqlite_library}."
  fi

else
  echo "Checksums are the same, no action taken."
fi
