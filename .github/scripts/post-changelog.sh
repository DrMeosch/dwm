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

number=$(gh issue list --repo "$REPO" --state open --author app/github-actions --limit 100 \
	--json number,title --jq "map(select(.title == \"$title\")) | first | .number // empty")
if [ -z "$number" ]; then
	issue=$(gh issue create --repo "$REPO" --title "$title" \
		--body 'Running list of the Changelog lines of merged pull requests, one comment per PR.')
	number=${issue##*/}
	id=$(gh api "repos/$REPO/issues/$number" --jq .node_id)
	gh api graphql -f query='mutation($id: ID!) { pinIssue(input: {issueId: $id}) { issue { number } } }' \
		-f id="$id" >/dev/null || echo "Could not pin issue $number, pin it manually." >&2
fi

gh issue comment "$number" --repo "$REPO" --body "$(printf '**%s** - %s - [`%.7s`](%s)\n\n%s\n' "$date" "$url" "$sha" "$commit" "$lines")"
