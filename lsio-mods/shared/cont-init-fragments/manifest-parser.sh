#!/usr/bin/env bash

manifest_parser_reject() {
  local reason detail
  reason="$1"
  detail="${2:-}"
  if [ -n "$detail" ]; then
    log_warn "event=manifest-schema-invalid reason=${reason} ${detail}" >&2
  else
    log_warn "event=manifest-schema-invalid reason=${reason}" >&2
  fi
}

manifest_parser_field_count() {
  local value without_pipes
  value="$1"
  without_pipes="${value//|/}"
  printf '%s\n' "$((${#value} - ${#without_pipes} + 1))"
}

manifest_parser_valid_sha256() {
  [[ "$1" =~ ^[[:xdigit:]]{64}$ ]]
}

manifest_parser_valid_hex_context() {
  [[ "$1" =~ ^[[:xdigit:]]{32}$ ]]
}

manifest_parser_lower_hex() {
  local value
  value="$1"
  value="${value//A/a}"
  value="${value//B/b}"
  value="${value//C/c}"
  value="${value//D/d}"
  value="${value//E/e}"
  value="${value//F/f}"
  printf '%s\n' "$value"
}

manifest_parser_pool_site_context_matches_write() {
  local original_hex patched_hex delta index original_byte patched_byte diff_count diff_index
  original_hex="$(manifest_parser_lower_hex "$1")"
  patched_hex="$(manifest_parser_lower_hex "$2")"
  delta="$3"
  diff_count=0
  diff_index=-1
  for index in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    original_byte="${original_hex:$((index * 2)):2}"
    patched_byte="${patched_hex:$((index * 2)):2}"
    if [ "$original_byte" != "$patched_byte" ]; then
      diff_count=$((diff_count + 1))
      diff_index="$index"
    fi
  done
  [ "$diff_count" -eq 1 ] && [ "$diff_index" -eq "$delta" ]
}

manifest_parser_valid_arch() {
  case "$1" in
    linux-x86_64-v2|linux-x86_64-v3|linux-arm64) return 0 ;;
    *) return 1 ;;
  esac
}

manifest_parser_valid_mod() {
  case "$1" in
    plex|emby) return 0 ;;
    *) return 1 ;;
  esac
}

manifest_parser_valid_compat_group() {
  local mod compat_group valid_pairs valid_pair
  mod="$1"
  compat_group="$2"
  valid_pairs="plex|icu69 emby|generic"
  if [ "${MANIFEST_PARSER_ENABLE_FIXTURE_COMPAT_GROUPS:-0}" = "1" ]; then
    valid_pairs="${valid_pairs} emby|generic2"
  fi
  for valid_pair in $valid_pairs; do
    [ "$valid_pair" = "${mod}|${compat_group}" ] && return 0
  done
  return 1
}

manifest_parser_valid_detect_role() {
  local mod role
  mod="$1"
  role="$2"
  case "${mod}|${role}" in
    emby\|emby_deps|emby\|emby_dll) return 0 ;;
    plex\|plex_pms:pristine|plex\|plex_pms:patched|plex\|plex_scanner:pristine|plex\|plex_scanner:patched) return 0 ;;
    *) return 1 ;;
  esac
}

manifest_parser_valid_pre_role() {
  local mod role
  mod="$1"
  role="$2"
  case "${mod}|${role}" in
    emby\|target_sqlite) return 0 ;;
    plex\|target_sqlite) return 0 ;;
    plex\|plex_icu_linked:libicuucplex.so.69) return 0 ;;
    plex\|plex_icu_linked:libicui18nplex.so.69) return 0 ;;
    plex\|plex_icu_linked:libicudataplex.so.69) return 0 ;;
    *) return 1 ;;
  esac
}

manifest_parser_valid_emby_target_path() {
  [[ "$1" =~ ^/app/emby/lib/libsqlite3\.so\.[0-9]+[.][0-9]+[.][0-9]+$ ]]
}

manifest_parser_manifest_dir() {
  local manifest dir
  manifest="$1"
  case "$manifest" in
    */*)
      dir="${manifest%/*}"
      [ -n "$dir" ] || dir="/"
      printf '%s\n' "$dir"
      ;;
    *) printf '.\n' ;;
  esac
}

manifest_parser_artifact_full_path() {
  local relpath manifest manifest_dir
  relpath="$1"
  manifest="${2:-${baked_pins:-}}"
  case "$relpath" in
    artifacts/*)
      if [ -n "${sqlite3_mod_artifact_root:-}" ]; then
        printf '%s/%s\n' "${sqlite3_mod_artifact_root%/}" "${relpath#artifacts/}"
      elif [ -n "${sqlite3_mod_root:-}" ]; then
        printf '%s/%s\n' "${sqlite3_mod_root%/}" "$relpath"
      elif [ -n "$manifest" ]; then
        manifest_dir="$(manifest_parser_manifest_dir "$manifest")"
        printf '%s/%s\n' "$manifest_dir" "$relpath"
      else
        printf '%s\n' "$relpath"
      fi
      ;;
    *)
      if [ -n "${sqlite3_mod_root:-}" ]; then
        printf '%s/%s\n' "${sqlite3_mod_root%/}" "$relpath"
      else
        printf '%s\n' "$relpath"
      fi
      ;;
  esac
}

manifest_parser_selected_artifact_row() {
  local mod arch manifest server_id
  mod="$1"
  arch="$2"
  manifest="$3"
  server_id="$4"

  if [ -z "$manifest" ] || [ ! -f "$manifest" ]; then
    return 1
  fi

  local -a fields
  local line kind row_mod row_server row_arch

  while IFS= read -r line || [ -n "$line" ]; do
    line="${line%$'\r'}"
    case "$line" in
      ""|\#*) continue ;;
    esac

    kind="${line%%|*}"
    [ "$kind" = "artifact" ] || continue

    IFS='|' read -r -a fields <<< "$line"
    [ "${#fields[@]}" -eq 9 ] || continue

    row_mod="${fields[2]}"
    row_server="${fields[3]}"
    row_arch="${fields[4]}"
    [ "$row_mod" = "$mod" ] || continue
    [ "$row_server" = "$server_id" ] || continue
    [ "$row_arch" = "$arch" ] || continue

    printf '%s|%s|%s|%s\n' "${fields[5]}" "${fields[6]}" "${fields[7]}" "${fields[8]}"
    return 0
  done < "$manifest"

  return 1
}

manifest_parser_selected_unsupported_row() {
  local mod arch manifest server_id
  mod="$1"
  arch="$2"
  manifest="$3"
  server_id="$4"

  if [ -z "$manifest" ] || [ ! -f "$manifest" ]; then
    return 1
  fi

  local -a fields
  local line kind row_version row_mod row_server row_arch

  while IFS= read -r line || [ -n "$line" ]; do
    line="${line%$'\r'}"
    case "$line" in
      ""|\#*) continue ;;
    esac

    kind="${line%%|*}"
    [ "$kind" = "unsupported" ] || continue

    IFS='|' read -r -a fields <<< "$line"
    [ "${#fields[@]}" -eq 7 ] || continue

    row_version="${fields[1]}"
    row_mod="${fields[2]}"
    row_server="${fields[3]}"
    row_arch="${fields[4]}"
    [ "$row_version" = "1" ] || continue
    [ "$row_mod" = "$mod" ] || continue
    [ "$row_server" = "$server_id" ] || continue
    [ "$row_arch" = "$arch" ] || continue

    printf '%s|%s\n' "${fields[5]}" "${fields[6]}"
    return 0
  done < "$manifest"

  return 1
}

manifest_parser_selected_pre_file_sha256_row() {
  local mod arch manifest server_id role
  mod="$1"
  arch="$2"
  manifest="$3"
  server_id="$4"
  role="$5"

  if [ -z "$manifest" ] || [ ! -f "$manifest" ]; then
    return 1
  fi

  local -a fields
  local line kind row_mod row_server row_arch row_role

  while IFS= read -r line || [ -n "$line" ]; do
    line="${line%$'\r'}"
    case "$line" in
      ""|\#*) continue ;;
    esac

    kind="${line%%|*}"
    [ "$kind" = "pre" ] || continue

    IFS='|' read -r -a fields <<< "$line"
    [ "${#fields[@]}" -eq 10 ] || continue

    row_mod="${fields[2]}"
    row_server="${fields[3]}"
    row_arch="${fields[4]}"
    row_role="${fields[5]}"
    [ "$row_mod" = "$mod" ] || continue
    [ "$row_server" = "$server_id" ] || continue
    [ "$row_arch" = "$arch" ] || continue
    [ "$row_role" = "$role" ] || continue

    printf '%s|%s\n' "${fields[8]}" "${fields[9]}"
    return 0
  done < "$manifest"

  return 1
}

manifest_parser_selected_pool_site_rows() {
  local mod arch manifest server_id binary_path
  mod="$1"
  arch="$2"
  manifest="$3"
  server_id="$4"
  binary_path="$5"

  if [ -z "$manifest" ] || [ ! -f "$manifest" ]; then
    return 1
  fi

  local -a fields
  local line kind row_mod row_server row_arch row_binary_path found
  found=0

  while IFS= read -r line || [ -n "$line" ]; do
    line="${line%$'\r'}"
    case "$line" in
      ""|\#*) continue ;;
    esac

    kind="${line%%|*}"
    [ "$kind" = "pool-site" ] || continue

    IFS='|' read -r -a fields <<< "$line"
    [ "${#fields[@]}" -eq 11 ] || continue

    row_mod="${fields[2]}"
    row_server="${fields[3]}"
    row_arch="${fields[4]}"
    row_binary_path="${fields[5]}"
    [ "$row_mod" = "$mod" ] || continue
    [ "$row_server" = "$server_id" ] || continue
    [ "$row_arch" = "$arch" ] || continue
    [ "$row_binary_path" = "$binary_path" ] || continue

    printf '%s|%s|%s|%s|%s\n' "${fields[6]}" "${fields[7]}" "${fields[8]}" "${fields[9]}" "${fields[10]}"
    found=1
  done < "$manifest"

  [ "$found" -eq 1 ]
}

manifest_parser_selected_pristine_detector_row() {
  local mod arch manifest server_id logical_role role
  mod="$1"
  arch="$2"
  manifest="$3"
  server_id="$4"
  logical_role="$5"

  case "${mod}|${logical_role}" in
    plex\|plex_pms) role="plex_pms:pristine" ;;
    plex\|plex_scanner) role="plex_scanner:pristine" ;;
    *) return 1 ;;
  esac

  if [ -z "$manifest" ] || [ ! -f "$manifest" ]; then
    return 1
  fi

  local -a fields
  local line kind row_mod row_server row_arch row_role

  while IFS= read -r line || [ -n "$line" ]; do
    line="${line%$'\r'}"
    case "$line" in
      ""|\#*) continue ;;
    esac

    kind="${line%%|*}"
    [ "$kind" = "detect" ] || continue

    IFS='|' read -r -a fields <<< "$line"
    [ "${#fields[@]}" -eq 8 ] || continue

    row_mod="${fields[2]}"
    row_server="${fields[3]}"
    row_arch="${fields[4]}"
    row_role="${fields[5]}"
    [ "$row_mod" = "$mod" ] || continue
    [ "$row_server" = "$server_id" ] || continue
    [ "$row_arch" = "$arch" ] || continue
    [ "$row_role" = "$role" ] || continue

    printf '%s|%s\n' "${fields[6]}" "${fields[7]}"
    return 0
  done < "$manifest"

  return 1
}

validate_baked_pins_schema() {
  local manifest
  manifest="${1:-${baked_pins:-}}"
  if [ -z "$manifest" ] || [ ! -f "$manifest" ]; then
    manifest_parser_reject "missing-manifest" "path=${manifest:-unset}"
    return 1
  fi

  local -a fields artifacts pool_sites tuples
  local line line_no kind field_count expected_count row_key
  local meta_count version key_name value_name
  local mod server_id arch role file_path sha compat_group relpath target_path
  local image_ref image_digest binary_path label offset write_seek original_hex patched_hex
  local unsupported_reason tuple artifact_key unsupported_key full_path actual_sha
  local offset_num write_seek_num delta
  local detector_key canonical_key existing_server
  local pms_pristine_path pms_patched_path scanner_pristine_path scanner_patched_path required_soname
  local pool_key artifact_expected_relpath

  local -A seen_row_keys
  local -A tuple_seen
  local -A detect_count
  local -A detect_sha
  local -A detect_path
  local -A artifact_count
  local -A unsupported_count
  local -A tuple_compat_group
  local -A pre_target_count
  local -A pre_icu_count
  local -A pool_site_count
  local -A canonical_sets

  artifacts=()
  pool_sites=()
  tuples=()
  meta_count=0
  line_no=0

  while IFS= read -r line || [ -n "$line" ]; do
    line_no=$((line_no + 1))
    line="${line%$'\r'}"
    case "$line" in
      ""|\#*) continue ;;
    esac

    kind="${line%%|*}"
    case "$kind" in
      current)
        manifest_parser_reject "legacy-current-row" "line=${line_no}"
        return 1
        ;;
      pool-pre)
        manifest_parser_reject "legacy-pool-pre-row" "line=${line_no}"
        return 1
        ;;
      version)
        case "$line" in
          version\|2\|*)
            manifest_parser_reject "legacy-version-v2-row" "line=${line_no}"
            return 1
            ;;
        esac
        ;;
      managed_window)
        manifest_parser_reject "legacy-managed-window-row" "line=${line_no}"
        return 1
        ;;
    esac

    case "$kind" in
      meta) expected_count=6 ;;
      detect) expected_count=8 ;;
      artifact) expected_count=9 ;;
      pre) expected_count=10 ;;
      pool-site) expected_count=11 ;;
      unsupported) expected_count=7 ;;
      *)
        manifest_parser_reject "unknown-row-kind" "line=${line_no} kind=${kind}"
        return 1
        ;;
    esac

    field_count="$(manifest_parser_field_count "$line")"
    if [ "$field_count" -ne "$expected_count" ]; then
      manifest_parser_reject "${kind}-arity" "line=${line_no} expected=${expected_count} actual=${field_count}"
      return 1
    fi

    case "$line" in
      "|"*|*"||"*|*"|")
        manifest_parser_reject "empty-field" "line=${line_no}"
        return 1
        ;;
    esac

    IFS='|' read -r -a fields <<< "$line"
    for i in "${!fields[@]}"; do
      if [ "${fields[$i]}" = "-" ] && { [ "$kind" != "pre" ] || [ "$i" -ne 7 ]; }; then
        manifest_parser_reject "sentinel-outside-pre-image-digest" "line=${line_no}"
        return 1
      fi
    done

    case "$kind" in
      meta)
        version="${fields[1]}"
        key_name="${fields[2]}"
        value_name="${fields[4]}"
        if [ "$version" != "3" ] || [ "$key_name" != "release_tag" ] || [ "$value_name" != "generated_at" ]; then
          manifest_parser_reject "missing-meta-row" "line=${line_no}"
          return 1
        fi
        if [ "$meta_count" -ne 0 ]; then
          manifest_parser_reject "duplicate-meta-row" "line=${line_no}"
          return 1
        fi
        meta_count=1
        ;;

      detect)
        version="${fields[1]}"
        mod="${fields[2]}"
        server_id="${fields[3]}"
        arch="${fields[4]}"
        role="${fields[5]}"
        file_path="${fields[6]}"
        sha="${fields[7]}"
        if [ "$version" != "1" ] || ! manifest_parser_valid_mod "$mod" || ! manifest_parser_valid_arch "$arch" || ! manifest_parser_valid_detect_role "$mod" "$role"; then
          manifest_parser_reject "invalid-field" "line=${line_no}"
          return 1
        fi
        if ! manifest_parser_valid_sha256 "$sha"; then
          manifest_parser_reject "invalid-sha" "line=${line_no}"
          return 1
        fi
        row_key="detect|${mod}|${server_id}|${arch}|${role}"
        if [ -n "${seen_row_keys[$row_key]:-}" ]; then
          manifest_parser_reject "duplicate-row-key" "line=${line_no}"
          return 1
        fi
        seen_row_keys[$row_key]=1
        tuple="${mod}|${server_id}|${arch}"
        if [ -z "${tuple_seen[$tuple]:-}" ]; then
          tuple_seen[$tuple]=1
          tuples+=("$tuple")
        fi
        detect_count[$row_key]=$((${detect_count[$row_key]:-0} + 1))
        detect_sha[$row_key]="$sha"
        detect_path[$row_key]="$file_path"
        ;;

      artifact)
        version="${fields[1]}"
        mod="${fields[2]}"
        server_id="${fields[3]}"
        arch="${fields[4]}"
        compat_group="${fields[5]}"
        relpath="${fields[6]}"
        target_path="${fields[7]}"
        sha="${fields[8]}"
        if [ "$version" != "1" ] || ! manifest_parser_valid_mod "$mod" || ! manifest_parser_valid_arch "$arch" || ! manifest_parser_valid_compat_group "$mod" "$compat_group"; then
          manifest_parser_reject "invalid-field" "line=${line_no}"
          return 1
        fi
        if ! manifest_parser_valid_sha256 "$sha"; then
          manifest_parser_reject "invalid-sha" "line=${line_no}"
          return 1
        fi
        if [ "$mod" = "plex" ] && [ "$target_path" != "/usr/lib/plexmediaserver/lib/libsqlite3.so" ]; then
          manifest_parser_reject "plex-artifact-target-allowlist" "line=${line_no}"
          return 1
        fi
        if [ "$mod" = "emby" ]; then
          if [ "$target_path" = "/app/emby/lib/libsqlite3.so" ]; then
            manifest_parser_reject "emby-alias-symlink" "line=${line_no}"
            return 1
          fi
          if ! manifest_parser_valid_emby_target_path "$target_path"; then
            manifest_parser_reject "emby-target-allowlist" "line=${line_no}"
            return 1
          fi
        fi
        row_key="artifact|${mod}|${server_id}|${arch}|${compat_group}"
        if [ -n "${seen_row_keys[$row_key]:-}" ]; then
          manifest_parser_reject "duplicate-row-key" "line=${line_no}"
          return 1
        fi
        seen_row_keys[$row_key]=1
        tuple="${mod}|${server_id}|${arch}"
        if [ -z "${tuple_seen[$tuple]:-}" ]; then
          tuple_seen[$tuple]=1
          tuples+=("$tuple")
        fi
        if [ -n "${tuple_compat_group[$tuple]:-}" ] && [ "${tuple_compat_group[$tuple]}" != "$compat_group" ]; then
          manifest_parser_reject "duplicate-artifact-coverage-group" "line=${line_no}"
          return 1
        fi
        tuple_compat_group[$tuple]="$compat_group"
        artifact_count[$row_key]=$((${artifact_count[$row_key]:-0} + 1))
        artifacts+=("${mod}|${server_id}|${arch}|${compat_group}|${relpath}|${target_path}|${sha}")
        ;;

      pre)
        version="${fields[1]}"
        mod="${fields[2]}"
        server_id="${fields[3]}"
        arch="${fields[4]}"
        role="${fields[5]}"
        image_ref="${fields[6]}"
        image_digest="${fields[7]}"
        file_path="${fields[8]}"
        sha="${fields[9]}"
        if [ "$version" != "2" ] || ! manifest_parser_valid_mod "$mod" || ! manifest_parser_valid_arch "$arch" || ! manifest_parser_valid_pre_role "$mod" "$role"; then
          manifest_parser_reject "invalid-field" "line=${line_no}"
          return 1
        fi
        if [ -z "$image_ref" ] || [ -z "$image_digest" ]; then
          manifest_parser_reject "empty-field" "line=${line_no}"
          return 1
        fi
        if ! manifest_parser_valid_sha256 "$sha"; then
          manifest_parser_reject "invalid-sha" "line=${line_no}"
          return 1
        fi
        if [ "$role" = "target_sqlite" ]; then
          if [ "$mod" = "plex" ] && [ "$file_path" != "/usr/lib/plexmediaserver/lib/libsqlite3.so" ]; then
            manifest_parser_reject "plex-pre-target-allowlist" "line=${line_no}"
            return 1
          fi
          if [ "$mod" = "emby" ]; then
            if [ "$file_path" = "/app/emby/lib/libsqlite3.so" ]; then
              manifest_parser_reject "emby-alias-symlink" "line=${line_no}"
              return 1
            fi
            if ! manifest_parser_valid_emby_target_path "$file_path"; then
              manifest_parser_reject "emby-target-allowlist" "line=${line_no}"
              return 1
            fi
          fi
          pre_target_count["${mod}|${server_id}|${arch}|${file_path}"]=$((${pre_target_count["${mod}|${server_id}|${arch}|${file_path}"]:-0} + 1))
        else
          required_soname="${role#plex_icu_linked:}"
          if [ "$file_path" != "/usr/lib/plexmediaserver/lib/${required_soname}" ]; then
            manifest_parser_reject "invalid-field" "line=${line_no}"
            return 1
          fi
          pre_icu_count["${server_id}|${arch}|${required_soname}"]=$((${pre_icu_count["${server_id}|${arch}|${required_soname}"]:-0} + 1))
        fi
        row_key="pre|${mod}|${server_id}|${arch}|${role}"
        if [ -n "${seen_row_keys[$row_key]:-}" ]; then
          manifest_parser_reject "duplicate-row-key" "line=${line_no}"
          return 1
        fi
        seen_row_keys[$row_key]=1
        tuple="${mod}|${server_id}|${arch}"
        if [ -z "${tuple_seen[$tuple]:-}" ]; then
          tuple_seen[$tuple]=1
          tuples+=("$tuple")
        fi
        ;;

      pool-site)
        version="${fields[1]}"
        mod="${fields[2]}"
        server_id="${fields[3]}"
        arch="${fields[4]}"
        binary_path="${fields[5]}"
        label="${fields[6]}"
        offset="${fields[7]}"
        write_seek="${fields[8]}"
        original_hex="${fields[9]}"
        patched_hex="${fields[10]}"
        if [ "$version" != "1" ] || [ "$mod" != "plex" ] || ! manifest_parser_valid_arch "$arch"; then
          manifest_parser_reject "invalid-field" "line=${line_no}"
          return 1
        fi
        if ! [[ "$offset" =~ ^[0-9]+$ ]] || ! [[ "$write_seek" =~ ^[0-9]+$ ]]; then
          manifest_parser_reject "pool-site-write-seek-out-of-range" "line=${line_no}"
          return 1
        fi
        if [[ "$offset" =~ ^0[0-9]+$ ]] || [[ "$write_seek" =~ ^0[0-9]+$ ]]; then
          manifest_parser_reject "pool-site-offset-leading-zero" "line=${line_no}"
          return 1
        fi
        offset_num=$((10#$offset))
        write_seek_num=$((10#$write_seek))
        delta=$((write_seek_num - offset_num))
        if [ "$delta" -lt 0 ]; then
          manifest_parser_reject "pool-site-write-seek-negative" "line=${line_no}"
          return 1
        fi
        if [ "$delta" -gt 15 ]; then
          manifest_parser_reject "pool-site-write-seek-out-of-range" "line=${line_no}"
          return 1
        fi
        if ! manifest_parser_valid_hex_context "$original_hex" || ! manifest_parser_valid_hex_context "$patched_hex"; then
          manifest_parser_reject "invalid-hex-context" "line=${line_no}"
          return 1
        fi
        if ! manifest_parser_pool_site_context_matches_write "$original_hex" "$patched_hex" "$delta"; then
          manifest_parser_reject "pool-site-byte-diff" "line=${line_no}"
          return 1
        fi
        row_key="pool-site|${server_id}|${arch}|${binary_path}|${label}|${offset}"
        if [ -n "${seen_row_keys[$row_key]:-}" ]; then
          manifest_parser_reject "duplicate-row-key" "line=${line_no}"
          return 1
        fi
        seen_row_keys[$row_key]=1
        tuple="${mod}|${server_id}|${arch}"
        if [ -z "${tuple_seen[$tuple]:-}" ]; then
          tuple_seen[$tuple]=1
          tuples+=("$tuple")
        fi
        pool_site_count["${server_id}|${arch}|${binary_path}"]=$((${pool_site_count["${server_id}|${arch}|${binary_path}"]:-0} + 1))
        pool_sites+=("${server_id}|${arch}|${binary_path}")
        ;;

      unsupported)
        version="${fields[1]}"
        mod="${fields[2]}"
        server_id="${fields[3]}"
        arch="${fields[4]}"
        compat_group="${fields[5]}"
        unsupported_reason="${fields[6]}"
        if [ "$version" != "1" ] || ! manifest_parser_valid_mod "$mod" || ! manifest_parser_valid_arch "$arch" || ! manifest_parser_valid_compat_group "$mod" "$compat_group"; then
          manifest_parser_reject "invalid-field" "line=${line_no}"
          return 1
        fi
        case "$unsupported_reason" in
          local-offline-*) ;;
          *)
            manifest_parser_reject "unsupported-row-not-offline" "line=${line_no}"
            return 1
            ;;
        esac
        row_key="unsupported|${mod}|${server_id}|${arch}|${compat_group}"
        if [ -n "${seen_row_keys[$row_key]:-}" ]; then
          manifest_parser_reject "duplicate-row-key" "line=${line_no}"
          return 1
        fi
        seen_row_keys[$row_key]=1
        tuple="${mod}|${server_id}|${arch}"
        if [ -z "${tuple_seen[$tuple]:-}" ]; then
          tuple_seen[$tuple]=1
          tuples+=("$tuple")
        fi
        if [ -n "${tuple_compat_group[$tuple]:-}" ] && [ "${tuple_compat_group[$tuple]}" != "$compat_group" ]; then
          manifest_parser_reject "duplicate-artifact-coverage-group" "line=${line_no}"
          return 1
        fi
        tuple_compat_group[$tuple]="$compat_group"
        unsupported_count[$row_key]=$((${unsupported_count[$row_key]:-0} + 1))
        ;;
    esac
  done < "$manifest"

  if [ "$meta_count" -eq 0 ]; then
    manifest_parser_reject "missing-meta-row"
    return 1
  fi

  for artifact_key in "${!unsupported_count[@]}"; do
    if [ -n "${artifact_count[artifact${artifact_key#unsupported}]:-}" ]; then
      manifest_parser_reject "unsupported-artifact-mutual-exclusion" "key=${artifact_key}"
      return 1
    fi
  done

  for tuple in "${tuples[@]}"; do
    IFS='|' read -r mod server_id arch <<< "$tuple"
    case "$mod" in
      emby)
        if [ "${detect_count["detect|${mod}|${server_id}|${arch}|emby_deps"]:-0}" -ne 1 ] || [ "${detect_count["detect|${mod}|${server_id}|${arch}|emby_dll"]:-0}" -ne 1 ]; then
          manifest_parser_reject "emby-detector-count" "server_id=${server_id} arch=${arch}"
          return 1
        fi
        detector_key="emby_deps=${detect_sha["detect|${mod}|${server_id}|${arch}|emby_deps"]}|emby_dll=${detect_sha["detect|${mod}|${server_id}|${arch}|emby_dll"]}"
        ;;
      plex)
        if [ "${detect_count["detect|${mod}|${server_id}|${arch}|plex_pms:pristine"]:-0}" -ne 1 ] || \
          [ "${detect_count["detect|${mod}|${server_id}|${arch}|plex_pms:patched"]:-0}" -ne 1 ] || \
          [ "${detect_count["detect|${mod}|${server_id}|${arch}|plex_scanner:pristine"]:-0}" -ne 1 ] || \
          [ "${detect_count["detect|${mod}|${server_id}|${arch}|plex_scanner:patched"]:-0}" -ne 1 ]; then
          manifest_parser_reject "plex-dual-detector-requirement" "server_id=${server_id} arch=${arch}"
          return 1
        fi
        pms_pristine_path="${detect_path["detect|${mod}|${server_id}|${arch}|plex_pms:pristine"]}"
        pms_patched_path="${detect_path["detect|${mod}|${server_id}|${arch}|plex_pms:patched"]}"
        scanner_pristine_path="${detect_path["detect|${mod}|${server_id}|${arch}|plex_scanner:pristine"]}"
        scanner_patched_path="${detect_path["detect|${mod}|${server_id}|${arch}|plex_scanner:patched"]}"
        if [ "$pms_pristine_path" != "$pms_patched_path" ] || [ "$scanner_pristine_path" != "$scanner_patched_path" ]; then
          manifest_parser_reject "plex-detector-path-conflict" "server_id=${server_id} arch=${arch}"
          return 1
        fi
        detector_key="plex_pms:pristine=${detect_sha["detect|${mod}|${server_id}|${arch}|plex_pms:pristine"]}|plex_pms:patched=${detect_sha["detect|${mod}|${server_id}|${arch}|plex_pms:patched"]}|plex_scanner:pristine=${detect_sha["detect|${mod}|${server_id}|${arch}|plex_scanner:pristine"]}|plex_scanner:patched=${detect_sha["detect|${mod}|${server_id}|${arch}|plex_scanner:patched"]}"
        ;;
      *)
        manifest_parser_reject "invalid-field" "server_id=${server_id} arch=${arch}"
        return 1
        ;;
    esac
    canonical_key="${mod}|${arch}|${detector_key}"
    existing_server="${canonical_sets[$canonical_key]:-}"
    if [ -n "$existing_server" ] && [ "$existing_server" != "$server_id" ]; then
      manifest_parser_reject "duplicate-canonical-detector-set" "server_id=${server_id} arch=${arch}"
      return 1
    fi
    canonical_sets[$canonical_key]="$server_id"
  done

  for tuple in "${tuples[@]}"; do
    IFS='|' read -r mod server_id arch <<< "$tuple"
    compat_group="${tuple_compat_group[$tuple]:-}"
    if [ -z "$compat_group" ]; then
      manifest_parser_reject "missing-artifact-row" "server_id=${server_id} arch=${arch}"
      return 1
    fi
    artifact_key="artifact|${mod}|${server_id}|${arch}|${compat_group}"
    unsupported_key="unsupported|${mod}|${server_id}|${arch}|${compat_group}"
    if [ "${artifact_count[$artifact_key]:-0}" -ne 1 ]; then
      if [ "${unsupported_count[$unsupported_key]:-0}" -ne 1 ]; then
        manifest_parser_reject "missing-artifact-row" "server_id=${server_id} arch=${arch}"
        return 1
      fi
    fi
  done

  for artifact in "${artifacts[@]}"; do
    IFS='|' read -r mod server_id arch compat_group relpath target_path sha <<< "$artifact"
    artifact_expected_relpath="artifacts/${arch}/${compat_group}/libsqlite3.so"
    full_path="$(manifest_parser_artifact_full_path "$relpath" "$manifest")"
    if [ "$relpath" != "$artifact_expected_relpath" ] || [ ! -f "$full_path" ]; then
      manifest_parser_reject "missing-artifact-file" "path=${relpath}"
      return 1
    fi
    actual_sha="$(sha256_of "$full_path")"
    if [ "$actual_sha" != "$sha" ]; then
      manifest_parser_reject "artifact-sha-mismatch" "path=${relpath}"
      return 1
    fi
    if [ "${pre_target_count["${mod}|${server_id}|${arch}|${target_path}"]:-0}" -ne 1 ]; then
      manifest_parser_reject "missing-pre-target-sqlite" "server_id=${server_id} arch=${arch}"
      return 1
    fi
  done

  for tuple in "${tuples[@]}"; do
    IFS='|' read -r mod server_id arch <<< "$tuple"
    if [ "$mod" != "plex" ]; then
      continue
    fi

    for required_soname in libicuucplex.so.69 libicui18nplex.so.69 libicudataplex.so.69; do
      if [ "${pre_icu_count["${server_id}|${arch}|${required_soname}"]:-0}" -ne 1 ]; then
        manifest_parser_reject "missing-plex-icu-pre" "server_id=${server_id} arch=${arch} soname=${required_soname}"
        return 1
      fi
    done

    pms_pristine_path="${detect_path["detect|${mod}|${server_id}|${arch}|plex_pms:pristine"]}"
    scanner_pristine_path="${detect_path["detect|${mod}|${server_id}|${arch}|plex_scanner:pristine"]}"
    if [ "${pool_site_count["${server_id}|${arch}|${pms_pristine_path}"]:-0}" -lt 1 ] || [ "${pool_site_count["${server_id}|${arch}|${scanner_pristine_path}"]:-0}" -lt 1 ]; then
      manifest_parser_reject "missing-plex-pool-site" "server_id=${server_id} arch=${arch}"
      return 1
    fi
  done

  for pool_key in "${pool_sites[@]}"; do
    IFS='|' read -r server_id arch binary_path <<< "$pool_key"
    pms_pristine_path="${detect_path["detect|plex|${server_id}|${arch}|plex_pms:pristine"]:-}"
    scanner_pristine_path="${detect_path["detect|plex|${server_id}|${arch}|plex_scanner:pristine"]:-}"
    if [ "$binary_path" != "$pms_pristine_path" ] && [ "$binary_path" != "$scanner_pristine_path" ]; then
      manifest_parser_reject "pool-site-not-pristine-detector" "server_id=${server_id} arch=${arch}"
      return 1
    fi
  done

  return 0
}
