# Maize Assembly for VS Code

Syntax highlighting for Maize assembly (`.mazm`) source files, the input language of the `mazm` assembler.

## Features

- TextMate grammar covering the full `mazm` token surface: `;` comments, `"..."` strings with escapes, the six directives (`ADDRESS`, `LABEL`, `DATA`, `STRING`, `INCLUDE`, `CODE`), all 57 instruction mnemonics (case-insensitive, as the assembler accepts them), registers `R0`-`R9`, `RT`, `RV`, `RF`, `RB`, `RP`, `RS` and their aliases `SP`, `BP`, `PC`, `FL` with subregister suffixes (`.B0`-`.B7`, `.Q0`-`.Q3`, `.H0`, `.H1`, `.W0`, `.W`), `$`/`#`/`%` numeric literals with `` ` ``/`_`/`,` digit separators, label declarations, and `@` address operands.
- Language configuration: toggle-comment inserts `;`, quotes auto-close, and dotted register names like `R0.B0` select as a single word.

The extension is declarative: no activation code runs.

## Installing locally

The extension is not published to a marketplace. Two local options:

- In VS Code, run the command "Developer: Install Extension from Location..." and select this folder (`editors/vscode`).
- Or package a `.vsix` with `npx @vscode/vsce package` in this folder and install it via "Extensions: Install from VSIX...".

## Development

The grammar lives in `syntaxes/mazm.tmLanguage.json`. Scope assertions live in `tests/`; run them with:

```
npx vscode-tmgrammar-test -g syntaxes/mazm.tmLanguage.json "tests/*.test.mazm"
```

The grammar mirrors the tokenizer in `src/mazm.cpp`; when the assembler's token surface changes, change the grammar with it.
