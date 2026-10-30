#!/bin/sh
# Regression check for .github/scripts/extract-changelog.sh and post-changelog.sh
# (the "Changelog feed" workflow), using a stub gh that records its calls.
set -eu

root=$1
scripts="$root/.github/scripts"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fail() { echo "changelog feed: $1" >&2; exit 1; }

extract() {
	printf '%b' "$1" | sh "$scripts/extract-changelog.sh"
}

[ "$(extract '## Changelog\nFixed: a bug\n')" = 'Fixed: a bug' ] || fail 'entry not extracted'
[ "$(extract '## Changelog\r\n- Added: x\r\n- Fixed: y\r\n## Notes\nz')" = "$(printf -- '- Added: x\n- Fixed: y')" ] || fail 'multi-line section'
[ -z "$(extract '## Changelog\nnone\n')" ] || fail 'none must be skipped'
[ -z "$(extract '## Changelog\n<!-- c -->\n- Added:\n')" ] || fail 'comment and bare prefix must be skipped'
[ -z "$(extract '## Summary\nx\n')" ] || fail 'missing section must be skipped'

mkdir "$tmp/bin"
cat >"$tmp/bin/gh" <<'STUB'
#!/bin/sh
echo "$*" >>"$STUB_LOG"
case "$1 $2" in
"api repos/o/r/pulls/7") cat "$STUB_PR" ;;
"issue list") printf '%s' "$STUB_ISSUE" ;;
"issue create") echo https://github.com/o/r/issues/9 ;;
"issue comment") printf '%s\n' "$*" >>"$STUB_LOG"; printf '%s' "$6" >"$STUB_BODY" ;;
esac
STUB
chmod +x "$tmp/bin/gh"

run() {
	body=$1 merged=$2 issue=$3
	: >"$tmp/log"
	jq -n --arg b "$body" --argjson m "$merged" \
		'{merged: $m, body: $b, html_url: "https://github.com/o/r/pull/7", merge_commit_sha: "abcdef0123456789", merged_at: "2026-01-02T03:04:05Z"}' >"$tmp/pr"
	PATH="$tmp/bin:$PATH" STUB_LOG="$tmp/log" STUB_PR="$tmp/pr" STUB_BODY="$tmp/comment" STUB_ISSUE=$issue \
		GH_TOKEN=x REPO=o/r PR_NUMBER=7 sh "$scripts/post-changelog.sh" >/dev/null 2>&1
}

run '## Changelog
Fixed: a bug' true 5
grep -q '^issue comment 5 ' "$tmp/log" || fail 'comment not posted to existing issue'
grep -q 'issue create' "$tmp/log" && fail 'existing issue must not be recreated'
grep -q 'abcdef0' "$tmp/log" && grep -q '2026-01-02' "$tmp/log" && grep -q 'pull/7' "$tmp/log" \
	&& grep -q 'commit/abcdef0123456789' "$tmp/log" && grep -q 'Fixed: a bug' "$tmp/log" || fail 'comment lacks date, PR link, commit link or lines'

run '## Changelog
Fixed: a bug' true ''
grep -q 'issue create' "$tmp/log" || fail 'missing issue must be created'
grep -q 'pinIssue' "$tmp/log" || fail 'created issue must be pinned'
grep -q '^issue comment 9 ' "$tmp/log" || fail 'comment not posted to the new issue'

run '## Changelog
none' true 5
grep -q 'issue comment' "$tmp/log" && fail 'none must not be posted'
run '## Changelog
Fixed: a bug' false 5
grep -q 'issue' "$tmp/log" && fail 'unmerged PR must not be posted'

echo 'changelog feed: ok'
