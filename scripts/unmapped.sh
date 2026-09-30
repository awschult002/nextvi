#!/bin/sh
# List globals declared `extern` in vi.h that are neither a new name in any
# scripts/renames-*.tsv table nor listed in scripts/renames-keep.txt
# (names deliberately left as they are). After an upstream merge, anything
# printed here is a global upstream added: give it a table row or keep it.
# Exit status 1 if anything is unmapped.
cd "$(dirname "$0")/.."
perl -ne '
	next unless /^\s*extern\b/; s/\/\/.*|\/\*.*?\*\///g; s/\[[^\]]*\]//g; s/=[^,;]*//g;
	s/^\s*extern\s+(const\s+)?(unsigned\s+|signed\s+)?(struct\s+\w+|\w+)\s*//;
	s/[*;]//g; print "$_\n" for grep /\w/, map { s/^\s+|\s+$//gr } split /,/;
' vi.h | sort -u > /tmp/unmapped.$$
{ cut -f2 scripts/renames-*.tsv; grep -v '^#' scripts/renames-keep.txt 2>/dev/null; } | sort -u > /tmp/mapped.$$
out=$(comm -23 /tmp/unmapped.$$ /tmp/mapped.$$)
rm -f /tmp/unmapped.$$ /tmp/mapped.$$
[ -z "$out" ] && { echo "all vi.h globals mapped or kept"; exit 0; }
echo "vi.h globals with no rename row and not in renames-keep.txt:"
echo "$out" | sed 's/^/  /'
exit 1
