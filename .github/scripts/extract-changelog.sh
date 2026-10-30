#!/bin/sh
# Prints the lines of the "## Changelog" section of a PR body (file argument or
# stdin) without HTML comments and bare type prefixes. Prints nothing when the
# section is missing or only says "none".
set -eu

awk '
	BEGIN { RS = "\0" }
	{ gsub(/\r/, ""); gsub(/<!--([^-]|-[^-]|--[^>])*-->/, ""); body = $0 }
	END {
		n = split(body, lines, "\n")
		for (i = 1; i <= n; i++) {
			if (in_sec && lines[i] ~ /^#+[ \t]/) break
			if (in_sec && lines[i] !~ /^[ \t]*([-*][ \t]*)?(Added|Changed|Fixed|Removed|Security)[ \t]*:?[ \t]*$/ && lines[i] ~ /[^ \t]/) {
				if (lines[i] !~ /^[ \t]*([-*][ \t]*)?[Nn]one[ \t.]*$/) print lines[i]
			}
			if (lines[i] ~ /^##[ \t]+[Cc]hangelog[ \t]*$/) in_sec = 1
		}
	}
' "$@"
