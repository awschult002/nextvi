#!/bin/sh
# List file-scope globals (extern, static, __thread, plain definitions) in
# every *.c and *.h that are neither a new name in any scripts/renames-*.tsv
# table nor listed in scripts/renames-keep.txt (names deliberately left as
# they are). Functions, prototypes, typedefs, struct members, enum constants
# and file-scope macro invocations such as EO(...) are skipped. Vendored
# third-party code (cJSON.*, jsmn.h) is not scanned.
# After an upstream merge, anything printed here is a global upstream added:
# give it a table row or keep it. Exit status 1 if anything is unmapped.
cd "$(dirname "$0")/.."
files=$(ls *.c *.h | grep -v '^cJSON\.' | grep -v '^jsmn\.h$')
perl -e '
use strict; use warnings;
local $/;
for my $f (@ARGV) {
	open my $fh, '\''<'\'', $f or die "$f: $!"; my $s = <$fh>; close $fh;
	# comments and literals in one pass, so "//" in a string is not a comment
	$s =~ s{(/\*.*?\*/)|//[^\n]*|("(?:\\.|[^"\\\n])*")|('\''(?:\\.|[^'\''\\\n])*'\'')}{defined $1 ? '\'' '\'' : defined $2 ? '\''""'\'' : defined $3 ? "'\'''\''" : '\'''\''}gse;
	$s =~ s{^[ \t]*#(?:[^\n]*\\\n)*[^\n]*}{}mg;	# preprocessor lines
	# collapse every brace block to {} so only file-scope text remains
	1 while $s =~ s/\{[^{}]*\}/\x01/g;
	$s =~ s/\x01/{}/g;
	# drop file-scope macro invocations: UPPER(...) not followed by '\'';'\''
	1 while $s =~ s/(^|[;}\n])\s*_?[A-Z][A-Z0-9_]*\s*(\((?:[^()]++|(?2))*\))(?!\s*;)/$1\n/g;
	for my $st (split /;/, $s) {
		$st =~ s/^\s+|\s+$//g;
		$st =~ s/\s+/ /g;
		next if $st eq '\'''\'';
		$st =~ s/^(?:\}\s*)+//;		# function bodies end without '\'';'\''
		# a function definition before this statement: keep what follows its body
		$st =~ s/.*\)\s*\{\}\s*//;
		next if $st eq '\'''\'' || $st =~ /^typedef\b/;
		# split declarators at top-level commas, dropping initializers
		my ($d, @decl) = (0, '\'''\'');
		for my $c (split //, $st) {
			$d++ if $c =~ /[(\[{]/; $d-- if $c =~ /[)\]}]/;
			if ($c eq '\'','\'' && !$d) { push @decl, '\'''\''; next }
			$decl[-1] .= $c;
		}
		my $first = 1;
		for my $x (@decl) {
			$x =~ s/=.*//s;
			if ($first) {
				$x =~ s/^(?:(?:extern|static|const|volatile|register|__thread|_Thread_local|inline)\s+)*//;
				# the type: struct/union/enum with optional tag and body, or one word
				$x =~ s/^(?:(?:struct|union|enum)\s*\w*\s*(?:\{\})?|(?:(?:unsigned|signed|long|short)\b\s*)+(?:(?:int|char|double)\b)?|\w+)\s*//
					or next;
				$x =~ s/^(?:(?:const|volatile)\s+)*//;
				$first = 0;
			}
			next if $x =~ /^\s*\**\s*\w+\s*\(/;		# prototype
			my ($n) = $x =~ /^\s*[\s*]*\(\s*\*\s*(\w+)\s*\)/;	# function pointer
			($n) = $x =~ /^[\s*]*(?:const\s+)?(\w+)/ unless defined $n;
			print "$n\t$f\n" if defined $n && $n !~ /^(?:const|volatile)$/;
		}
	}
}
' $files | sort -u > /tmp/unmapped.$$
{ cut -f2 scripts/renames-*.tsv; grep -v '^#' scripts/renames-keep.txt 2>/dev/null; } | sort -u > /tmp/mapped.$$
out=$(cut -f1 /tmp/unmapped.$$ | sort -u | comm -23 - /tmp/mapped.$$)
if [ -z "$out" ]; then
	echo "all file-scope globals mapped or kept"
	rm -f /tmp/unmapped.$$ /tmp/mapped.$$
	exit 0
fi
echo "file-scope globals with no rename row and not in renames-keep.txt:"
for n in $out; do
	printf '  %s (%s)\n' "$n" "$(awk -F'\t' -v n="$n" '$1 == n { printf "%s%s", s, $2; s = " " }' /tmp/unmapped.$$)"
done
rm -f /tmp/unmapped.$$ /tmp/mapped.$$
exit 1
