# This file is sourced-only, not executed directly.
# It defines assert_pre_replacement_sha.

assert_pre_replacement_sha() {
  # >>> ASSERT_PRE_REPLACEMENT_SHA BEGIN <<<
  local target=$1 current_sha match expected_sha_list

  if [[ "${target}" == "jellyfin" ]]; then
    echo "WARNING: skipping SQLite library pin assertion for dormant jellyfin target" >&2
    return 0
  fi

  current_sha="$(sha256_of "${target_path}")"
  match="$(
    printf "%s\n" "${SQLITE_LIBRARY_PINS}" | tr -d '\r' | awk -F'|' \
      -v target="${target}" \
      -v arch="${arch}" \
      -v tag="${TAG}" \
      -v target_path="${target_path}" \
      -v current_sha="${current_sha}" '
        /^#/ || NF == 0 { next }
        $1 == "version" { next }
        NF != 9 { next }
        $2 != "1" { next }
        $3 != target || $4 != arch || $7 != target_path || $9 != current_sha { next }
        $1 == "pre" {
          print "pre|" $5 "|" $6
          found = 1
          exit
        }
        $1 == "post" && $5 == "release" && $6 == tag {
          print "current-post|" $6 "|" $8
          found = 1
          exit
        }
        $1 == "post" && $5 == "release" {
          print "window-post|" $5 "|" $6
          found = 1
          exit
        }
        END { if (!found) exit 1 }
      '
  )" || {
    expected_sha_list="$(
      printf "%s\n" "${SQLITE_LIBRARY_PINS}" | tr -d '\r' | awk -F'|' \
        -v target="${target}" \
        -v arch="${arch}" \
        -v target_path="${target_path}" '
          /^#/ || NF == 0 { next }
          NF != 9 { next }
          $2 != "1" { next }
          $3 != target || $4 != arch || $7 != target_path { next }
          $1 == "pre" { print "pre source=" $5 " digest=" $6 " sha=" $9; next }
          $1 == "post" && $5 == "release" { print "post release=" $6 " artifact=" $8 " sha=" $9; next }
        '
    )"
    if [[ -z "${expected_sha_list}" ]]; then
      expected_sha_list="(none)"
    fi
    fatal "unknown pre-replacement SHA for ${target} ${arch} at ${target_path}: ${current_sha}; schema=${SQLITE_LIBRARY_PINS_SCHEMA_VERSION}; release=${SQLITE_LIBRARY_PINS_RELEASE_TAG}; window=${SQLITE_LIBRARY_PINS_WINDOW_N}; expected pre/post SHAs: ${expected_sha_list}; use a matching release script or restore a managed LSIO/runtime library before deploying"
  }

  case "${match}" in
    pre\|*)
      echo "Verified managed pre-replacement ${target} ${arch} SHA ${current_sha}."
      ;;
    window-post\|*)
      echo "Verified managed prior-release ${target} ${arch} SHA ${current_sha}."
      ;;
    current-post\|*)
      echo "${target_path} already deployed at ${current_sha}."
      exit 0
      ;;
    *)
      fatal "internal pin assertion error for ${target}: ${match}"
      ;;
  esac
  OUT_ASSERTED_SHA="${current_sha}"
  # >>> ASSERT_PRE_REPLACEMENT_SHA END <<<
}
