#!/bin/sh
# Replay a whole-word rename table (old<TAB>new per line) over the C sources.
# Also renames tmp<old> locals made by the preserve()/restore() macros in vi.h.
# Usage: scripts/rename.sh scripts/renames-globals.tsv
# Refuses to run if any new name already exists as a whole word (collision).
set -e
tbl=$1
cd "$(dirname "$0")/.."
while IFS='	' read -r old new; do
	[ -z "$old" ] && continue
	if grep -qw -- "$new" *.c *.h; then
		echo "collision: $new already used" >&2; exit 1
	fi
done < "$tbl"
while IFS='	' read -r old new; do
	[ -z "$old" ] && continue
	perl -pi -e "s/\\b(tmp)?\Q$old\E\\b/\$1$new/g" *.c *.h
done < "$tbl"
