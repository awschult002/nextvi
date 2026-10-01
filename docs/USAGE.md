# Using nextvi

Short how-tos for features that are easy to miss in the main `README` key table.

## Building

```sh
make          # builds ./vi (first run fetches the pinned tree-sitter sources)
make ts       # fetch tree-sitter only
make clean
```

`make` gives the same binary as `./cbuild.sh build`. `cbuild.sh` stays in the tree only because the patch scripts edit it, and `rebuild-aws.sh` stops if its flags and the Makefile's disagree. Use `make`. Don't edit `README`: `exspec.h`, the built-in help, is generated from it.

## Line numbers

Line numbers are controlled by a count typed before `#`. Each count turns one display mode on or off, and modes can be combined.

| Keys | What you get | Stays on? |
|---|---|---|
| `#` | Absolute numbers plus a relative number just before the first non-blank character | **No.** It clears on the next keypress |
| `2#` | Absolute numbers in the left column | Yes, until you type `2#` again |
| `4#` | Relative number just before the first non-blank character (it moves with the indent) | Yes, until you type `4#` again |
| `8#` | Relative numbers in the left column | Yes, until you type `8#` again |
| `10#` | Absolute and relative numbers in the left column (2 + 8) | Yes, until you type `10#` again |

How the count works: the count is a set of bits (2 = absolute, 4 = relative at the indent, 8 = relative in the left column), so you add them to combine modes. If *any* of those bits is already on, typing the count turns them off. Otherwise it turns them all on. For example, `10#` while `8#` is active turns off both the absolute and relative modes. A bare `#` with any mode already on turns every mode off.

For code with indentation, use `8#` or `10#`. The numbers stay in the left column instead of moving with the indent.

### Turn numbers on at startup

nextvi runs the ex commands in the `EXINIT` environment variable at startup, and the `&` ex command runs a string of vi keys. Add this to your shell rc:

```sh
export EXINIT='& 8#'    # relative numbers in the left column
# export EXINIT='& 10#' # absolute + relative
```

There is no runtime key remapping: key bindings are compiled into the big switch in `vi.c`. `EXINIT` is the way to change startup behavior without patching source.

For developers: the modes are the `LN_*` flags on `lnum_mode` in `vi.c`, drawn by `vi_drawrow`.
