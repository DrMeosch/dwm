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
[ -z "$(extract '## Changelog\r\n- None.\r\n')" ] || fail 'None. must be skipped'
[ -z "$(extract '## Changelog\n<!-- c -->\n- Added:\n')" ] || fail 'comment and bare prefix must be skipped'
[ -z "$(extract '## Summary\nx\n')" ] || fail 'missing section must be skipped'

mkdir "$tmp/bin"
# Stateful stub: STUB_ISSUE is the number of an existing bot-authored issue (or empty);
# issue list only returns it for --author 'github-actions[bot]'.
cat >"$tmp/bin/gh" <<'STUB'
#!/bin/sh
echo "$*" >>"$STUB_LOG"
case "$1 $2" in
"api repos/o/r/pulls/7") cat "$STUB_PR" ;;
"api --paginate") cat "$STUB_COMMENTS" 2>/dev/null || : ;;
"issue list")
	case "$*" in
	*"--author github-actions[bot]"*)
		if [ -f "$STUB_CREATED" ]; then printf '%s\n' $STUB_ISSUE_AFTER; else printf '%s\n' $STUB_ISSUE; fi ;;
	esac ;;
"issue create") : >"$STUB_CREATED"; echo https://github.com/o/r/issues/9 ;;
"issue comment") printf '%s\n' "$7" >>"$STUB_COMMENTS" ;;
esac
STUB
chmod +x "$tmp/bin/gh"

run() {
	body=$1 merged=$2 issue=$3
	: >"$tmp/log"; rm -f "$tmp/created"
	jq -n --arg b "$body" --argjson m "$merged" \
		'{merged: $m, body: $b, html_url: "https://github.com/o/r/pull/7", merge_commit_sha: "abcdef0123456789", merged_at: "2026-01-02T03:04:05Z"}' >"$tmp/pr"
	PATH="$tmp/bin:$PATH" STUB_LOG="$tmp/log" STUB_PR="$tmp/pr" STUB_COMMENTS="$tmp/comments" STUB_ISSUE=$issue STUB_ISSUE_AFTER=${4:-9} STUB_CREATED="$tmp/created" \
		GH_TOKEN=x REPO=o/r PR_NUMBER=7 sh "$scripts/post-changelog.sh" >/dev/null 2>&1
}

rm -f "$tmp/comments"
run '## Changelog
Fixed: a bug for @someone' true 5
grep -q '^issue comment 5 ' "$tmp/log" || fail 'comment not posted to existing issue'
grep -q 'issue create' "$tmp/log" && fail 'existing issue must not be recreated'
grep -q 'abcdef0' "$tmp/comments" && grep -q '2026-01-02' "$tmp/comments" && grep -q 'pull/7' "$tmp/comments" \
	&& grep -q 'commit/abcdef0123456789' "$tmp/comments" && grep -q 'Fixed: a bug' "$tmp/comments" \
	&& grep -qF '<!-- changelog-feed pr=7 -->' "$tmp/comments" || fail 'comment lacks marker, date, PR link, commit link or lines'
grep -q '@someone' "$tmp/comments" && fail '@mention must be defused'

run '## Changelog
Fixed: a bug for @someone' true 5
[ "$(grep -c 'changelog-feed pr=7' "$tmp/comments")" = 1 ] || fail 're-run must not post a second comment'

rm -f "$tmp/comments"
run '## Changelog
Fixed: a bug' true ''
grep -q 'issue create' "$tmp/log" || fail 'missing issue must be created'
grep -q 'pinIssue' "$tmp/log" || fail 'created issue must be pinned'
grep -q '^issue comment 9 ' "$tmp/log" || fail 'comment not posted to the new issue'

# A concurrent first run created issue 4 before ours (9): keep 4, close 9, pin 4.
rm -f "$tmp/comments"
run '## Changelog
Fixed: a bug' true '' '4 9'
grep -q '^issue close 9 ' "$tmp/log" || fail 'newer duplicate must be closed'
grep -q '^issue close 4 ' "$tmp/log" && fail 'oldest issue must be kept'
grep -q '^issue comment 4 ' "$tmp/log" || fail 'comment must go to the oldest issue'
grep -q 'issues/4 ' "$tmp/log" || fail 'oldest issue must be pinned'

for none in none '- None.' 'None'; do
	run "## Changelog
$none" true 5
	grep -q 'issue comment' "$tmp/log" && fail "'$none' must not be posted"
done
run '## Changelog
Fixed: a bug' false 5
grep -q 'issue' "$tmp/log" && fail 'unmerged PR must not be posted'

echo 'changelog feed: ok'
