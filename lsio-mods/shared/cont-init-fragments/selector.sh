#!/usr/bin/env bash

selector_logical_detect_role() {
  local mod role
  mod="$1"
  role="$2"
  case "${mod}|${role}" in
    emby\|emby_deps) printf '%s\n' "emby_deps" ;;
    emby\|emby_dll) printf '%s\n' "emby_dll" ;;
    plex\|plex_pms:pristine|plex\|plex_pms:patched) printf '%s\n' "plex_pms" ;;
    plex\|plex_scanner:pristine|plex\|plex_scanner:patched) printf '%s\n' "plex_scanner" ;;
    *) return 1 ;;
  esac
}

selector_required_roles() {
  case "$1" in
    emby) printf '%s\n' "emby_deps emby_dll" ;;
    plex) printf '%s\n' "plex_pms plex_scanner" ;;
    *) return 1 ;;
  esac
}

selector_warn() {
  local mod arch event detail
  mod="$1"
  arch="$2"
  event="$3"
  detail="${4:-}"
  if [ -n "$detail" ]; then
    log_warn "mod=${mod} phase=${phase_name:-selector} event=${event} arch=${arch} ${detail}" >&2
  else
    log_warn "mod=${mod} phase=${phase_name:-selector} event=${event} arch=${arch}" >&2
  fi
}

selector_join_matches() {
  local joined item
  joined=""
  for item in "$@"; do
    if [ -z "$joined" ]; then
      joined="$item"
    else
      joined="${joined},${item}"
    fi
  done
  printf '%s\n' "$joined"
}

select_supported_server() {
  local mod arch manifest required_roles
  mod="$1"
  arch="$2"
  manifest="$3"

  if [ -z "$manifest" ] || [ ! -f "$manifest" ]; then
    selector_warn "$mod" "$arch" "missing-manifest" "path=${manifest:-unset}"
    return 0
  fi

  if ! required_roles="$(selector_required_roles "$mod")"; then
    selector_warn "$mod" "$arch" "unsupported-mod"
    return 0
  fi

  local -a fields server_ids detector_paths complete_matches
  local -A seen_server role_present role_path role_path_conflict role_allowed
  local -A path_seen path_exists path_sha
  local line kind row_mod server_id row_arch role file_path expected_sha logical_role
  local key path actual_sha logical complete role_count match_count
  local any_present any_missing any_allowed_match any_mismatch

  server_ids=()
  detector_paths=()
  complete_matches=()
  any_present=0
  any_missing=0
  any_allowed_match=0
  any_mismatch=0

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
    server_id="${fields[3]}"
    row_arch="${fields[4]}"
    role="${fields[5]}"
    file_path="${fields[6]}"
    expected_sha="${fields[7]}"

    [ "$row_mod" = "$mod" ] || continue
    [ "$row_arch" = "$arch" ] || continue
    logical_role="$(selector_logical_detect_role "$mod" "$role")" || continue

    if [ -z "${seen_server[$server_id]:-}" ]; then
      seen_server[$server_id]=1
      server_ids+=("$server_id")
    fi

    key="${server_id}|${logical_role}"
    if [ -z "${role_present[$key]:-}" ]; then
      role_present[$key]=1
      role_path[$key]="$file_path"
    elif [ "${role_path[$key]}" != "$file_path" ]; then
      role_path_conflict[$key]=1
    fi
    role_allowed["${key}|${expected_sha}"]=1

    if [ -z "${path_seen[$file_path]:-}" ]; then
      path_seen[$file_path]=1
      detector_paths+=("$file_path")
    fi
  done < "$manifest"

  for path in "${detector_paths[@]}"; do
    if [ -f "$path" ]; then
      if actual_sha="$(sha256_of "$path" 2>/dev/null)"; then
        path_exists[$path]=1
        path_sha[$path]="$actual_sha"
        any_present=1
      else
        path_exists[$path]=0
        any_missing=1
      fi
    else
      path_exists[$path]=0
      any_missing=1
    fi
  done

  for server_id in "${server_ids[@]}"; do
    complete=1
    role_count=0
    match_count=0

    for logical in $required_roles; do
      role_count=$((role_count + 1))
      key="${server_id}|${logical}"
      if [ -z "${role_present[$key]:-}" ] || [ -n "${role_path_conflict[$key]:-}" ]; then
        complete=0
        continue
      fi

      path="${role_path[$key]}"
      if [ "${path_exists[$path]:-0}" != "1" ]; then
        complete=0
        continue
      fi

      actual_sha="${path_sha[$path]}"
      if [ -n "${role_allowed["${key}|${actual_sha}"]:-}" ]; then
        match_count=$((match_count + 1))
        any_allowed_match=1
      else
        complete=0
        any_mismatch=1
      fi
    done

    if [ "$complete" -eq 1 ] && [ "$match_count" -eq "$role_count" ]; then
      complete_matches+=("$server_id")
    fi
  done

  case "${#complete_matches[@]}" in
    1)
      printf '%s\n' "${complete_matches[0]}"
      return 0
      ;;
    0)
      if [ "$any_present" -eq 0 ]; then
        selector_warn "$mod" "$arch" "zero-detectors"
      elif [ "$any_allowed_match" -eq 1 ]; then
        selector_warn "$mod" "$arch" "partial-detector-match"
      elif [ "$any_mismatch" -eq 1 ]; then
        selector_warn "$mod" "$arch" "detector-mismatch"
      elif [ "$any_missing" -eq 1 ]; then
        selector_warn "$mod" "$arch" "zero-detectors"
      else
        selector_warn "$mod" "$arch" "zero-detectors"
      fi
      return 0
      ;;
    *)
      selector_warn "$mod" "$arch" "ambiguous-detector-match" "matches=$(selector_join_matches "${complete_matches[@]}")"
      return 0
      ;;
  esac
}

selected_artifact_row() {
  manifest_parser_selected_artifact_row "$@"
}

selected_unsupported_row() {
  manifest_parser_selected_unsupported_row "$@"
}

selected_pre_sha256() {
  local pre_row

  if ! pre_row="$(manifest_parser_selected_pre_file_sha256_row "$@")"; then
    return 1
  fi

  case "$pre_row" in
    *\|*)
      printf '%s\n' "${pre_row##*|}"
      ;;
    *)
      return 1
      ;;
  esac
}

selected_pre_file_sha256_row() {
  manifest_parser_selected_pre_file_sha256_row "$@"
}

selected_pool_site_rows() {
  manifest_parser_selected_pool_site_rows "$@"
}

selected_pristine_detector_row() {
  manifest_parser_selected_pristine_detector_row "$@"
}

selected_artifact_is_current() {
  local mod arch manifest server_id
  mod="$1"
  arch="$2"
  manifest="$3"
  server_id="$4"

  if [ -z "$manifest" ] || [ ! -f "$manifest" ]; then
    selector_warn "$mod" "$arch" "missing-manifest" "path=${manifest:-unset}"
    return 1
  fi

  local artifact_row compat_group artifact_relpath target_path expected_sha actual_sha
  if ! artifact_row="$(selected_artifact_row "$mod" "$arch" "$manifest" "$server_id")"; then
    selector_warn "$mod" "$arch" "missing-artifact-row" "server_id=${server_id}"
    return 1
  fi
  IFS='|' read -r compat_group artifact_relpath target_path expected_sha <<< "$artifact_row"
  : "$compat_group" "$artifact_relpath"

  if [ -z "$target_path" ] || [ -z "$expected_sha" ]; then
    selector_warn "$mod" "$arch" "missing-artifact-row" "server_id=${server_id}"
    return 1
  fi

  if [ ! -f "$target_path" ]; then
    selector_warn "$mod" "$arch" "missing-target" "server_id=${server_id} target_path=${target_path}"
    return 1
  fi

  if ! actual_sha="$(sha256_of "$target_path" 2>/dev/null)"; then
    selector_warn "$mod" "$arch" "target-sha-failed" "server_id=${server_id} target_path=${target_path}"
    return 1
  fi

  if [ "$actual_sha" = "$expected_sha" ]; then
    return 0
  fi

  selector_warn "$mod" "$arch" "current-artifact-mismatch" "server_id=${server_id} target_path=${target_path} expected=${expected_sha} actual=${actual_sha}"
  return 1
}
