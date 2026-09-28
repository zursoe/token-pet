#!/bin/bash
# sync_remote_agents.sh — pull filtered agent-data mirrors from remote nodes.
#
# Filter keeps only token-accounting lines and strips prompt/system text:
#   codex    : token_count / session_meta / turn_context  (base_instructions stripped)
#   claude   : lines containing "usage" or "summary"
#   kimi     : usage.record lines
#   opencode : remote sqlite3 trims the db to sessions + assistant messages only
#
# Nodes are read from a config file (never hard-coded here):
#   default path : ~/.config/tokenpet/sync_nodes.conf
#   override     : TOKENPET_SYNC_NODES=/path/to/conf
#   example      : tools/sync_nodes.example.conf
#
# Usage:
#   ./sync_remote_agents.sh          run the sync (idempotent; re-running only adds deltas)
#   ./sync_remote_agents.sh --check  print the planned pulls without executing anything
set -u
MIRROR="${HOME}/agent-mirror"
mkdir -p "$MIRROR"

CONF="${TOKENPET_SYNC_NODES:-$HOME/.config/tokenpet/sync_nodes.conf}"
CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

if [ ! -f "$CONF" ]; then
  echo "node config not found: $CONF"
  echo "copy tools/sync_nodes.example.conf to that path and edit it"
  exit 1
fi
# shellcheck source=/dev/null
. "$CONF"

run() {
  if [ "$CHECK_ONLY" = "1" ]; then
    echo "WOULD: $*"
  else
    "$@"
  fi
}

# ---------- generic codex pull: $1=user@host $2=node_label $3=remote_codex_dir ----------
pull_codex() {
  local host="$1" node="$2" remote_dir="$3"
  local dest="$MIRROR/$node/.codex"
  mkdir -p "$dest"
  echo "[$node] codex: filtering + streaming ..."
  ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" bash -s -- "$remote_dir" 2>/dev/null <<'REMOTE' | tar xzf - -C "$dest"
RD=$(eval printf '%s' "$1")
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cd "$RD" 2>/dev/null || exit 0
find sessions archived_sessions -name 'rollout-*.jsonl' -print0 2>/dev/null | \
while IFS= read -r -d '' f; do
  mkdir -p "$TMP/$(dirname "$f")"
  grep -aE '"token_count"|"session_meta"|"turn_context"' "$f" > "$TMP/$f" 2>/dev/null || true
done
if command -v python3 >/dev/null 2>&1; then
  python3 - "$TMP" <<'PY' 2>/dev/null || true
import json, os, sys
root = sys.argv[1]
for dirpath, _, files in os.walk(root):
    for fn in files:
        p = os.path.join(dirpath, fn)
        try:
            out = []
            with open(p, encoding='utf-8', errors='replace') as fh:
                for line in fh:
                    if '"session_meta"' in line and 'base_instructions' in line:
                        try:
                            d = json.loads(line)
                            pl = d.get('payload') or {}
                            if pl.get('base_instructions'):
                                pl['base_instructions'] = ''
                                line = json.dumps(d, ensure_ascii=False) + '\n'
                        except Exception:
                            pass
                    out.append(line)
            with open(p, 'w', encoding='utf-8') as fh:
                fh.writelines(out)
        except Exception:
            pass
PY
fi
cp session_index.jsonl "$TMP/" 2>/dev/null || true
tar czf - -C "$TMP" .
REMOTE
  echo "[$node] codex done: $(du -sh "$dest" 2>/dev/null | cut -f1)"
}

# ---------- claude pull: $1=user@host $2=node_label $3=remote_projects_dir ----------
pull_claude() {
  local host="$1" node="$2" remote_dir="$3"
  local dest="$MIRROR/$node/.claude/projects"
  mkdir -p "$dest"
  echo "[$node] claude: filtering + streaming ..."
  ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" bash -s -- "$remote_dir" 2>/dev/null <<'REMOTE' | tar xzf - -C "$dest"
RD=$(eval printf '%s' "$1")
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cd "$RD" 2>/dev/null || exit 0
find . -name '*.jsonl' -print0 2>/dev/null | \
while IFS= read -r -d '' f; do
  mkdir -p "$TMP/$(dirname "$f")"
  grep -aE '"usage"|"summary"' "$f" > "$TMP/$f" 2>/dev/null || true
done
tar czf - -C "$TMP" .
REMOTE
  echo "[$node] claude done: $(du -sh "$dest" 2>/dev/null | cut -f1)"
}

# ---------- kimi pull: $1=user@host $2=node_label $3=remote_sessions_dir ----------
pull_kimi() {
  local host="$1" node="$2" remote_dir="$3"
  local dest="$MIRROR/$node/.kimi-code/sessions"
  mkdir -p "$dest"
  echo "[$node] kimi: filtering + streaming ..."
  ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" bash -s -- "$remote_dir" 2>/dev/null <<'REMOTE' | tar xzf - -C "$dest"
RD=$(eval printf '%s' "$1")
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cd "$RD" 2>/dev/null || exit 0
find . -name wire.jsonl -print0 2>/dev/null | \
while IFS= read -r -d '' f; do
  mkdir -p "$TMP/$(dirname "$f")"
  grep -a 'usage.record' "$f" > "$TMP/$f" 2>/dev/null || true
done
tar czf - -C "$TMP" .
REMOTE
  echo "[$node] kimi done: $(du -sh "$dest" 2>/dev/null | cut -f1)"
}

# ---------- opencode trimmed pull: $1=user@host $2=node_label $3=remote_db_path ----------
pull_opencode_trim() {
  local host="$1" node="$2" remote_db="$3"
  local dest="$MIRROR/$node/opencode"
  mkdir -p "$dest"
  echo "[$node] opencode (trimmed) ..."
  ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" bash -s -- "$remote_db" 2>/dev/null <<'REMOTE' | tar xf - -C "$dest" 2>/dev/null || true
DB=$(eval printf '%s' "$1")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
[ -f "$DB" ] || exit 0
cat > "$T/exp.sql" <<SQL
ATTACH '$T/opencode.db' AS out;
CREATE TABLE out.session AS SELECT id,title,model,directory,time_created,time_updated FROM session;
CREATE TABLE out.message(id TEXT, session_id TEXT, time_created INTEGER, time_updated INTEGER, data TEXT);
INSERT INTO out.message(rowid,id,session_id,time_created,time_updated,data)
 SELECT rowid,id,session_id,time_created,time_updated,
  json_object('role','assistant','tokens',json_extract(data,'$.tokens'),
              'cost',json_extract(data,'$.cost'),'time',json_extract(data,'$.time'))
 FROM message WHERE json_extract(data,'$.role')='assistant';
SQL
sqlite3 "file:${DB}?mode=ro" < "$T/exp.sql" || exit 0
sqlite3 "$T/opencode.db" "VACUUM;" 2>/dev/null || true
cd "$T" && tar cf - opencode.db
REMOTE
  echo "[$node] opencode done: $(du -sh "$dest" 2>/dev/null | cut -f1)"
}

# ---------- execute node list ----------
for entry in "${SYNC_LINUX_NODES[@]:-}"; do
  [ -z "$entry" ] && continue
  IFS='|' read -r host label tools_csv <<< "$entry"
  IFS=',' read -ra tool_arr <<< "$tools_csv"
  for t in "${tool_arr[@]}"; do
    case "$t" in
      codex)    run pull_codex        "$host" "$label" '$HOME/.codex' ;;
      claude)   run pull_claude       "$host" "$label" '$HOME/.claude/projects' ;;
      kimi)     run pull_kimi         "$host" "$label" '$HOME/.kimi-code/sessions' ;;
      opencode) run pull_opencode_trim "$host" "$label" '$HOME/.local/share/opencode/opencode.db' ;;
      *) echo "skip unknown tool: $t" ;;
    esac
  done
done

if [ -n "${SYNC_WSL_NODE:-}" ]; then
  IFS='|' read -r wsl_host wsl_label win_label <<< "$SYNC_WSL_NODE"
  if [ "$CHECK_ONLY" = "1" ] || ssh -o BatchMode=yes -o ConnectTimeout=8 "$wsl_host" true 2>/dev/null; then
    run pull_codex        "$wsl_host" "$wsl_label" '$HOME/.codex'
    run pull_claude       "$wsl_host" "$wsl_label" '$HOME/.claude/projects'
    run pull_kimi         "$wsl_host" "$wsl_label" '$HOME/.kimi-code/sessions'
    run pull_opencode_trim "$wsl_host" "$wsl_label" '$HOME/.local/share/opencode/opencode.db'
    if [ -n "${win_label:-}" ]; then
      WU=$(ssh -o BatchMode=yes -o ConnectTimeout=8 "$wsl_host" \
           'ls /mnt/c/Users 2>/dev/null | grep -vE "^(Public|Default|All Users|WDAGUtilityAccount|defaultuser0|desktop.ini)$" | head -1' 2>/dev/null || true)
      if [ -n "${WU:-}" ]; then
        run pull_codex        "$wsl_host" "$win_label" "/mnt/c/Users/$WU/.codex"
        run pull_claude       "$wsl_host" "$win_label" "/mnt/c/Users/$WU/.claude/projects"
        run pull_opencode_trim "$wsl_host" "$win_label" "/mnt/c/Users/$WU/.local/share/opencode/opencode.db"
      else
        echo "[$win_label] cannot detect Windows user, skipped"
      fi
    fi
  else
    echo "[$wsl_label] not reachable, skipped"
  fi
fi

echo "=== mirrors ==="
du -sh "$MIRROR"/* 2>/dev/null
