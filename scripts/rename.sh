#!/bin/sh
# Replay a whole-word rename table (old<TAB>new per line) over the C sources.
# Usage: scripts/rename.sh scripts/renames-globals.tsv   (then -term, -vi-lnum)
#
# nextvi is a unity build (vi.c #includes every .c), so renames and collision
# checks always cover every *.c and *.h in the tree.
# Per row: old present, new absent -> rename; old absent, new present ->
# "already applied", skipped; both present -> collision, abort (nothing is
# changed). Also renames tmp<old> locals made by preserve()/restore() in vi.h.
# If ex.c still has the upstream EO(opt) macro (which pastes x##opt), it is
# first rewritten to EO(opt, var) so option globals can be renamed.
set -e
tbl=$1
[ -f "$tbl" ] || { echo "usage: $0 table.tsv" >&2; exit 2; }
cd "$(dirname "$0")/.."

if grep -q 'x##opt' ex.c; then
	perl -0pi -e 's/#define EO\(opt\) \\\n\t_EO\(opt, x##opt = \*arg \? eo_val\(arg\) : !x##opt; return NULL;\)/#define EO(opt, var) \\\n\t_EO(opt, var = *arg ? eo_val(arg) : !var; return NULL;)/;
		s/(return NULL;\)\n\n)((?:EO\(\w+\)[ \n]*)+)/my ($h, $b) = ($1, $2); $b =~ s#EO\((\w+)\)#EO($1, x$1)#g; $h . $b/e' ex.c
	grep -q 'x##opt' ex.c && { echo "EO() macro rewrite failed; see ex.c" >&2; exit 1; }
	echo "rewrote EO(opt) -> EO(opt, var) in ex.c"
fi

has() { grep -qw -- "$1" *.c *.h; }
todo=$(mktemp)
trap 'rm -f "$todo"' EXIT
bad=0
while IFS='	' read -r old new; do
	[ -z "$old" ] && continue
	if has "$old" && has "$new"; then
		echo "collision: $new already used while $old still exists" >&2; bad=1
	elif has "$old"; then
		printf '%s\t%s\n' "$old" "$new" >> "$todo"
	elif has "$new"; then
		echo "already applied: $old -> $new"
	else
		echo "not found, skipped: $old"
	fi
done < "$tbl"
[ $bad = 0 ] || exit 1
while IFS='	' read -r old new; do
	perl -pi -e "s/\\b(tmp)?\Q$old\E\\b/\$1$new/g" *.c *.h
done < "$todo"
echo "renamed $(wc -l < "$todo") names"

# Report vi.h globals that no table maps (e.g. new upstream globals). Informational only.
scripts/unmapped.sh || true
