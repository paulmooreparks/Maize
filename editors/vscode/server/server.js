// Maize v2 assembly language server (maize-46, rewritten for v2 by maize-429).
//
// Diagnostics come from the assembler itself: `mzasm --check <file>` is spawned
// against the saved file and its fatal lines (mzasm: <file>:<line>: error:
// <msg>) become the diagnostics. The symbol index below is a line-scan
// projection of mzasm's tokenizer, the same stance as the TextMate grammar;
// anything semantic stays mzasm's job.
//
// The transport and session layer here is v1's unchanged: document lifecycle,
// the debounce and generation guard, the live/file probe, and the spawn
// timeout encode nothing about either language. What v2 replaced is the
// language knowledge, and the differences that mattered are these. mzasm's
// diagnostic prefix is its own name, hardcoded in the formatter
// (src/v2/mzasm_lexer.cpp), so it is `mzasm: ` and not the configured path.
// v2's `include` is lowercase, because case matters everywhere in v2. And v2
// has no LABEL directive at all, so the colon form is the only label
// declaration there is and the directive-shaped symbol kind went with it
// rather than being translated into something v2 does not have.

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const { pathToFileURL, fileURLToPath } = require('url');

const ERROR_LINE = /^mzasm: (.+?):(\d+): error: (.+)$/m;
const PROBE_SOURCE_NAME = 'mzasm-stdin-probe';

// An unrecognized mnemonic, which fails for a reason that rests on no lexical
// construct at all and so cannot stop failing as the language settles. This is
// the same probe scripts/install-mzasm.ps1 and .sh already use for their own
// installed-mzasm smoke test.
const PROBE_INPUT = 'no_such_instruction\n';
const PROBE_MARKER = /^mzasm: mzasm-stdin-probe:1: error:/m;

const IDENT = /[A-Za-z_][A-Za-z0-9_]*/g;
const COLON_LABEL = /^(\s*)([A-Za-z_][A-Za-z0-9_]*):/;
const INCLUDE_LINE = /^\s*include[ \t]+"([^"]+)"/;

/* Parse mzasm's first fatal diagnostic line out of a stderr blob. The `note:
   included from here` continuation lines mzasm emits after an error inside an
   included file are deliberately not matched: only `error:` lines carry a
   diagnostic, and the notes repeat a location the message already names. */
function parseMzasmError(stderr) {
    const m = ERROR_LINE.exec(stderr || '');

    if (!m) {
        return null;
    }

    return { file: m[1], line: parseInt(m[2], 10), message: m[3] };
}

/* All fatal diagnostic lines, in output order (maize-50: the assembler reports
   every error it can recover past, one line each). */
function parseMzasmErrors(stderr) {
    const out = [];
    const re = /^mzasm: (.+?):(\d+): error: (.+)$/gm;
    let m;

    while ((m = re.exec(stderr || '')) !== null) {
        out.push({ file: m[1], line: parseInt(m[2], 10), message: m[3] });
    }

    return out;
}

/* Blank out the comment tail, string bodies and character-literal bodies of a
   source line, preserving columns, so identifier scans cannot match inside any
   of them.

   The character-literal case is handled symmetrically with the string case
   rather than left out. A v2 character literal holds one character and cannot
   itself look like an identifier, so on its own it is low risk; the reason to
   handle it is that a semicolon inside one is an ordinary character
   (assembler.md, Lexical structure), so an unhandled `';'` would blank the
   rest of a real line as though a comment had started there. */
function maskLine(line) {
    let out = '';
    let inString = false;
    let inChar = false;
    let escaped = false;

    for (let i = 0; i < line.length; i++) {
        const c = line[i];

        if (inString || inChar) {
            out += ' ';

            if (escaped) {
                escaped = false;
            }
            else if (c === '\\') {
                escaped = true;
            }
            else if (inString && c === '"') {
                inString = false;
            }
            else if (inChar && c === "'") {
                inChar = false;
            }
        }
        else if (c === '"') {
            out += ' ';
            inString = true;
        }
        else if (c === "'") {
            out += ' ';
            inChar = true;
        }
        else if (c === ';') {
            out += ' '.repeat(line.length - i);
            break;
        }
        else {
            out += c;
        }
    }

    return out;
}

/* Index a document: label declarations and `include` targets. Line numbers are
   0-based.

   v2 declares a label one way only, as an identifier followed by a colon on a
   line of its own (assembler.md, Symbols). v1's `LABEL <name>` directive form
   has no v2 equivalent, so it is gone from here rather than translated, and
   with it the second symbol kind the document-symbol handler used to carry. */
function indexText(text) {
    const labels = [];
    const includes = [];
    const lines = text.split(/\r\n|\r|\n/);

    for (let i = 0; i < lines.length; i++) {
        const masked = maskLine(lines[i]);

        const colon = COLON_LABEL.exec(masked);
        if (colon) {
            labels.push({
                name: colon[2],
                line: i,
                startChar: colon[1].length,
                endChar: colon[1].length + colon[2].length,
            });
        }

        // `include` targets live inside the string, so scan the raw line.
        const inc = INCLUDE_LINE.exec(lines[i]);
        if (inc) {
            includes.push({ target: inc[1], line: i });
        }
    }

    return { labels, includes };
}

/* All occurrences of `name` as a whole identifier, comments/strings excluded. */
function findIdentifiers(text, name) {
    const hits = [];
    const lines = text.split(/\r\n|\r|\n/);

    for (let i = 0; i < lines.length; i++) {
        const masked = maskLine(lines[i]);
        let m;

        IDENT.lastIndex = 0;
        while ((m = IDENT.exec(masked)) !== null) {
            if (m[0] === name) {
                hits.push({ line: i, startChar: m.index, endChar: m.index + name.length });
            }
        }
    }

    return hits;
}

/* The identifier under a (0-based) character position, or null. */
function wordAt(lineText, character) {
    const masked = maskLine(lineText);
    let m;

    IDENT.lastIndex = 0;
    while ((m = IDENT.exec(masked)) !== null) {
        if (m.index <= character && character <= m.index + m[0].length) {
            return { word: m[0], start: m.index, end: m.index + m[0].length };
        }
    }

    return null;
}

/* Breadth-first label lookup across the `include` closure of `filePath`,
   with a visited set so include cycles terminate. Returns
   { file, label } or null. Reads from disk; the open document's own text
   is supplied by the caller as `rootText`. */
function findDefinitionAcrossIncludes(filePath, rootText, name) {
    const visited = new Set();
    const queue = [{ file: path.resolve(filePath), text: rootText }];

    while (queue.length > 0) {
        const { file, text } = queue.shift();
        const key = process.platform === 'win32' ? file.toLowerCase() : file;

        if (visited.has(key)) {
            continue;
        }
        visited.add(key);

        const idx = indexText(text);
        const hit = idx.labels.find(l => l.name === name);

        if (hit) {
            return { file, label: hit };
        }

        for (const inc of idx.includes) {
            const target = path.resolve(path.dirname(file), inc.target);
            const targetKey = process.platform === 'win32' ? target.toLowerCase() : target;

            if (visited.has(targetKey)) {
                continue;
            }

            let targetText;
            try {
                targetText = fs.readFileSync(target, 'utf8');
            }
            catch {
                continue;
            }

            queue.push({ file: target, text: targetText });
        }
    }

    return null;
}

/* True iff a probe outcome proves --stdin support (maize-49): only exit 1 plus
   the marker line counts. Any other outcome means an assembler that ignored
   the flags, a crash, or a timeout, and the caller must fall back. */
function classifyProbe(exitCode, stderr) {
    return exitCode === 1 && PROBE_MARKER.test(stderr || '');
}

module.exports = {
    parseMzasmError,
    parseMzasmErrors,
    maskLine,
    indexText,
    findIdentifiers,
    wordAt,
    findDefinitionAcrossIncludes,
    classifyProbe,
    PROBE_INPUT,
    PROBE_SOURCE_NAME,
};

/* ------------------------------------------------------------------------- */
/* Everything below runs only when launched as a server process, so the unit
   tests can require() the helpers without starting a connection. */

if (require.main !== module) {
    return;
}

const {
    createConnection,
    TextDocuments,
    DiagnosticSeverity,
    ProposedFeatures,
    SymbolKind,
    TextDocumentSyncKind,
} = require('vscode-languageserver/node');
const { TextDocument } = require('vscode-languageserver-textdocument');

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);

let hasConfiguration = false;
let warnedNoMzasm = false;

const CHECK_TIMEOUT_MS = parseInt(process.env.MZASM_CHECK_TIMEOUT_MS || '10000', 10);
const DEBOUNCE_MS = 300;

/* 'live' (buffer over stdin, on-type) or 'file' (saved state, maize-46
   behavior). Decided once per session by the version-skew probe. */
let modePromise = null;

const generations = new Map();    // uri -> latest validation generation
const inflight = new Map();       // uri -> running mzasm child process
const debounceTimers = new Map(); // uri -> pending didChange timer

/* Spawn mzasm, optionally feeding stdin, with a kill-timer. .cmd/.bat targets
   (wrapper scripts) need shell mode on Windows. cb(err, exitCode, stderr). */
function spawnMzasm(mzasmPath, args, inputText, cb) {
    const useShell = /\.(cmd|bat)$/i.test(mzasmPath);
    let child;

    try {
        // Shell mode (wrapper scripts) needs explicit quoting or cmd.exe
        // splits paths containing spaces.
        child = useShell
            ? spawn(`"${mzasmPath}"`, args.map(a => `"${a}"`), { shell: true, windowsHide: true })
            : spawn(mzasmPath, args, { windowsHide: true });
    }
    catch (e) {
        cb(e, -1, '');
        return null;
    }

    let stderr = '';
    let settled = false;
    const timer = setTimeout(() => { try { child.kill(); } catch { /* gone */ } }, CHECK_TIMEOUT_MS);

    child.stderr.on('data', (d) => { stderr += d; });
    child.on('error', (e) => {
        if (!settled) { settled = true; clearTimeout(timer); cb(e, -1, stderr); }
    });
    child.on('close', (code) => {
        if (!settled) { settled = true; clearTimeout(timer); cb(null, code, stderr); }
    });

    if (inputText !== null) {
        child.stdin.on('error', () => { /* EPIPE when the child exits first */ });
        child.stdin.end(inputText);
    }

    return child;
}

function ensureMode(mzasmPath) {
    if (!modePromise) {
        modePromise = new Promise((resolve) => {
            const args = ['--check', '--stdin', '--base-path', os.tmpdir(), '--source-name', PROBE_SOURCE_NAME];

            spawnMzasm(mzasmPath, args, PROBE_INPUT, (err, code, stderr) => {
                if (!err && classifyProbe(code, stderr)) {
                    resolve('live');
                    return;
                }

                connection.console.log(
                    `mzasm --stdin probe failed (${err ? err.code : 'exit ' + code}); falling back to save-time diagnostics`);
                connection.window.showInformationMessage(
                    'This mzasm build does not support --stdin; diagnostics update on save only. Rebuild mzasm for on-type checking.');
                resolve('file');
            });
        });
    }

    return modePromise;
}

connection.onInitialize((params) => {
    hasConfiguration = !!(params.capabilities.workspace && params.capabilities.workspace.configuration);

    return {
        capabilities: {
            textDocumentSync: {
                openClose: true,
                change: TextDocumentSyncKind.Incremental,
                save: true,
            },
            documentSymbolProvider: true,
            definitionProvider: true,
            referencesProvider: true,
        },
    };
});

async function getMzasmPath() {
    if (hasConfiguration) {
        try {
            const configured = await connection.workspace.getConfiguration({ section: 'maize.mzasm.path' });

            if (typeof configured === 'string' && configured.length > 0) {
                return configured;
            }
        }
        catch {
            /* fall through to defaults */
        }
    }

    return process.env.MZASM_PATH || 'mzasm';
}

function samePath(a, b) {
    const ra = path.resolve(a);
    const rb = path.resolve(b);

    return process.platform === 'win32'
        ? ra.toLowerCase() === rb.toLowerCase()
        : ra === rb;
}

async function validate(doc, isChange) {
    if (!doc.uri.startsWith('file:')) {
        return;
    }

    const fsPath = fileURLToPath(doc.uri);
    const mzasmPath = await getMzasmPath();
    const mode = await ensureMode(mzasmPath);

    if (mode === 'file' && isChange) {
        // Fallback mode checks saved state only; a dirty buffer has nothing
        // on disk worth checking.
        return;
    }

    const gen = (generations.get(doc.uri) || 0) + 1;
    generations.set(doc.uri, gen);

    const prev = inflight.get(doc.uri);
    if (prev) {
        try { prev.kill(); } catch { /* already gone */ }
    }

    const args = mode === 'live'
        ? ['--check', '--stdin', '--base-path', path.dirname(fsPath), '--source-name', fsPath]
        : ['--check', fsPath];
    const input = mode === 'live' ? doc.getText() : null;

    const child = spawnMzasm(mzasmPath, args, input, (err, code, stderr) => {
        if (generations.get(doc.uri) !== gen) {
            // A newer validation superseded this one (typically we killed it).
            return;
        }

        inflight.delete(doc.uri);

        if (err && (err.code === 'ENOENT' || err.code === 'EACCES')) {
            if (!warnedNoMzasm) {
                warnedNoMzasm = true;
                connection.window.showWarningMessage(
                    `mzasm not found at "${mzasmPath}". Set maize.mzasm.path to your built assembler; ` +
                    'diagnostics are disabled until then.');
            }
            return;
        }

        if (code === 0) {
            connection.sendDiagnostics({ uri: doc.uri, diagnostics: [] });
            return;
        }

        const parsed = parseMzasmErrors(stderr);

        if (parsed.length === 0) {
            // Killed by timeout, crashed, or unrecognized output: nothing to map.
            connection.sendDiagnostics({ uri: doc.uri, diagnostics: [] });
            connection.console.log(`mzasm --check produced no parseable diagnostic for ${fsPath}`);
            return;
        }

        const diagnostics = parsed.map((p) => {
            if (samePath(p.file, fsPath)) {
                const line = Math.max(0, p.line - 1);
                const lineText = doc.getText({
                    start: { line, character: 0 },
                    end: { line: line + 1, character: 0 },
                });

                return {
                    severity: DiagnosticSeverity.Error,
                    range: {
                        start: { line, character: 0 },
                        end: { line, character: Math.max(1, lineText.replace(/\r?\n$/, '').length) },
                    },
                    message: p.message,
                    source: 'mzasm',
                };
            }

            // The error is inside an INCLUDEd file; anchor it at the top of the
            // open document and name the real location in the message.
            return {
                severity: DiagnosticSeverity.Error,
                range: { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } },
                message: `in included file ${p.file}:${p.line}: ${p.message}`,
                source: 'mzasm',
            };
        });

        connection.sendDiagnostics({ uri: doc.uri, diagnostics });
    });

    if (child) {
        inflight.set(doc.uri, child);
    }
}

documents.onDidOpen((e) => { validate(e.document, false); });
documents.onDidSave((e) => { validate(e.document, false); });
documents.onDidChangeContent((e) => {
    // Fires on open as well as edits; the debounce plus generation guard makes
    // the duplicate open-time validation harmless.
    const uri = e.document.uri;
    const existing = debounceTimers.get(uri);

    if (existing) {
        clearTimeout(existing);
    }

    debounceTimers.set(uri, setTimeout(() => {
        debounceTimers.delete(uri);
        validate(e.document, true);
    }, DEBOUNCE_MS));
});
documents.onDidClose((e) => {
    const uri = e.document.uri;
    const timer = debounceTimers.get(uri);

    if (timer) {
        clearTimeout(timer);
        debounceTimers.delete(uri);
    }

    generations.set(uri, (generations.get(uri) || 0) + 1);

    const child = inflight.get(uri);
    if (child) {
        try { child.kill(); } catch { /* already gone */ }
        inflight.delete(uri);
    }

    connection.sendDiagnostics({ uri, diagnostics: [] });
});

connection.onDocumentSymbol((params) => {
    const doc = documents.get(params.textDocument.uri);

    if (!doc) {
        return null;
    }

    // One label kind, so one symbol kind. v1 chose between Function and
    // Constant here because it had two declaration forms; v2 has only the
    // colon form, and a branch with one reachable arm would be describing a
    // language this one is not.
    return indexText(doc.getText()).labels.map(l => ({
        name: l.name,
        kind: SymbolKind.Function,
        location: {
            uri: params.textDocument.uri,
            range: {
                start: { line: l.line, character: l.startChar },
                end: { line: l.line, character: l.endChar },
            },
        },
    }));
});

connection.onDefinition((params) => {
    const doc = documents.get(params.textDocument.uri);

    if (!doc || !doc.uri.startsWith('file:')) {
        return null;
    }

    const lineText = doc.getText({
        start: { line: params.position.line, character: 0 },
        end: { line: params.position.line + 1, character: 0 },
    });
    const w = wordAt(lineText.replace(/\r?\n$/, ''), params.position.character);

    if (!w) {
        return null;
    }

    const fsPath = fileURLToPath(doc.uri);
    const found = findDefinitionAcrossIncludes(fsPath, doc.getText(), w.word);

    if (!found) {
        return null;
    }

    return {
        uri: pathToFileURL(found.file).toString(),
        range: {
            start: { line: found.label.line, character: found.label.startChar },
            end: { line: found.label.line, character: found.label.endChar },
        },
    };
});

connection.onReferences((params) => {
    const doc = documents.get(params.textDocument.uri);

    if (!doc) {
        return null;
    }

    const lineText = doc.getText({
        start: { line: params.position.line, character: 0 },
        end: { line: params.position.line + 1, character: 0 },
    });
    const w = wordAt(lineText.replace(/\r?\n$/, ''), params.position.character);

    if (!w) {
        return null;
    }

    return findIdentifiers(doc.getText(), w.word).map(h => ({
        uri: params.textDocument.uri,
        range: {
            start: { line: h.line, character: h.startChar },
            end: { line: h.line, character: h.endChar },
        },
    }));
});

documents.listen(connection);
connection.listen();
