#!/bin/sh
# Rebuild the AWS branch from bare upstream, following patches/aws/series.
#
# Usage: scripts/rebuild-aws.sh [-b branch] [-d dir] [-a aws] [-k] [-t] [upstream-ref]
#        scripts/rebuild-aws.sh --check-export [-a aws]
#        scripts/rebuild-aws.sh --export COMMIT
#   upstream-ref  commit to start from (default: master)
#   -b branch     new local branch (default: aws-rebuild-<short sha>)
#   -d dir        worktree for it (default: ../nextvi-<branch> next to this checkout)
#   -a aws        the AWS branch the export guard checks (default: origin/AWS, fetched)
#   -k            keep going after an unmapped.sh difference (report only)
#   -t            run the export guard, then the rebuild, then print the commands
#                 that tag the old AWS and move AWS (never runs them)
#   --check-export  only run the export guard
#   --export COMMIT  format-patch COMMIT into patches/aws (next number) and add
#                 an `am` line before `selfcopy` in the series; commit the result
#
# Export guard: rebuild the upstream base of AWS with this series in a
# temporary worktree and diff it against AWS (merged with this checkout's
# HEAD, so commits pending here count as exported). Anything left, outside
# scripts/rebuild-aws.sh, patches/aws/ and a Makefile AWS does not have yet, is
# a commit on AWS that was never exported: the guard lists it and fails. The
# base is the `upstream-base` line of that merged patches/aws/series (selfcopy
# writes it); without one, git merge-base AWS upstream-ref, which is only
# right if upstream-ref is not behind the base.
#
# Steps (see patches/aws/series):
#   script NAME [VAR=val...]  run kyx0r's patch script NAME.sh from the pinned
#                             patches-branch commit with DBG1=1, commit "patch: NAME"
#   expect NAME LINE          a known anchor report line of NAME that is not a failure
#   am FILE                   git am patches/aws/FILE
#   ref                       build, save the binary as the new reference, check make
#   rename TABLE MESSAGE      scripts/rename.sh scripts/renames-TABLE.tsv, commit
#   unmapped                  scripts/unmapped.sh, compared with patches/aws/unmapped.expected
#   selfcopy                  commit this script and patches/aws into the new branch
# After `ref`, every step that changes a .c/.h file is built and the stripped
# binary compared with the reference. Nothing is pushed. Stops at the first
# failing anchor, conflict or check and names the step.
set -eu

die() { printf '\nSTOP at step %s: %s\n' "${STEP:-setup}" "$*" >&2; exit 1; }
say() { printf '%s\n' "$*"; }

BR= DIR= KEEP=0 TAG=0 CHECK=0 EXPORT= AWS=
case ${1:-} in
--check-export) CHECK=1; shift ;;
--export) EXPORT=${2:?--export needs a commit}; shift 2 ;;
esac
while getopts a:b:d:kt o; do
	case $o in
	a) AWS=$OPTARG ;; b) BR=$OPTARG ;; d) DIR=$OPTARG ;; k) KEEP=1 ;; t) TAG=1 ;;
	*) sed -n '2,24s/^# \{0,1\}//p' "$0" >&2; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
UP=${1:-master}

SRC=$(cd "$(dirname "$0")/.." && pwd)
PD=$SRC/patches/aws
SERIES=$PD/series
[ -f "$SERIES" ] || die "no $SERIES"
UPSHA=$(git -C "$SRC" rev-parse --verify "$UP^{commit}") || die "unknown ref $UP"
SHORT=$(git -C "$SRC" rev-parse --short=8 "$UPSHA")
BR=${BR:-aws-rebuild-$SHORT}
DIR=${DIR:-$(dirname "$SRC")/nextvi-$BR}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
unset CFLAGS CPPFLAGS LDFLAGS LDLIBS || true
export CC=cc

if [ -n "$EXPORT" ]; then
	c=$(git -C "$SRC" rev-parse --verify "$EXPORT^{commit}") || die "unknown commit $EXPORT"
	pid=$(git -C "$SRC" show "$c" | git patch-id --stable | cut -d' ' -f1)
	for f in "$PD"/*.patch; do
		[ "$(git patch-id --stable < "$f" | cut -d' ' -f1)" = "$pid" ] && die "$EXPORT is already exported as ${f##*/}"
	done
	n=$(ls "$PD" | sed -n 's/^\([0-9][0-9]*\)-.*\.patch$/\1/p' | sort -n | tail -1 | sed 's/^0*//')
	f=$(git -C "$SRC" format-patch -1 --zero-commit --no-signature \
		--start-number $((${n:-0} + 1)) -o "$PD" "$c")
	awk -v l="am ${f##*/}" '$1 == "selfcopy" && !d { print l; d = 1 } { print }' "$SERIES" > "$TMP/series"
	grep -q "^am ${f##*/}\$" "$TMP/series" || die "no selfcopy line in $SERIES"
	cat "$TMP/series" > "$SERIES"
	say "exported $(git -C "$SRC" log -1 --format='%h %s' "$c") as patches/aws/${f##*/}"
	exit 0
fi

# Export guard (see the header). Rebuilds by running this script on the base.
guard() {
	if [ -z "$AWS" ]; then
		AWS=origin/AWS
		git -C "$SRC" fetch -q origin AWS || say "warning: cannot fetch origin AWS; using the local $AWS"
	fi
	git -C "$SRC" rev-parse -q --verify "$AWS^{commit}" >/dev/null || die "unknown AWS ref $AWS"
	want=$(git -C "$SRC" merge-tree --write-tree "$AWS" HEAD) || die "$AWS and HEAD do not merge cleanly"
	base=$(git -C "$SRC" show "$want:patches/aws/series" 2>/dev/null | sed -n 's/^upstream-base[ \t]*//p')
	[ -n "$base" ] || base=$(git -C "$SRC" merge-base "$AWS" "$UPSHA") ||
		die "no upstream-base in the series and no merge base of $AWS and $UP"
	git -C "$SRC" merge-base --is-ancestor "$base" "$AWS" || die "upstream-base $base is not in $AWS"
	set -- . ':!scripts/rebuild-aws.sh' ':!patches/aws'
	git -C "$SRC" cat-file -e "$AWS:Makefile" 2>/dev/null || set -- "$@" ':!Makefile'
	gb=aws-export-guard-$$
	say "export guard: rebuilding $AWS's upstream base $(echo "$base" | cut -c1-8) ..."
	sh "$SRC/scripts/rebuild-aws.sh" -b "$gb" -d "$TMP/guard" "$base" > "$TMP/guard.log" 2>&1 || {
		tail -15 "$TMP/guard.log" >&2
		git -C "$SRC" worktree remove --force "$TMP/guard" 2>/dev/null || true
		git -C "$SRC" branch -q -D "$gb" 2>/dev/null || true
		die "export guard: the rebuild of $base failed (log above)"; }
	git -C "$SRC" diff --stat "$want" "$gb" -- "$@" > "$TMP/guard.stat"
	paths=$(git -C "$SRC" diff --name-only "$want" "$gb" -- "$@")
	git -C "$SRC" worktree remove --force "$TMP/guard"
	git -C "$SRC" branch -q -D "$gb"; rm -f "$TMP/guard.ref.bin"
	if [ -s "$TMP/guard.stat" ]; then
		say "export guard FAILED: $AWS differs from a rebuild of $base:" >&2
		cat "$TMP/guard.stat" >&2
		for f in $(sed -n 's/^am[ \t]*//p' "$SERIES"); do git patch-id --stable < "$PD/$f"; done |
			cut -d' ' -f1 > "$TMP/pids"
		say "commits on $AWS since $(echo "$base" | cut -c1-8) touching these paths and not in the series:" >&2
		for c in $(git -C "$SRC" rev-list --no-merges "$base..$AWS" -- $paths); do
			pid=$(git -C "$SRC" show "$c" | git patch-id --stable | cut -d' ' -f1)
			grep -qx "$pid" "$TMP/pids" || git -C "$SRC" log -1 --format='  %h %ad %s' --date=short "$c" >&2
		done
		say "export them with: scripts/rebuild-aws.sh --export COMMIT" >&2
		exit 1
	fi
	say "export guard: a rebuild of $(echo "$base" | cut -c1-8) reproduces $AWS"
}
if [ $CHECK = 1 ]; then guard; exit 0; fi
[ $TAG = 1 ] && guard

# Pinned kyx0r patches-branch commit: "patches-commit SHA URL" in the series.
set -- $(sed -n 's/^patches-commit[ \t]*//p' "$SERIES")
PC=$1 PURL=$2
if ! git -C "$SRC" cat-file -e "$PC^{commit}" 2>/dev/null; then
	git -C "$SRC" fetch -q "$PURL" "$PC" 2>/dev/null ||
		git -C "$SRC" fetch -q "$PURL" patches || die "cannot fetch $PURL"
	git -C "$SRC" cat-file -e "$PC^{commit}" 2>/dev/null || die "patches commit $PC not found"
fi
say "upstream $UPSHA, patches-branch commit $PC, branch $BR in $DIR"

# nextvi is the patch scripts' engine: build it from the bare upstream ref.
mkdir "$TMP/tool" "$TMP/p"
git -C "$SRC" archive "$UPSHA" | tar -x -C "$TMP/tool"
(cd "$TMP/tool" && $CC -O2 -D_POSIX_C_SOURCE=200809L vi.c -o "$TMP/vi_tool" 2>/dev/null) ||
	die "cannot build nextvi from $UP"

git -C "$SRC" worktree add -q -b "$BR" "$DIR" "$UPSHA" || die "cannot create $BR in $DIR"
cd "$DIR"
# Reuse the pinned tree-sitter checkouts if this checkout has them (the build
# verifies they match the pin); otherwise the first build fetches them.
[ -d "$SRC/.treesitter" ] && ln -s "$SRC/.treesitter" .treesitter

# Stripped binary without the build-id note, so two builds of one tree compare equal.
norm() { strip "$1" && objcopy -R .note.gnu.build-id "$1"; }
# The reference build: the same command that produced f2aa0fe9 (docs/PATCHES.md).
REF_FLAGS="-O2 -D_POSIX_C_SOURCE=200809L"
refbuild() {
	ts=$(make -s print-flags | grep -e '^-I' -e '^-D_DEFAULT_SOURCE$' -e '\.c$' | grep -v '^vi\.c$' | tr '\n' ' ')
	$CC vi.c $ts -o "$1" $REF_FLAGS 2>"$TMP/ref.log" || { cat "$TMP/ref.log" >&2; die "build failed"; }
	norm "$1"
}
makebuild() {
	make -s clean >/dev/null 2>&1 || true
	make -s vi >"$TMP/make.log" 2>&1 || { tail -20 "$TMP/make.log" >&2; die "make failed"; }
	cp vi "$1" && norm "$1" && make -s clean >/dev/null 2>&1
}
cbuild() {
	rm -f vi
	sh ./cbuild.sh build >"$TMP/cbuild.log" 2>&1 && [ -x vi ] ||
		{ tail -20 "$TMP/cbuild.log" >&2; die "cbuild.sh build failed"; }
	cp vi "$1" && norm "$1" && rm -f vi
}
# Drift guard: the flags, defines, include paths, libs and sources cbuild.sh
# passes to the compiler must be the ones the Makefile uses (as sets).
flagcheck() {
	printf '#!/bin/sh\nfor a; do printf "%%s\\n" "$a"; done >> %s\n' "$TMP/cc.args" > "$TMP/ccrec"
	chmod +x "$TMP/ccrec"; : > "$TMP/cc.args"; rm -f vi
	CC=$TMP/ccrec sh ./cbuild.sh build >/dev/null 2>&1 || die "cbuild.sh dry run failed"
	awk 'p { p = 0; next } $0 == "-o" { p = 1; next } { print }' "$TMP/cc.args" | sort -u > "$TMP/cb.flags"
	make -s print-flags | sort -u > "$TMP/mk.flags"
	diff -u "$TMP/cb.flags" "$TMP/mk.flags" > "$TMP/flags.diff" ||
		{ sed 's/^--- .*/--- cbuild.sh/; s/^+++ .*/+++ Makefile/' "$TMP/flags.diff" >&2
		  die "cbuild.sh and Makefile flags differ"; }
	say "  flags: cbuild.sh == Makefile ($(wc -l < "$TMP/mk.flags") tokens)"
}
makecheck() {
	flagcheck
	cbuild "$TMP/cb.bin"; makebuild "$TMP/mk.bin"
	cmp -s "$TMP/cb.bin" "$TMP/mk.bin" || die "make binary differs from cbuild.sh binary"
	say "  make == cbuild.sh ($(sha256sum < "$TMP/mk.bin" | cut -c1-12))"
}
check() {
	[ -f "$TMP/ref.bin" ] || return 0
	git diff --name-only HEAD~1 HEAD | grep -q '\.[ch]$' || { say "  no C change"; return 0; }
	refbuild "$TMP/cur.bin"
	cmp -s "$TMP/ref.bin" "$TMP/cur.bin" || die "binary differs from the reference"
	say "  binary == reference"
}
# The patch scripts exit 0 even when an anchor fails; their DBG1 report lines
# are checked instead. Everything but OK, compat and file-write lines must be
# listed as `expect` for that script.
runscript() {
	n=$1; shift
	git -C "$SRC" show "$PC:$n.sh" > "$TMP/p/$n.sh" || die "no $n.sh in $PC"
	env P2VI_PATCH="$APPLIED" DBG1=1 VI="$TMP/vi_tool" "$@" sh "$TMP/p/$n.sh" > "$TMP/$n.out" 2>&1 < /dev/null ||
		{ tail -20 "$TMP/$n.out" >&2; die "$n.sh exited non-zero"; }
	sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' "$TMP/$n.out" | tr '\r' '\n' |
		grep -a -v -e '^[[:space:]]*$' -e '^OK ' -e '^compat [0-9]* applied' |
		grep -a -v -E '^"[^"]+" [0-9]+L \[[rwn]\]$' | sort -u > "$TMP/$n.rep" || true
	awk -v n="$n" '$1 == "expect" && $2 == n { sub(/^expect[ \t]+[^ \t]+[ \t]+/, ""); print }' "$SERIES" |
		sort -u > "$TMP/$n.exp"
	if ! cmp -s "$TMP/$n.rep" "$TMP/$n.exp"; then
		say "  report lines not expected (+) or expected but missing (-):" >&2
		diff "$TMP/$n.exp" "$TMP/$n.rep" | sed -n 's/^> /  + /p; s/^< /  - /p' >&2
		die "$n.sh anchor report differs"
	fi
	[ -s "$TMP/$n.rep" ] && sed 's/^/  known: /' "$TMP/$n.rep"
	rm -f vi
	git add -A
	{ echo "patch: $n"; echo
	  echo "Applied by scripts/rebuild-aws.sh from kyx0r/nextvi patches@$PC"
	  echo "(DBG1=1${*:+ $*})."
	  [ -s "$TMP/$n.rep" ] && { echo; echo "Known anchor report:"; sed 's/^/  /' "$TMP/$n.rep"; }
	} > "$TMP/msg"
	git commit -q --allow-empty -F "$TMP/msg"
	APPLIED="${APPLIED:+$APPLIED }$n.sh"
}

APPLIED=
i=0
grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' "$SERIES" > "$TMP/steps"
while read -r op a rest <&3; do
	case $op in patches-commit|upstream-base|expect) continue ;; esac
	i=$((i + 1)); STEP="$i ($op${a:+ $a})"
	say "[$i] $op ${a:-} ${rest:-}"
	case $op in
	script) runscript "$a" $rest ;;
	am)	git am -q --3way "$PD/$a" < /dev/null ||
			{ git am --show-current-patch=diff >/dev/null 2>&1; git status --short >&2
			  die "git am $a failed (resolve, then git am --continue; or git am --abort)"; }
		check ;;
	ref)	refbuild "$TMP/ref.bin"; cp "$TMP/ref.bin" "$DIR.ref.bin"
		say "  new reference: $DIR.ref.bin $(sha256sum < "$TMP/ref.bin" | cut -c1-64)"
		makecheck; cp "$TMP/mk.bin" "$TMP/mk0.bin" ;;
	rename)	sh scripts/rename.sh "scripts/renames-$a.tsv" > "$TMP/rename.out" 2>&1 ||
			{ cat "$TMP/rename.out" >&2; die "rename.sh $a failed"; }
		grep -E '^(renamed|not found)' "$TMP/rename.out" | sed 's/^/  /'
		git add -A; git commit -q -m "$rest"; check ;;
	unmapped)
		sh scripts/unmapped.sh > "$TMP/unmapped.out" 2>&1 || true
		sed -n 's/^  \([^ ]*\).*/\1/p' "$TMP/unmapped.out" > "$TMP/unmapped.now"
		if cmp -s "$TMP/unmapped.now" "$PD/unmapped.expected"; then
			say "  unmapped.sh: $(wc -l < "$TMP/unmapped.now") names, as expected (upstream statics no table renames)"
		else
			say "  unmapped.sh differs from patches/aws/unmapped.expected (+ new, - gone):" >&2
			diff "$PD/unmapped.expected" "$TMP/unmapped.now" | sed -n 's/^> /  + /p; s/^< /  - /p' >&2
			[ $KEEP = 1 ] || die "unmapped globals changed: add table rows or keep-list entries (or -k)"
		fi ;;
	selfcopy)
		mkdir -p patches scripts
		rm -rf patches/aws; cp -R "$PD" patches/aws
		cp "$SRC/scripts/rebuild-aws.sh" scripts/rebuild-aws.sh
		# record the base, for the export guard of the next rebuild
		if grep -q '^upstream-base' "$PD/series"; then
			sed "s/^upstream-base.*/upstream-base $UPSHA/" "$PD/series"
		else
			awk -v b="upstream-base $UPSHA" '{ print } /^patches-commit/ { print b }' "$PD/series"
		fi > patches/aws/series
		git add patches/aws scripts/rebuild-aws.sh
		git commit -q -m "rebuild: add scripts/rebuild-aws.sh and patches/aws" ;;
	*)	die "unknown step '$op'" ;;
	esac
done 3< "$TMP/steps"

STEP=final
makecheck
cmp -s "$TMP/mk0.bin" "$TMP/mk.bin" || die "final make binary differs from the one at the ref step"
say "  make binary unchanged since the ref step"
refbuild "$TMP/cur.bin"; cmp -s "$TMP/ref.bin" "$TMP/cur.bin" || die "final binary differs from the reference"
say "done: $BR at $(git rev-parse --short HEAD) in $DIR"
say "reference binary: $DIR.ref.bin"
if [ $TAG = 1 ]; then
	old=$(echo "${base:-}" | cut -c1-8)	# the guard's base of the old AWS
	say "To keep the old AWS and move AWS to this branch (not run):"
	say "  git tag AWS-$old $AWS && git push origin AWS-$old"
	say "  git push --force-with-lease=AWS origin $BR:AWS"
fi
