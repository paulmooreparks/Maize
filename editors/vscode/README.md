# Maize Assembly for VS Code

Syntax highlighting and a language server for Maize assembly (`.mazm`) source files, the input language of the `mazm` assembler.

## Features

- TextMate grammar covering the full `mazm` token surface: `;` comments, `"..."` strings with escapes, the six directives (`ADDRESS`, `LABEL`, `DATA`, `STRING`, `INCLUDE`, `CODE`), the assembler's full instruction-mnemonic set (case-insensitive, as the assembler accepts them), registers `R0`-`R9`, `RT`, `RV`, `RF`, `RB`, `RP`, `RS` and their aliases `SP`, `BP`, `PC`, `FL` with subregister suffixes (`.B0`-`.B7`, `.Q0`-`.Q3`, `.H0`, `.H1`, `.W0`, `.W`), `$`/`#`/`%` numeric literals with `` ` ``/`_`/`,` digit separators, label declarations, and `@` address operands.
- Language configuration: toggle-comment inserts `;`, quotes auto-close, and dotted register names like `R0.B0` select as a single word.
- Language server:
  - **Diagnostics from the assembler itself, live as you type.** The extension pipes your buffer through `mazm --check --stdin` (debounced ~300 ms) and surfaces the assembler's error as a squiggle, unsaved edits included. There is no second parser guessing at validity; if mazm rejects it, it's an error, and if mazm accepts it, it's clean. INCLUDE targets resolve against the file's real directory (`--base-path`). With an older mazm build that lacks `--stdin`, the extension detects this at startup and falls back to checking the saved file on open/save.
  - **Document symbols** for label declarations (`name:` and `LABEL name` forms).
  - **Go to definition** for labels, following `INCLUDE` chains.
  - **Find references** for labels within the current file.

The diagnostics format is mazm's existing fatal line (`mazm: <file>:<line>: error: <msg>`); `--check` runs the full assembly pipeline with no filesystem effects, and `--stdin --base-path <dir> --source-name <path>` checks a piped buffer the same way. This is the intended v1 contract, not a placeholder: a structured format only becomes worthwhile if mazm ever gains multi-error recovery.

## Setup

Point the extension at your built assembler if `mazm` is not on PATH:

```json
"maize.mazm.path": "c:/path/to/Maize/build/windows-llvm-mingw-debug/mazm.exe"
```

Without a working `mazm`, highlighting, symbols, definition, and references still work; only diagnostics are disabled (the extension warns once).

## Installing locally

The extension is not published to a marketplace. Two local options:

- In VS Code, run the command "Developer: Install Extension from Location..." and select this folder (`editors/vscode`). Run `npm install` in this folder first so the language client/server dependencies are present.
- Or package a `.vsix` with `npx @vscode/vsce package` in this folder and install it via "Extensions: Install from VSIX...".

## Development

- The grammar lives in `syntaxes/mazm.tmLanguage.json`. Scope assertions:
  `npx vscode-tmgrammar-test -g syntaxes/mazm.tmLanguage.json "tests/*.test.mazm"`
- The language server lives in `server/server.js` (plain JavaScript, no build step); the client shim in `client/extension.js`. Server tests (unit + a scripted stdio LSP session):
  `MAZM_PATH=/path/to/mazm node tests/lsp.test.js`

The grammar and the server's symbol index both mirror the tokenizer in `src/mazm.cpp`; when the assembler's token surface changes, change them with it. Anything semantic belongs in mazm, reached via `--check`.
