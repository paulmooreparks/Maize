# Maize for VS Code

Syntax highlighting and a language server for Maize v2 assembly (`.mzasm`) source files, the input language of the `mzasm` assembler.

## Features

- A TextMate grammar covering the v2 token surface: `;` comments, `"..."` strings and `'x'` character literals with the eight escapes the specification allows, the fifteen directives (`section`, `origin`, `align`, the four `data_*` width directives, `data_string`, `data_string_zero`, `data_fill`, `reserve`, `constant`, `global`, `extern`, `include`), the four section kinds, the full instruction-mnemonic set, registers `r0` through `r31` with all thirty-two ABI aliases and their `.b`/`.q`/`.h` slice suffixes, `$`/`#`/`%` numeric literals with back-tick and comma digit separators, label declarations, and `@` memory operands.
- Case matters. Mnemonics, directive names and register names are lowercase in v2, so `MOVE` and `Load` highlight as the mistakes they are rather than as instructions.
- Every numeric literal carries a base marker, and the grammar holds to that: an unmarked number such as `10` is left unstyled rather than painted as a number whose base a reader would have to guess.
- Language configuration: toggle-comment inserts `;`, quotes and parentheses auto-close, and a register slice like `r0.b0` selects as a single word.
- A language server providing:
  - **Diagnostics from the assembler itself, live as you type.** The extension pipes your buffer through `mzasm --check --stdin` (debounced about 300 ms) and surfaces the assembler's errors as squiggles, unsaved edits included. There is no second parser guessing at validity. `include` targets resolve against the file's real directory. Against a build that lacks `--stdin`, the extension detects that at startup and falls back to checking the saved file on open and save.
  - **Document symbols** for label declarations.
  - **Go to definition** for labels, following `include` chains.
  - **Find references** for labels within the current file.

The diagnostic format is `mzasm`'s fatal line, `mzasm: <file>:<line>: error: <msg>`, one line per error. The assembler recovers past each error and reports everything it finds in a single run, so several squiggles appear at once and the count drops as you fix them. `--check` runs the full assembly pipeline with no filesystem effects.

## Setup

The `maize.mzasm.path` setting names the assembler, and it defaults to the bare name `mzasm`. On any machine that has run `scripts/install-mzasm.ps1` or `scripts/install-mzasm.sh`, that default resolves with no configuration at all, because those scripts install `mzasm` alongside `mzvm`. Point it at a build tree if you want a specific binary:

```json
"maize.mzasm.path": "c:/path/to/Maize/build/windows-llvm-mingw-debug/mzasm.exe"
```

Without a working `mzasm`, highlighting, symbols, definition and references all still work, and only diagnostics are disabled. The extension warns once when it cannot find one.

## Installing locally

The extension is not published to a marketplace. There are two local options:

- In VS Code, run the command "Developer: Install Extension from Location..." and select this folder (`editors/vscode`). Run `npm install` here first so the language client and server dependencies are present.
- Or package a `.vsix` with `npx @vscode/vsce package` in this folder and install it through "Extensions: Install from VSIX...".

## Development

Run both suites with `npm test`, or each on its own:

- `node tests/grammar.test.js` checks the grammar. It re-derives the mnemonic table from `docs/spec-v2/appendix-a-opcode-map.md` and fails naming the spellings that differ, then tokenizes through `vscode-textmate` and asserts scope names.
- `MZASM_PATH=/path/to/mzasm node tests/lsp.test.js` checks the server, as unit tests over the pure helpers plus a scripted stdio LSP session.

`syntaxes/mzasm.tmLanguage.json` is generated, so edit `tests/generate-grammar.js` and run `npm run generate-grammar` rather than editing the JSON. The mnemonic list in it comes from the specification's opcode-map appendix and never from a transcription, and the test run fails if the checked-in file and the appendix disagree. That is deliberate: a hand-copied list of 150 spellings drifts silently, and this one cannot.

The grammar and the server's symbol index both mirror the tokenizer in `src/v2/mzasm_lexer.cpp`, and the authority for both is `docs/spec-v2/assembler.md`. When the language changes, change them with it. Anything semantic belongs in `mzasm` itself, reached through `--check`.
