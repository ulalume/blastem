#!/bin/zsh
# sync-upstream.sh — pull new retrodev BlastEm (Mercurial) changesets and land
# them in this git fork as real per-changeset commits, then merge into main.
#
# Why not hg-fast-export: the .git/hg2git-* marks from the original conversion
# went stale when history was rewritten with git-filter-repo, so an incremental
# fast-export would graft onto orphaned pre-rewrite commits. This script
# replays changesets patch-by-patch instead: for each new upstream rev it
# applies `hg diff prev cur` and commits with the original author/date/message,
# plus an "Upstream-Rev: <hg node>" trailer (earlier replays: .github/upstream-revs.txt).
#
# Self-contained — nothing outside this repository is needed:
#   - the Mercurial mirror is a private cache inside the git dir
#     (<git-common-dir>/hg-upstream, no working copy), cloned on first run and
#     updated with `hg pull -r <tip>` afterwards;
#   - the previous sync point comes from the last "sync: upstream blastem @<node>"
#     merge commit: <node> in its subject, and its second parent is the git
#     commit holding that upstream tree. No local-only branch is involved;
#   - the replay runs in a temporary worktree, so your checkout is never
#     switched to the pure-upstream tree and ignored build output (obj/, the
#     blastem binary, generated cores, local notes) can never be picked up;
#   - the result is verified by comparing git tree hashes (the replayed commit
#     vs. an `hg archive` of the upstream tip), independent of the working dir.
#
# Usage:  ./sync-upstream.sh [--seed <local-hg-clone>]
#   - run from the repo, on a clean working tree, on the branch to merge into
#   - --seed only matters on the first run: clone the cache from an existing
#     local hg clone instead of retrodev (saves a full clone over the network)
#   - on a merge conflict the merge is left in progress: resolve the files,
#     then `git commit --no-edit` (keeps the sync marker message)
set -euo pipefail

HG_URL=https://www.retrodev.com/repos/blastem
SEED=
if [ "${1:-}" = "--seed" ]; then
  SEED=${2:?"--seed needs a path to a local hg clone"}
elif [ -n "${1:-}" ]; then
  echo "usage: $0 [--seed <local-hg-clone>]" >&2; exit 2
fi

# ---- sanity ----------------------------------------------------------------
git rev-parse --is-inside-work-tree >/dev/null
if [ -n "$(git status --porcelain)" ]; then
  echo "error: working tree not clean" >&2; exit 1
fi
command -v hg >/dev/null || { echo "error: mercurial (hg) not installed" >&2; exit 1; }
GIT_COMMON=$(git rev-parse --path-format=absolute --git-common-dir)
HG_CACHE="$GIT_COMMON/hg-upstream"

# ---- find last synced upstream node & its git commit -----------------------
# Falls back to the original conversion boundary (hg 2840 e623ea1d5363 == git 47db172).
sync_merge=$(git log --grep='^sync: upstream blastem @' -1 --format='%H' || true)
if [ -n "$sync_merge" ]; then
  subject=$(git log -1 --format='%s' "$sync_merge")
  BASE_NODE=${subject##*@}
  BASE_GIT=$(git rev-parse --verify -q "$sync_merge^2") \
    || { echo "error: sync marker ${sync_merge:0:12} is not a merge commit" >&2; exit 1; }
else
  BASE_NODE=e623ea1d5363c33dc4b4f57ea8df3b4a28ae2ee6
  BASE_GIT=47db172bb94181fdcfc15797a1f7b5a71c6df0bc
fi

# ---- update the private hg cache -------------------------------------------
if [ ! -d "$HG_CACHE/.hg" ]; then
  echo "creating hg cache: $HG_CACHE${SEED:+ (seeded from $SEED)}"
  hg clone -q -U "${SEED:-$HG_URL}" "$HG_CACHE"
  printf '[paths]\ndefault = %s\n' "$HG_URL" > "$HG_CACHE/.hg/hgrc"
fi
REMOTE_TIP=$(hg identify -q "$HG_URL")
echo "upstream tip: $REMOTE_TIP   last synced: ${BASE_NODE:0:12}"
hg -R "$HG_CACHE" pull -q -r "$REMOTE_TIP" "$HG_URL"
NEW=$(hg -R "$HG_CACHE" log -r "sort(::$REMOTE_TIP and not ::$BASE_NODE, rev)" -T '{node}\n')
if [ -z "$NEW" ]; then echo "already up to date"; exit 0; fi
echo "new changesets: $(echo "$NEW" | wc -l | tr -d ' ')"

# ---- replay in a temporary worktree ----------------------------------------
tmp=$(mktemp -d)
wt="$tmp/wt"
cleanup() {
  git worktree remove --force "$wt" 2>/dev/null || true
  git worktree prune 2>/dev/null || true
  rm -rf "$tmp"
}
trap cleanup EXIT
git worktree add -q --detach "$wt" "$BASE_GIT"
prev=$BASE_NODE
echo "$NEW" | while read -r node; do
  hg -R "$HG_CACHE" diff --git -X path:.hgtags -r "$prev" -r "$node" > "$tmp/p"
  [ -s "$tmp/p" ] && git -C "$wt" apply --index --binary --whitespace=nowarn "$tmp/p"
  author=$(hg -R "$HG_CACHE" log -r "$node" -T '{author}')
  adate=$(hg -R "$HG_CACHE" log -r "$node" -T '{date|rfc822date}')
  hg -R "$HG_CACHE" log -r "$node" -T '{desc}' > "$tmp/msg"
  # Release notes link each upstream commit to retrodev by this trailer.
  git interpret-trailers --in-place --trailer "Upstream-Rev: $node" "$tmp/msg"
  GIT_COMMITTER_NAME="${author%% <*}" \
  GIT_COMMITTER_EMAIL="$(echo "$author" | sed -n 's/.*<\(.*\)>.*/\1/p')" \
  GIT_COMMITTER_DATE="$adate" \
  git -C "$wt" commit -q --allow-empty --author="$author" --date="$adate" -F "$tmp/msg"
  prev=$node
done
REPLAY_TIP=$(git -C "$wt" rev-parse HEAD)

# ---- verify: replayed tree == upstream tip, compared as git tree hashes ----
hg -R "$HG_CACHE" archive -r "$REMOTE_TIP" "$tmp/hgtip"
rm -f "$tmp/hgtip/.hg_archival.txt" "$tmp/hgtip/.hgtags"
GIT_INDEX_FILE="$tmp/index" git --work-tree="$tmp/hgtip" add -A --force -- "$tmp/hgtip"
want=$(GIT_INDEX_FILE="$tmp/index" git write-tree)
got=$(git rev-parse "$REPLAY_TIP^{tree}")
if [ "$want" != "$got" ]; then
  echo "error: replayed tree ($got) differs from upstream archive ($want) — aborting before merge" >&2
  exit 1
fi
echo "tree verified against upstream archive (${got:0:12})"

# ---- merge into the current branch -----------------------------------------
if ! git merge --no-ff -m "sync: upstream blastem @$REMOTE_TIP" "$REPLAY_TIP"; then
  echo >&2
  echo "merge conflict: resolve the files listed above, then run 'git commit --no-edit'" >&2
  echo "(the replayed upstream commits end at ${REPLAY_TIP:0:12}; 'git merge --abort' to back out)" >&2
  exit 1
fi
echo "done: merged upstream @$REMOTE_TIP"
