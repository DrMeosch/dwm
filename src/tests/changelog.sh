#!/bin/sh
# Regression check for .github/scripts/check-changelog.sh.
set -eu

root=$1
check="$root/.github/scripts/check-changelog.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

expect() {
	want=$1 name=$2
	printf '%b' "$3" >"$tmp/body"
	if sh "$check" "$tmp/body" 2>/dev/null; then got=0; else got=1; fi
	[ "$got" = "$want" ] || { echo "changelog check: $name: expected exit $want, got $got" >&2; exit 1; }
}

expect 0 'entry' '## Summary\nx\n\n## Changelog\nFixed: a bug\n'
expect 0 'none' '## Changelog\nnone'
expect 0 'last section, CRLF' '## Changelog\r\nAdded: x\r\n'
expect 0 'followed by section' '## Changelog\nnone\n## Notes\n'
expect 1 'missing' '## Summary\nx\n'
expect 1 'empty body' ''
expect 1 'empty section' '## Changelog\n\n## Notes\ny\n'
expect 1 'comment only' '## Changelog\n<!-- one line\nper change -->\n'
expect 1 'content only in other section' '## Changelog\n## Notes\ny\n'
expect 1 'wrong heading' '# Changelog\nnone\n'

expect 1 'bare prefix' '## Changelog\n- Added: \n'
expect 1 'bare prefix, no colon' '## Changelog\nFixed\n'
expect 0 'prefix with text' '## Changelog\n- Added: a thing\n'

if sh "$check" "$root/.github/pull_request_template.md" 2>/dev/null; then
	echo 'changelog check: untouched PR template must fail' >&2
	exit 1
fi

echo 'changelog check: ok'
