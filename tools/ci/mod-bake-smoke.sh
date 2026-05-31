#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/ci/lib/assertions.sh
. "${script_dir}/lib/assertions.sh"

MATRIX_MOD=$1
MATRIX_IMAGE_NAME=$2
MATRIX_ARCH_SUFFIX=$3
GITHUB_RUN_ID=$4
OWNER_NAME=$5
GITHUB_REF=$6
GITHUB_REF_NAME=$7
SQLITE_VERSION=$8
SQLITE_VERSION_DOTTED=$9
PLEX_IMAGE_TAG=${10}
EMBY_IMAGE_TAG=${11}
artifact_root=${12}

case "$MATRIX_MOD" in
  plex)
    lsio_image="lscr.io/linuxserver/plex:${PLEX_IMAGE_TAG}"
    target_path="/usr/lib/plexmediaserver/lib/libsqlite3.so"
    artifact_kind="library-plex"
    ;;
  emby)
    lsio_image="lscr.io/linuxserver/emby:${EMBY_IMAGE_TAG}"
    target_path="/app/emby/lib/libsqlite3.so.3.49.2"
    artifact_kind="library"
    ;;
  *)
    echo "FATAL: unsupported mod: $MATRIX_MOD" >&2
    exit 1
    ;;
esac
case "$MATRIX_ARCH_SUFFIX" in
  amd64) runtime_arches=(linux-x86_64-v2 linux-x86_64-v3); expected_arch_re='arch=linux-x86_64-v(2|3)' ;;
  arm64) runtime_arches=(linux-arm64); expected_arch_re='arch=linux-arm64' ;;
  *)
    echo "FATAL: unsupported arch suffix: $MATRIX_ARCH_SUFFIX" >&2
    exit 1
    ;;
esac
if [[ "$GITHUB_REF" == refs/tags/* ]]; then
  mod_release_tag="$GITHUB_REF_NAME"
else
  mod_release_tag="run-${GITHUB_RUN_ID}"
fi

tmpdir="$(mktemp -d)"
staged="${tmpdir}/staged"
pre_fragment="${tmpdir}/pre-fragment.txt"
sha256sums="${tmpdir}/SHA256SUMS"
baked_pins="${tmpdir}/baked-pins.txt"
: > "$pre_fragment"
: > "$sha256sums"

image_digest="$(docker image inspect --format '{{if .RepoDigests}}{{index .RepoDigests 0}}{{end}}' "$lsio_image")"
if [ -z "$image_digest" ]; then
  echo "FATAL: docker image inspect returned no repo digest for $lsio_image" >&2
  exit 1
fi
emit_pre_row() {
  local target="$1" arch="$2" image="$3" digest="$4" path="$5" pre_sha
  pre_sha="$(docker run --rm --entrypoint sha256sum "$image" "$path" | awk '{print $1}')"
  [[ "$pre_sha" =~ ^[0-9A-Fa-f]{64}$ ]] || {
    echo "FATAL: invalid runtime SHA for $path from $image: $pre_sha" >&2
    exit 1
  }
  printf 'pre|1|%s|%s|%s|%s|%s|runtime|%s\n' \
    "$target" "$arch" "$image" "$digest" "$path" "$pre_sha" >> "$pre_fragment"
}
for arch in "${runtime_arches[@]}"; do
  emit_pre_row "$MATRIX_MOD" "$arch" "$lsio_image" "$image_digest" "$target_path"
  if [ "$MATRIX_MOD" = "plex" ]; then
    emit_pre_row plex "$arch" "$lsio_image" "$image_digest" /usr/lib/plexmediaserver/lib/libicuucplex.so.69
    emit_pre_row plex "$arch" "$lsio_image" "$image_digest" /usr/lib/plexmediaserver/lib/libicui18nplex.so.69
    emit_pre_row plex "$arch" "$lsio_image" "$image_digest" /usr/lib/plexmediaserver/lib/libicudataplex.so.69
  fi
done

render_args=()
stage_args=()
for arch in "${runtime_arches[@]}"; do
  artifact_dir="${artifact_root}/sqlite-${SQLITE_VERSION}-${artifact_kind}-${arch}"
  artifact_path="${artifact_dir}/libsqlite3.so"
  artifact_name="sqlite-${mod_release_tag}-${artifact_kind}-${arch}.so"
  if [ ! -f "$artifact_path" ]; then
    echo "FATAL: missing sqlite artifact: $artifact_path" >&2
    exit 1
  fi
  artifact_sha="$(sha256sum "$artifact_path" | awk '{print $1}')"
  printf '%s  %s\n' "$artifact_sha" "$artifact_name" >> "$sha256sums"
  render_args+=(--artifact "${arch}:${artifact_name}:${artifact_path}:${target_path}")
  stage_args+=(--artifact "${arch}:${artifact_path}")
done

if [ "$MATRIX_MOD" = "plex" ]; then
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
    --mod plex \
    --release-tag "$mod_release_tag" \
    --generated-at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    --sha256sums "$sha256sums" \
    --pre-fragment "$pre_fragment" \
    --pool-baselines pins/plex-pool-patch-baselines.txt \
    --output "$baked_pins" \
    "${render_args[@]}"
  bash tools/lsio-mod/stage-lsio-mod.sh \
    --mod plex \
    --output-dir "$staged" \
    --baked-pins "$baked_pins" \
    "${stage_args[@]}"
else
  bash tools/lsio-mod/render-lsio-mod-baked-pins.sh \
    --mod emby \
    --release-tag "$mod_release_tag" \
    --generated-at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    --sha256sums "$sha256sums" \
    --pre-fragment "$pre_fragment" \
    --output "$baked_pins" \
    "${render_args[@]}"
  bash tools/lsio-mod/stage-lsio-mod.sh \
    --mod emby \
    --output-dir "$staged" \
    --baked-pins "$baked_pins" \
    "${stage_args[@]}"
fi

if [ ! -d "$staged/root-fs/etc/s6-overlay/s6-rc.d" ]; then
  echo "FATAL: staged mod is missing native s6-rc init-mod tree" >&2
  exit 1
fi
legacy_init_dir="${staged}/root-fs/etc/cont"'-init.d'
if [ -d "$legacy_init_dir" ]; then
  echo "FATAL: staged mod still contains legacy init scripts" >&2
  exit 1
fi

smoke_ref="ghcr.io/${OWNER_NAME}/${MATRIX_IMAGE_NAME}:${GITHUB_RUN_ID}-${MATRIX_MOD}-${MATRIX_ARCH_SUFFIX}-smoke"
mod_ref="ghcr.io/${OWNER_NAME}/${MATRIX_IMAGE_NAME}:${GITHUB_RUN_ID}-${MATRIX_MOD}-${MATRIX_ARCH_SUFFIX}"
# Why: mod-build has no registry credentials, so smoke bakes root-fs into a derived image instead of coupling validation to post-publish install flow.
printf 'FROM %s\nCOPY root-fs /\n' "$lsio_image" > "${tmpdir}/Dockerfile.smoke"
docker build --rm -t "$smoke_ref" -f "${tmpdir}/Dockerfile.smoke" "$staged"
docker build --rm -t "$mod_ref" "$staged"

smoke_container="sqlite-mod-smoke-${GITHUB_RUN_ID}-${MATRIX_MOD}-${MATRIX_ARCH_SUFFIX}"
run1_logs="${tmpdir}/smoke-run1.log"
run2_logs="${tmpdir}/smoke-run2.log"
run3_logs="${tmpdir}/smoke-run3.log"
bad_signal_re='\bSQLITE_(ERROR|INTERNAL|NOMEM|IOERR|CORRUPT|FULL|CANTOPEN|PROTOCOL|MISMATCH|MISUSE|NOTADB|FORMAT|AUTH)\b|database disk image is malformed|Cannot create table|no such function|no such collation|Failed to open'

write_log_slice() {
  local cursor="$1" output="$2"
  # Use a line-count cursor instead of --since so same-second restarts cannot bleed terminal markers across slices.
  docker logs "$smoke_container" 2>&1 | tail -n +$((cursor + 1)) > "$output" || true
}

debug_cat() {
  local file="$1"
  if [ -n "${RUNNER_DEBUG:-}" ]; then
    cat "$file"
  fi
}

capture_log_cursor() {
  docker logs "$smoke_container" 2>&1 | wc -l | tr -d ' '
}

wait_for_slice_marker() {
  local cursor="$1" slice="$2" marker="$3" run_label="$4"
  local ready=0
  for _ in $(seq 1 30); do
    write_log_slice "$cursor" "$slice"
    if grep -Fq "$marker" "$slice"; then
      ready=1
      break
    fi
    sleep 3
  done
  if [ "$ready" -ne 1 ]; then
    cat "$slice"
    echo "FATAL: LSIO mod bake-in smoke did not reach ${run_label} terminal marker: ${marker}" >&2
    exit 1
  fi
}

wait_for_plex_slice() {
  local cursor="$1" slice="$2" run_label="$3" pool_marker="$4"
  local ready=0
  for _ in $(seq 1 30); do
    write_log_slice "$cursor" "$slice"
    if grep -Fq "$pool_marker" "$slice"; then
      if [ "$MATRIX_ARCH_SUFFIX" = "arm64" ] || grep -Fq 'mod=plex phase=04-pool-patch event=complete' "$slice"; then
        ready=1
        break
      fi
    fi
    sleep 3
  done
  if [ "$ready" -ne 1 ]; then
    cat "$slice"
    echo "FATAL: LSIO mod bake-in smoke did not reach ${run_label} terminal marker: ${pool_marker}" >&2
    exit 1
  fi
}

wait_for_emby_system_xml() {
  local ready=0
  for _ in $(seq 1 40); do
    if docker exec --user 0:0 "$smoke_container" test -f /config/config/system.xml; then
      ready=1
      break
    fi
    sleep 3
  done
  if [ "$ready" -ne 1 ]; then
    echo "FATAL: Emby did not generate /config/config/system.xml within 120s" >&2
    docker exec --user 0:0 "$smoke_container" ls -la /config || true
    docker exec --user 0:0 "$smoke_container" ls -la /config/config || true
    docker exec --user 0:0 "$smoke_container" cat /config/config/system.xml 2>/dev/null || true
    exit 1
  fi
}

wait_for_tcp_port() {
  local port="$1" run_label="$2"
  local ready=0
  for _ in $(seq 1 60); do
    if docker exec --user 0:0 "$smoke_container" bash -c "echo > /dev/tcp/127.0.0.1/${port}" 2>/dev/null; then
      ready=1
      break
    fi
    sleep 3
  done
  if [ "$ready" -ne 1 ]; then
    echo "FATAL: ${run_label} app did not accept TCP on port ${port} within 180s" >&2
    exit 1
  fi
}

trap 'docker logs "$smoke_container" 2>&1 || true; docker rm -f "$smoke_container" >/dev/null 2>&1 || true' EXIT

log_cursor=0
docker run --detach \
  --name "$smoke_container" \
  -e PUID=1000 \
  -e PGID=1000 \
  "$smoke_ref"
if [ "$MATRIX_MOD" = "plex" ]; then
  if [ "$MATRIX_ARCH_SUFFIX" = "amd64" ]; then
    wait_for_plex_slice "$log_cursor" "$run1_logs" "run 1" 'pool_patch event=patched'
  else
    wait_for_plex_slice "$log_cursor" "$run1_logs" "run 1" 'event=pool-patch-deferred'
  fi
  wait_for_slice_marker "$log_cursor" "$run1_logs" '[ls.io-init] done.' "run 1"
  wait_for_tcp_port 32400 "run 1"
  write_log_slice "$log_cursor" "$run1_logs"
  debug_cat "$run1_logs"
  assert_plex_run "$run1_logs" "run 1" installed "$MATRIX_ARCH_SUFFIX" "$expected_arch_re" "$bad_signal_re"
  assert_runtime_load "$run1_logs" "run 1" "$MATRIX_MOD" "$SQLITE_VERSION_DOTTED"
else
  wait_for_slice_marker "$log_cursor" "$run1_logs" 'mod=emby phase=04-config event=missing-config' "run 1"
  wait_for_slice_marker "$log_cursor" "$run1_logs" '[ls.io-init] done.' "run 1"
  wait_for_tcp_port 8096 "run 1"
  docker exec --user 0:0 "$smoke_container" test -f /config/config/system.xml || wait_for_emby_system_xml
  write_log_slice "$log_cursor" "$run1_logs"
  debug_cat "$run1_logs"
  assert_common_run "$run1_logs" "run 1" "$expected_arch_re" "$bad_signal_re"
  assert_swap_run "$run1_logs" "run 1" installed
  require_grep 'mod=emby phase=04-config event=missing-config' "$run1_logs" "FATAL: run 1 missing Emby initial missing-config marker"
  assert_runtime_load "$run1_logs" "run 1" "$MATRIX_MOD" "$SQLITE_VERSION_DOTTED"
fi

log_cursor="$(capture_log_cursor)"
docker stop -t 60 "$smoke_container" >/dev/null
docker start "$smoke_container" >/dev/null
if [ "$MATRIX_MOD" = "plex" ]; then
  if [ "$MATRIX_ARCH_SUFFIX" = "amd64" ]; then
    wait_for_plex_slice "$log_cursor" "$run2_logs" "run 2" 'pool_patch event=already-patched'
  else
    wait_for_plex_slice "$log_cursor" "$run2_logs" "run 2" 'event=pool-patch-deferred'
  fi
  wait_for_slice_marker "$log_cursor" "$run2_logs" '[ls.io-init] done.' "run 2"
  wait_for_tcp_port 32400 "run 2"
  write_log_slice "$log_cursor" "$run2_logs"
  debug_cat "$run2_logs"
  assert_plex_run "$run2_logs" "run 2" skip "$MATRIX_ARCH_SUFFIX" "$expected_arch_re" "$bad_signal_re"
  assert_runtime_load "$run2_logs" "run 2" "$MATRIX_MOD" "$SQLITE_VERSION_DOTTED"
else
  wait_for_slice_marker "$log_cursor" "$run2_logs" 'mod=emby phase=04-config event=complete' "run 2"
  wait_for_tcp_port 8096 "run 2"
  write_log_slice "$log_cursor" "$run2_logs"
  debug_cat "$run2_logs"
  assert_common_run "$run2_logs" "run 2" "$expected_arch_re" "$bad_signal_re"
  assert_swap_run "$run2_logs" "run 2" skip
  for tag in MaxLibraryDbConnections MaxAuthDbConnections MaxOtherDbConnections EnableSqLiteMmio; do
    assert_emby_tag_transition "$run2_logs" "$tag" "run 2"
  done
  assert_runtime_load "$run2_logs" "run 2" "$MATRIX_MOD" "$SQLITE_VERSION_DOTTED"
fi

log_cursor="$(capture_log_cursor)"
docker stop -t 60 "$smoke_container" >/dev/null
docker start "$smoke_container" >/dev/null
if [ "$MATRIX_MOD" = "plex" ]; then
  if [ "$MATRIX_ARCH_SUFFIX" = "amd64" ]; then
    wait_for_plex_slice "$log_cursor" "$run3_logs" "run 3" 'pool_patch event=already-patched'
  else
    wait_for_plex_slice "$log_cursor" "$run3_logs" "run 3" 'event=pool-patch-deferred'
  fi
  wait_for_slice_marker "$log_cursor" "$run3_logs" '[ls.io-init] done.' "run 3"
  wait_for_tcp_port 32400 "run 3"
  write_log_slice "$log_cursor" "$run3_logs"
  debug_cat "$run3_logs"
  assert_plex_run "$run3_logs" "run 3" skip "$MATRIX_ARCH_SUFFIX" "$expected_arch_re" "$bad_signal_re"
  assert_runtime_load "$run3_logs" "run 3" "$MATRIX_MOD" "$SQLITE_VERSION_DOTTED"
else
  wait_for_slice_marker "$log_cursor" "$run3_logs" 'mod=emby phase=04-config event=complete' "run 3"
  wait_for_tcp_port 8096 "run 3"
  write_log_slice "$log_cursor" "$run3_logs"
  debug_cat "$run3_logs"
  assert_common_run "$run3_logs" "run 3" "$expected_arch_re" "$bad_signal_re"
  assert_swap_run "$run3_logs" "run 3" skip
  assert_emby_run3_idempotent "$run3_logs"
  assert_runtime_load "$run3_logs" "run 3" "$MATRIX_MOD" "$SQLITE_VERSION_DOTTED"
fi

docker rm -f "$smoke_container" >/dev/null 2>&1 || true
trap - EXIT

mkdir -p "mod-image-${MATRIX_MOD}-${MATRIX_ARCH_SUFFIX}"
docker save "$mod_ref" -o "mod-image-${MATRIX_MOD}-${MATRIX_ARCH_SUFFIX}/image.tar"
printf '%s\n' "$mod_ref" > "mod-image-${MATRIX_MOD}-${MATRIX_ARCH_SUFFIX}/image-ref.txt"
