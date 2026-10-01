# Embed the rendered README without indentation or example prompt colons.
function quote(s,    i, c, out) {
	out = "\""
	for (i = 1; i <= length(s); i++) {
		c = substr(s, i, 1)
		if (c == "\\" || c == "\"" || c == "?")
			out = out "\\"
		out = out c
	}
	return out "\""
}
function entry(end,    i) {
	if (!count)
		return
	while (end > begin && lines[end - 1] == "")
		end--
	for (i = 1; i <= count; i++)
		if (!seen[names[i]]++)
			records = records "\t{" quote(names[i]) ", " quote(desc) ", " begin ", " end ", " (section == "EX OPTIONS" ? 1 : 0) ", 0},\n"
	count = 0
	desc = ""
}
/^EX PARSING$/ { active = 1 }
/^EXINIT ENV VAR$/ { active = 0 }
/^REGEX$/ { active = 1; regex = 1; $0 = "EX REGEX" }
/^SPECIAL MARKS$/ { if (regex) active = 0 }
active { lines[n++] = $0 }
END {
	print "/* Generated from README by exspec.awk. */"
	print "static char *exspec_lines[] = {"
	for (i = 0; i < n; i++) {
		s = lines[i]
		if (s ~ /^ +:/)
			sub(/:/, "", s)
		if (s ~ /Evaluates to/)
			gsub(/":/, "\"", s)
		sub(/^[ \t]+/, "", s)
		print "\t" quote(s) ","
	}
	print "};"
	for (i = 0; i < n; i++) {
		s = lines[i]
		if (s ~ /^EX /) {
			entry(i)
			section = s
			continue
		}
		if (section != "EX COMMANDS" && section != "EX OPTIONS")
			continue
		if (section == "EX OPTIONS" && !count && s !~ /^     [^ ]+\[/)
			continue
		if (s ~ /^     [^ ]/) {
			if (desc != "")
				entry(i)
			if (!count)
				begin = i
			s = substr(s, 6)
			inline_desc = ""
			if (match(s, /  +/)) {
				inline_desc = substr(s, RSTART + RLENGTH)
				s = substr(s, 1, RSTART - 1)
			}
			sub(/^\[[^]]*\]/, "", s)
			sub(/^\{[^}]*\}/, "", s)
			sub(/[\[{ ].*$/, "", s)
			names[++count] = s
			desc = inline_desc
		} else if (count && desc == "" && s ~ /[^ ]/) {
			sub(/^ +/, "", s)
			desc = s
		}
	}
	entry(n)
	print "static struct {"
	print "\tchar *name, *desc;"
	print "\tint begin, end, option, read;"
	print "} exspec_cmds[] = {"
	printf "%s", records
	print "};"
}
