#!/bin/sh
# Succeeds when the PR body (file argument or stdin) has a non-empty
# "## Changelog" section. HTML comments do not count as content.
set -eu

awk '
	BEGIN { RS = "\0" }
	{ gsub(/\r/, ""); gsub(/<!--([^-]|-[^-]|--[^>])*-->/, ""); body = $0 }
	END {
		n = split(body, lines, "\n")
		for (i = 1; i <= n; i++) {
			if (in_sec && lines[i] ~ /^#+[ \t]/) break
			if (in_sec && lines[i] ~ /[^ \t]/) found = 1
			if (lines[i] ~ /^##[ \t]+[Cc]hangelog[ \t]*$/) in_sec = 1
		}
		exit !found
	}
' "$@" || {
	echo 'PR body needs a non-empty "## Changelog" section (write "none" for internal-only changes).' >&2
	exit 1
}
