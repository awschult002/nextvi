# Comment conventions (Doxygen)

Keep comments short. Explain *why* and *what the pointers point at*; don't restate the code.

## File header (top of every .c/.h)
```c
/**
 * @file lbuf.c
 * @brief Line buffer: stores the file as an array of lines, with undo marks.
 */
```

## Functions
```c
/**
 * @brief Insert text at a row.
 * @param lb   target line buffer
 * @param row  row index to insert before (0-based)
 * @param s    NUL-terminated text, may contain newlines
 * @return 0 on success, nonzero on failure
 */
```
Static helpers get a one-line `/** @brief ... */` when the name alone isn't enough.

## Globals and struct fields
Use a trailing `///<` so the comment stays on the same line:
```c
int cursor_row;        ///< current cursor row (was xrow)
```

## Pointer arithmetic
Don't rewrite it. Put a plain `/* */` note above tricky lines saying what each pointer marks, e.g.
`/* s: start of current word, r: one past its end */`.

## Rename trail
Every renamed symbol is listed in `docs/RENAMES.md` (old name, new name, file).
