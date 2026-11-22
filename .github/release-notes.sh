#!/usr/bin/env bash
# Print the Markdown body of a release: the fixed fork description, then the
# changes since the previous published release, split into this fork's commits
# and the upstream changesets each "sync: upstream blastem @<hg>" merge brought.
#
#   .github/release-notes.sh <owner/repo> [<rev>]      (rev defaults to HEAD)
#
# Fork commits: first-parent, non-merge commits after the fork point. Upstream
# commits: the second-parent side of each sync merge, linked to retrodev by
# their Upstream-Rev trailer (sync-upstream.sh) or, for earlier replays, by
# .github/upstream-revs.txt.
#
# The previous release is the newest published (non-draft) release whose tag
# is an ancestor of <rev>. Needs the full history and the release tags
# (actions/checkout with fetch-depth: 0) and an authenticated gh.
set -euo pipefail

repo=${1:?usage: release-notes.sh <owner/repo> [<rev>]}
rev=$(git rev-parse --verify "${2:-HEAD}^{commit}")
short=$(git rev-parse --short=12 "$rev")
here=$(cd "$(dirname "$0")" && pwd)
hgweb=https://www.retrodev.com/repos/blastem/rev
# Where the fork branched off upstream (hg e623ea1d5363), as in sync-upstream.sh.
fork_base=47db172bb94181fdcfc15797a1f7b5a71c6df0bc
fork_base_hg=e623ea1d5363

prev=
while read -r t; do
  [ -n "$t" ] || continue
  c=$(git rev-parse -q --verify "refs/tags/$t^{commit}" 2>/dev/null) || continue
  [ "$c" != "$rev" ] || continue          # a re-run of this same release
  if git merge-base --is-ancestor "$c" "$rev"; then prev=$t; break; fi
done < <(gh release list --repo "$repo" --exclude-drafts --limit 100 \
           --json tagName,createdAt --jq 'sort_by(.createdAt) | reverse | .[].tagName')

if [ -n "$prev" ]; then
  title="Changes since ${prev}"
  range=("refs/tags/$prev..$rev" "^$fork_base")
else
  title="Changes"
  range=("$rev" "^$fork_base")
fi

# '<' escaped so "<name>"-style placeholders in subjects are not eaten as HTML
# (sed, not ${s//</&lt;}: bash 5.2 reads '&' in the replacement as the match).
esc() { sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g'; }

hg_of() {   # <git commit> -> hg node, or nothing
  local n
  n=$(git log -1 --format='%(trailers:key=Upstream-Rev,valueonly)' "$1" | head -n 1)
  [ -n "$n" ] || n=$(awk -v g="$1" '$1 == g { print $2; exit }' "$here/upstream-revs.txt")
  echo "$n"
}

fork_list=$(git log --first-parent --no-merges --format='%H %h %s' "${range[@]}" | esc |
  while read -r full abbrev subject; do
    echo "- ${subject} ([\`${abbrev}\`](https://github.com/${repo}/commit/${full}))"
  done)

upstream_list=
n_up=0
for m in $(git log --first-parent --merges --format=%H \
             --grep='^sync: upstream blastem @' "${range[@]}"); do
  node=$(git log -1 --format=%s "$m"); node=${node##*@}; node=${node:0:12}
  items=$(git log --format='%H %s' "$m^1..$m^2" | esc |
    while read -r full subject; do
      h=$(hg_of "$full")
      if [ -n "$h" ]; then
        echo "- ${subject} ([hg \`${h:0:12}\`](${hgweb}/${h:0:12}))"
      else
        echo "- ${subject} ([\`${full:0:7}\`](https://github.com/${repo}/commit/${full}))"
      fi
    done)
  n_up=$((n_up + $(git rev-list --count "$m^1..$m^2")))
  upstream_list+="
#### Upstream BlastEm, synced to [hg ${node}](${hgweb}/${node})

${items}
"
done

n_fork=$(git rev-list --first-parent --no-merges --count "${range[@]}")
plural() { [ "$1" = 1 ] && echo "1 $2" || echo "$1 $2s"; }
if [ "$n_up" -gt 0 ]; then
  counts="$(plural "$n_fork" "fork commit"), $(plural "$n_up" "upstream commit")"
else
  counts=$(plural "$n_fork" commit)
fi

cat <<EOF
Patched (debug-capable) BlastEm — GDB remote ( -D / BLASTEM_GDB_PORT ), a scriptable Unix-domain control socket ( BLASTEM_CTRL_SOCK ), and an embedded UI font. Built from ${repo}@${short}.

EOF
# Collapsed by default: <details> without "open". The blank line after
# </summary> is what makes GitHub render the Markdown inside it.
echo "<details>"
echo "<summary><b>${title}</b> (${counts})</summary>"
if [ -n "$fork_list" ]; then
  printf '\n#### This fork\n\n%s\n' "$fork_list"
fi
[ -z "$upstream_list" ] || printf '%s' "$upstream_list"
if [ -n "$prev" ]; then
  printf '\n**Full diff**: https://github.com/%s/compare/%s...%s\n' "$repo" "$prev" "$short"
else
  printf '\nForked from upstream BlastEm [hg %s](%s/%s).\n' "$fork_base_hg" "$hgweb" "$fork_base_hg"
fi
echo
echo "</details>"

cat <<'EOF'

Fork changes are dedicated to the public domain (The Unlicense — see COPYING.fork); BlastEm itself is GPLv3 (see COPYING).
EOF
