#!/bin/sh
# Appends the Changelog lines of a merged PR as one comment on the running
# "Changelog" issue, creating and pinning the issue when it is missing.
# Needs GH_TOKEN, REPO and PR_NUMBER in the environment. The PR body is read
# through the API and never executed or checked out.
set -eu

title=Changelog
here=$(dirname "$0")

pr=$(gh api "repos/$REPO/pulls/$PR_NUMBER")
[ "$(printf '%s' "$pr" | jq -r .merged)" = true ] || { echo "PR $PR_NUMBER is not merged, nothing to do."; exit 0; }

lines=$(printf '%s' "$pr" | jq -r '.body // ""' | sh "$here/extract-changelog.sh")
[ -n "$lines" ] || { echo "PR $PR_NUMBER has no changelog lines, skipping."; exit 0; }

url=$(printf '%s' "$pr" | jq -r .html_url)
sha=$(printf '%s' "$pr" | jq -r .merge_commit_sha)
date=$(printf '%s' "$pr" | jq -r '.merged_at[:10]')
commit="https://github.com/$REPO/commit/$sha"

# Open bot-authored "Changelog" issues, oldest first.
list_issues() {
	gh issue list --repo "$REPO" --state open --author 'github-actions[bot]' --limit 100 \
		--json number,title,author \
		--jq "map(select(.title == \"$title\" and .author.login == \"app/github-actions\")) | sort_by(.number) | .[].number"
}

number=$(list_issues | head -n 1)
if [ -z "$number" ]; then
	gh issue create --repo "$REPO" --title "$title" \
		--body 'Running list of the Changelog lines of merged pull requests, one comment per PR.' >/dev/null
	# Runs are not serialized, so a concurrent first run may have created one too: keep the oldest.
	issues=$(list_issues)
	number=$(printf '%s\n' "$issues" | head -n 1)
	for dup in $(printf '%s\n' "$issues" | tail -n +2); do
		gh issue close "$dup" --repo "$REPO" --reason 'not planned' --comment "Duplicate of #$number."
	done
	id=$(gh api "repos/$REPO/issues/$number" --jq .node_id)
	gh api graphql -f query='mutation($id: ID!) { pinIssue(input: {issueId: $id}) { issue { number } } }' \
		-f id="$id" >/dev/null || echo "Could not pin issue $number, pin it manually." >&2
	gh issue lock "$number" --repo "$REPO" || echo "Could not lock issue $number." >&2
fi

marker="<!-- changelog-feed pr=$PR_NUMBER -->"
# Only the bot's own comments count, and only their first line, so nobody can forge a marker.
if gh api --paginate "repos/$REPO/issues/$number/comments" \
	--jq '.[] | select(.user.login == "github-actions[bot]") | .body | split("\n")[0]' | grep -qxF -- "$marker"; then
	echo "PR $PR_NUMBER is already on issue $number, skipping."
	exit 0
fi

# Defuse @mentions so the feed never pings anyone.
lines=$(printf '%s\n' "$lines" | sed 's/@/@\xe2\x80\x8b/g')
gh issue comment "$number" --repo "$REPO" --body "$(printf '%s\n**%s** - %s - [`%.7s`](%s)\n\n%s\n' "$marker" "$date" "$url" "$sha" "$commit" "$lines")"
