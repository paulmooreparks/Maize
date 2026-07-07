// Maize assembly language server (maize-46).
//
// Diagnostics come from the assembler itself: `mazm --check <file>` is spawned
// against the saved file and its single fatal line (mazm: <file>:<line>:
// error: <msg>) becomes the diagnostic. The symbol index below is a line-scan
// projection of mazm's tokenizer, the same stance as the TextMate grammar;
// anything semantic stays mazm's job.

'use strict';

const fs = require('fs');
const path = require('path');
const { execFile } = require('child_process');
const { pathToFileURL, fileURLToPath } = require('url');

const ERROR_LINE = /^mazm: (.+?):(\d+): error: (.+)$/m;
const IDENT = /[A-Za-z_][A-Za-z0-9_]*/g;
const COLON_LABEL = /^(\s*)([A-Za-z_][A-Za-z0-9_]*):/;
const LABEL_DIRECTIVE = /^(\s*LABEL[ \t]+)([A-Za-z_][A-Za-z0-9_]*)/;
const INCLUDE_LINE = /^\s*INCLUDE[ \t]+"([^"]+)"/;

/* Parse mazm's fatal diagnostic line out of a stderr blob. */
function parseMazmError(stderr) {
    const m = ERROR_LINE.exec(stderr || '');

    if (!m) {
        return null;
    }

    return { file: m[1], line: parseInt(m[2], 10), message: m[3] };
}

/* Blank out the comment tail and string bodies of a source line, preserving
   columns, so identifier scans cannot match inside either. */
function maskLine(line) {
    let out = '';
    let inString = false;
    let escaped = false;

    for (let i = 0; i < line.length; i++) {
        const c = line[i];

        if (inString) {
            out += ' ';

            if (escaped) {
                escaped = false;
            }
            else if (c === '\\') {
                escaped = true;
            }
            else if (c === '"') {
                inString = false;
            }
        }
        else if (c === '"') {
            out += ' ';
            inString = true;
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

/* Index a document: label declarations (colon-form and LABEL directives) and
   INCLUDE targets. Line numbers are 0-based. */
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
                kind: 'colon',
            });
        }

        const directive = LABEL_DIRECTIVE.exec(masked);
        if (directive) {
            labels.push({
                name: directive[2],
                line: i,
                startChar: directive[1].length,
                endChar: directive[1].length + directive[2].length,
                kind: 'directive',
            });
        }

        // INCLUDE targets live inside the string, so scan the raw line.
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

/* Breadth-first label lookup across the INCLUDE closure of `filePath`,
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

module.exports = {
    parseMazmError,
    maskLine,
    indexText,
    findIdentifiers,
    wordAt,
    findDefinitionAcrossIncludes,
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
let warnedNoMazm = false;

const CHECK_TIMEOUT_MS = parseInt(process.env.MAZM_CHECK_TIMEOUT_MS || '10000', 10);

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

async function getMazmPath() {
    if (hasConfiguration) {
        try {
            const configured = await connection.workspace.getConfiguration({ section: 'maize.mazm.path' });

            if (typeof configured === 'string' && configured.length > 0) {
                return configured;
            }
        }
        catch {
            /* fall through to defaults */
        }
    }

    return process.env.MAZM_PATH || 'mazm';
}

function samePath(a, b) {
    const ra = path.resolve(a);
    const rb = path.resolve(b);

    return process.platform === 'win32'
        ? ra.toLowerCase() === rb.toLowerCase()
        : ra === rb;
}

async function validate(doc) {
    if (!doc.uri.startsWith('file:')) {
        return;
    }

    const fsPath = fileURLToPath(doc.uri);
    const mazmPath = await getMazmPath();

    execFile(mazmPath, ['--check', fsPath], { timeout: CHECK_TIMEOUT_MS }, (error, stdout, stderr) => {
        if (error && (error.code === 'ENOENT' || error.code === 'EACCES')) {
            if (!warnedNoMazm) {
                warnedNoMazm = true;
                connection.window.showWarningMessage(
                    `mazm not found at "${mazmPath}". Set maize.mazm.path to your built assembler; ` +
                    'diagnostics are disabled until then.');
            }
            return;
        }

        if (!error) {
            connection.sendDiagnostics({ uri: doc.uri, diagnostics: [] });
            return;
        }

        const parsed = parseMazmError(stderr);

        if (!parsed) {
            // Killed by timeout, crashed, or unrecognized output: nothing to map.
            connection.sendDiagnostics({ uri: doc.uri, diagnostics: [] });
            connection.console.log(`mazm --check produced no parseable diagnostic for ${fsPath}`);
            return;
        }

        let diagnostic;

        if (samePath(parsed.file, fsPath)) {
            const line = Math.max(0, parsed.line - 1);
            const lineText = doc.getText({
                start: { line, character: 0 },
                end: { line: line + 1, character: 0 },
            });

            diagnostic = {
                severity: DiagnosticSeverity.Error,
                range: {
                    start: { line, character: 0 },
                    end: { line, character: Math.max(1, lineText.replace(/\r?\n$/, '').length) },
                },
                message: parsed.message,
                source: 'mazm',
            };
        }
        else {
            // The error is inside an INCLUDEd file; anchor it at the top of the
            // open document and name the real location in the message.
            diagnostic = {
                severity: DiagnosticSeverity.Error,
                range: { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } },
                message: `in included file ${parsed.file}:${parsed.line}: ${parsed.message}`,
                source: 'mazm',
            };
        }

        connection.sendDiagnostics({ uri: doc.uri, diagnostics: [diagnostic] });
    });
}

documents.onDidOpen((e) => { validate(e.document); });
documents.onDidSave((e) => { validate(e.document); });
documents.onDidClose((e) => {
    connection.sendDiagnostics({ uri: e.document.uri, diagnostics: [] });
});

connection.onDocumentSymbol((params) => {
    const doc = documents.get(params.textDocument.uri);

    if (!doc) {
        return null;
    }

    return indexText(doc.getText()).labels.map(l => ({
        name: l.name,
        kind: l.kind === 'directive' ? SymbolKind.Constant : SymbolKind.Function,
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
