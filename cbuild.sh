#!/bin/sh

POSIXLY_CORRECT=1
cbuild_OPWD="$PWD"
BASE="${0%/*}" && [ "$BASE" = "$0" ] && BASE="." # -> BASE="$(realpath "$0")" && BASE="${BASE%/*}"
cd "$BASE" || log "$R" "Unable to change directory to ${BASE##*/}. Re-execute using a POSIX shell and check again."
BASE="${PWD%/}"
trap 'cd "$cbuild_OPWD"' EXIT

# Color escape sequences
G="\033[32m" #     Green
R="\033[31m" #     Red
B="\033[34m" #     Blue
NC="\033[m"  #     Unset

log() {
    # shellcheck disable=SC2059 # Using %s with ANSII escape sequences is not possible
    printf "${1}->$NC "
    shift
    printf "%s\n" "$*"
}

require() {
    set -- $1
    command -v "$1" >/dev/null 2>&1 || {
        log "$R" "[$1] is not installed. Please ensure the command is available [$1] and try again."
        exit 1
    }
}

run() {
    log "$B" "$*"
    # shellcheck disable=SC2068 # We want to split elements, but avoid whitespace problems (`$*`), and also avoid `eval $*`
    $@
}

: "${CC:=cc}"
: "${STRIP:=strip}"
: "${PREFIX:=/usr/local}"
: "${OS:=$(uname)}"
: "${CFLAGS:=-O2}"

CFLAGS="\
-pedantic -Wall -Wextra \
-Wno-implicit-fallthrough \
-pthread \
-Wno-missing-field-initializers \
-Wno-unused-parameter \
-Wno-unused-result \
-Wfatal-errors -std=c99 \
-lpthread \
$CFLAGS"

case "$OS" in
*_NT*) CFLAGS="$CFLAGS -D_POSIX_C_SOURCE=200809L" ;;
*Darwin*) CFLAGS="$CFLAGS -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE" ;;
*Linux*) CFLAGS="$CFLAGS -D_POSIX_C_SOURCE=200809L" ;;
*) CFLAGS="$CFLAGS -D_DEFAULT_SOURCE" ;;
esac

# Fetch once; compile the runtime and generated C parser into vi.
ts_fetch() (
    ts_dest=".treesitter/$1-$3"
    require git
    # checkout $1 must be exactly commit $2: HEAD matches and no file differs
    ts_ok() {
        [ "$(git -C "$1" rev-parse HEAD 2>/dev/null)" = "$2" ] &&
        [ -z "$(git -C "$1" status --porcelain --untracked-files=all)" ]
    }
    if [ -d "$ts_dest" ]; then
        ts_ok "$ts_dest" "$3" && exit 0
        log "$R" "$ts_dest does not match $1 $2 ($3); remove it to refetch"
        exit 1
    fi
    mkdir -p .treesitter || exit 1
    ts_tmp="$ts_dest.tmp.$$"
    trap 'rm -rf "$ts_tmp"' EXIT
    trap 'exit 1' HUP INT TERM
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$2" "https://github.com/tree-sitter/$1" "$ts_tmp" || exit 1
    ts_ok "$ts_tmp" "$3" || {
        log "$R" "Unexpected revision for $1 $2"
        exit 1
    }
    mv "$ts_tmp" "$ts_dest" || exit 1
)

ts_setup() {
    ts_fetch tree-sitter v0.25.10 da6fe9beb4f7f67beb75914ca8e0d48ae48d6406 || exit 1
    ts_fetch tree-sitter-c v0.24.1 7fa1be1b694b6e763686793d97da01f36a0e5c12 || exit 1
    ts_runtime=.treesitter/tree-sitter-da6fe9beb4f7f67beb75914ca8e0d48ae48d6406/lib
    ts_grammar=.treesitter/tree-sitter-c-7fa1be1b694b6e763686793d97da01f36a0e5c12/src
    TS_SOURCES="$ts_runtime/src/lib.c $ts_grammar/parser.c"
    TS_CFLAGS="-D_DEFAULT_SOURCE -I$ts_runtime/include -I$ts_runtime/src -I$ts_grammar"
}

build() {
    require "${CC}"
    ts_setup
    log "$G" "Entering step: \"Build \"${BASE##*/}\" using \"$CC\"\""
    run "$CC vi.c $TS_SOURCES $TS_CFLAGS -o vi $CFLAGS" || {
        log "$R" "Failed during step: \"Build \"${BASE##*/}\" using \"$CC\""
        exit 1
    }
}

spec() {
    require "awk"
    # Additional ex specs are injected here so README stays pristine on master.
    # Keep this block in sync with the agent command table in ex.c.
    tmp="$(mktemp)"
    awk '
        function spec(name, desc, body) {
            print "     " name
            print "             " desc
            print ""
            gsub(/\n/, "\n             ", body)
            print "             " body
            print ""
        }
        /^     ac\[regex\]$/ && !done {
            spec("exspec[command range topic]", "Print ex command catalog or specification",
                "No argument lists useful commands for agents, the full catalog for\n" \
                "humans. Use catalog for all commands and options; use a command name\n" \
                "for its specification, including hidden commands. Topics: parsing,\n" \
                "escapes, expansion, ranges, regex, commands, options.\n\n" \
                "Example: list all commands\n:exspec catalog")
            spec("[range]a[text]", "Open or resume the agent conversation",
                "Keeps the conversation. Text prefills the prompt; range attaches\n" \
                "buffer text to the next submission. Enter adds a newline; Escape\n" \
                "submits; Ctrl-C exits; Ctrl-O opens the editor. Ex specials are\n" \
                "disabled. Unavailable as an agent tool.")
            spec("[range]a![text]", "Start a new agent conversation",
                "Clears history and log, aspec tracking and deferred command.\n" \
                "Range, text and prompt controls work as for a. Unavailable as an agent tool.")
            spec("[range]a~[text]", "Resume an agent conversation from its log",
                "Loads b-4 as context, including edits. Keeps aspec tracking.\n" \
                "Range, text and prompt controls work as for a. Unavailable as an agent tool.")
            spec("[range]apack[text]", "Compact the agent session from its log",
                "Loads b-4 as context and asks the agent to replace it with a summary.\n" \
                "Stays at the prompt; reloads the log on exit. Text replaces the default\n" \
                "instructions; range attaches buffer text. Unavailable as an agent tool.")
            spec("[range]apack![text]", "Compact the agent session by browsing its log",
                "Starts fresh without loading or clearing b-4. The agent reads bounded\n" \
                "ranges and replaces the log with a summary. Stays at the prompt;\n" \
                "reloads on exit. Resets aspec tracking. Text and range work as for\n" \
                "apack. Unavailable as an agent tool.")
            spec("acm", "Toggle the caveman response style skill",
                "Adds or removes the skill in b-5. Tool calls update the system\n" \
                "message; otherwise context is rebuilt from the log.")
            print "     aretry"
            print "             Execute the last deferred agent command once"
            print ""
            print "             Uses the saved range and expanded argument. Takes no range or"
            print "             argument. A new deferral replaces it; retry consumes it even on"
            print "             failure. A new session clears it. Errors if none is saved."
            print ""
            print "             Example: execute a deferred command"
            print "             :aretry"
            print ""
            spec("ast", "Print agent status and token usage",
                "Prints sizes, per-role usage, activity, limits and autocompact mode.\n" \
                "Usage counts come from the last accepted response. Next input is\n" \
                "estimated from reported input plus new JSON bytes / 3, or all JSON\n" \
                "bytes / 3 without a usable count. Includes tool definitions and\n" \
                "framing; not a tokenizer or a guarantee the request fits.")
            done = 1
        }
        /^     ai\[1\]/ && !aspec_done {
            spec("aco[0]  Automatically compact using the loaded session log",
                "Positive argument sets an estimated input-token threshold; 0 or\n" \
                "negative disables. No argument enables at 85000 or toggles off.\n" \
                "aco and aco! share a threshold; the last setting wins.",
                "After tool batches, runs apack without a prompt and resumes. Never\n" \
                "recurses. Failed, cancelled or inadequate summaries keep the old\n" \
                "history and stop the run. Leave room for instructions and output;\n" \
                "use aco! if the log cannot fit. Does not change the API limit.")
            spec("aco![0]  Automatically compact by browsing the session log",
                "Threshold and toggle work as for aco (default 85000). Selecting\n" \
                "either mode replaces the other.",
                "Runs apack! with fresh context and bounded reads of b-4. Keeps the\n" \
                "current task and restores the editor buffer. Use ast for status\n" \
                "and token estimates.")
            spec("ar[0]  Display returned agent reasoning",
                "No argument logically inverts the option.",
                "Nonzero includes returned reasoning in the session log.")
            spec("gr[2]  Control agent output protection",
                "No argument logically inverts the option.",
                "Value 2 limits tool output to 4096 bytes and protects captured shell\n" \
                "output. Other values disable protection; 0 and 1 increment after\n" \
                "each tool call until 2. Negative values stay disabled.")
            print "     aspec[1]  Print ex specifications for agents"
            print ""
            print "             No argument logically inverts the option. 0 disables automatic"
            print "             specifications; 1 enables them (the default)."
            print ""
            aspec_done = 1
        }
        { print }
    ' README > "$tmp" &&
    awk -f exspec.awk "$tmp" > exspec.h
    rm -f "$tmp"
}

install() {
    run rm -f "$DESTDIR$PREFIX/bin/vi" 2> /dev/null
    command -v "$STRIP" >/dev/null 2>&1 && run "$STRIP" vi
    run mkdir -p "$DESTDIR$PREFIX/bin/" &&
    run cp -f vi "$DESTDIR$PREFIX/bin/vi" &&
    [ -x "$DESTDIR$PREFIX/bin/vi" ] && log "$G" "\"${BASE##*/}\" has been installed to $DESTDIR$PREFIX/bin/vi" || log "$R" "Couldn't finish installation"
}

print_usage() {
    echo "Usage: $0 {install|pgobuild|build|debug|fetch|clean|retrieve|bench|spec}"
    echo "Options may be shortened to a prefix"
    exit "$1"
}

# Argument processing
while [ $# -gt 0 ] || [ "$1" = "" ]; do
    case "$1" in
    s*)
        spec && exit 0 || exit 1
        ;;
    i*)
        shift
        [ -x ./vi ] && install && exit 0 || build && install && exit 0
        ;;
    d*)
        shift
        if command -v scan-build >/dev/null 2>&1; then
                CC="scan-build $CC"
        fi
        CFLAGS="$CFLAGS -O0 -g -fsanitize=address -fsanitize=undefined"
        log "$G" "Entering step: \"Append \"\$CFLAGS\" with debugging flags\""
        set -- build "$@"
        ;;
    "" | b | bu*)
        # If the user doesn't use "build" explicitly, do not run the build step again.
        [ -n "$1" ] && explicit="1"
        if [ "$explicit" != "1" ]; then
            if [ -f ./vi ] || [ -f ./nextvi ]; then
                log "$R" "Nothing to do; \"${BASE##*/}\" was already compiled"
                print_usage 0
            fi
        fi
        # Start build process
        build && exit 0 || exit 1
        ;;
    p*)
        shift
        pgobuild() {
            ccversion="$($CC --version)"
            case "$ccversion" in *clang*) clang=1 ;; esac
            if [ "$clang" = 1 ] && [ -z "$PROFDATA" ]; then
                if command -v llvm-profdata >/dev/null 2>&1; then
                    PROFDATA=llvm-profdata
                elif xcrun -f llvm-profdata >/dev/null 2>&1; then
                    PROFDATA="xcrun llvm-profdata"
                fi
                [ -z "$PROFDATA" ] && log "$R" "pgobuild with clang requires llvm-profdata" && exit 1
            fi
            run "$CC vi.c $TS_SOURCES $TS_CFLAGS -fprofile-generate=. -o vi -O2 $CFLAGS" || return 1
            EXINIT="$(printf '%b' '&dw100.1\\\\:/not matching:&:b0:&100J0300liinsert:&ewbgw:q!')"
            export EXINIT && ./vi ./vi.c > /dev/null
            [ "$clang" = 1 ] && run "$PROFDATA" merge ./*.profraw -o default.profdata
            run "$CC vi.c $TS_SOURCES $TS_CFLAGS -fprofile-use=. -o vi -O2 $CFLAGS" || return 1
            rm -f ./*.gcda ./*.profraw ./default.profdata
        }
        require "${CC}"
        ts_setup
        log "$G" "Entering step: \"Build \"${BASE##*/}\" using \"$CC\" and PGO\""
        pgobuild || {
            log "$R" "Failed during step: \"Build \"${BASE##*/}\" using \"$CC\" and PGO\""
            exit 1
        } && exit 0 || exit 1
        ;;
    c*)
        shift
        run rm -f vi nextvi callgrind.out.* cachegrind.out.* 2>/dev/null
        exit 0
        ;;
    r*)
        shift
        if [ -x ./vi ]; then
            [ ! -e ./nextvi ] && mv ./vi ./nextvi
        else
            log "$R" "\"${BASE##*/}\" was never compiled OR it was but its binaries weren't found anyways." ; exit 1
        fi
        readlink -f ./nextvi && exit 0
        ;;
    f*)
        shift
        ! git diff --quiet HEAD && {
          log "$R" "Please stash changes before fetching."
          exit 1
        }
        git switch -c upstream-temp
        git pull https://github.com/kyx0r/nextvi
        git switch master
        git rebase --rebase-merges upstream-temp
        git branch -D upstream-temp
        log "$G" "Successfully fetched from upstream."
        ;;
    be*)
        shift
        export EXINIT="${EXINIT}:&dw1999.1Zx"
        valgrind --tool=callgrind ./vi vi.c
        valgrind --tool=cachegrind --cache-sim=yes --branch-sim=yes ./vi vi.c
        exit 0
        ;;
    *)
        print_usage 1
        ;;
    esac
done
