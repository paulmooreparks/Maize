// Scripted verification for the Maize language server (maize-46).
//
// Part 1: unit checks on the pure helpers (require'd, no connection started).
// Part 2: an end-to-end stdio JSON-RPC session against server.js, driving
//         diagnostics (broken file -> error; fixed file -> cleared), document
//         symbols, cross-INCLUDE definition, references, and the include-cycle
//         guard.
//
// Env: MAZM_PATH must point at a built mazm executable.
// Run: node tests/lsp.test.js   (from editors/vscode/)

'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const { pathToFileURL } = require('url');

const ROOT = path.resolve(__dirname, '..');
const FIXTURES = path.join(__dirname, 'fixtures');
const server = require(path.join(ROOT, 'server', 'server.js'));

let failures = 0;
function ok(cond, label) {
    if (cond) { console.log('PASS ' + label); }
    else { failures++; console.log('FAIL ' + label); }
}

/* ---------------- Part 1: unit checks ---------------- */

const parsed = server.parseMazmError(
    'mazm: C:\\x\\broken.mazm:12: error: LD reads from a memory address');
ok(parsed && parsed.line === 12 && parsed.file === 'C:\\x\\broken.mazm'
    && parsed.message.startsWith('LD reads'), 'unit: parseMazmError extracts file/line/message');
ok(server.parseMazmError('Assembling from x') === null, 'unit: parseMazmError rejects non-error output');

ok(server.maskLine('    CP hw R0 ; hw is a label') === '    CP hw R0                ',
    'unit: maskLine blanks comments, preserves columns');
ok(server.maskLine('    STRING "a;b\\"c" ; tail').indexOf(';') === -1,
    'unit: maskLine blanks string bodies including escaped quotes and the comment');

const mainText = fs.readFileSync(path.join(FIXTURES, 'lsp_main.mazm'), 'utf8');
const idx = server.indexText(mainText);
ok(idx.labels.map(l => l.name).join(',') === 'entry,local_helper',
    'unit: indexText finds colon labels in lsp_main');
ok(idx.includes.length === 1 && idx.includes[0].target === 'lsp_lib.mazm',
    'unit: indexText finds INCLUDE target');

const libText = fs.readFileSync(path.join(FIXTURES, 'lsp_lib.mazm'), 'utf8');
const libIdx = server.indexText(libText);
ok(libIdx.labels.some(l => l.name === 'lib_data' && l.kind === 'directive')
    && libIdx.labels.some(l => l.name === 'lib_func' && l.kind === 'colon'),
    'unit: indexText finds LABEL-directive and colon declarations in lsp_lib');

const def = server.findDefinitionAcrossIncludes(
    path.join(FIXTURES, 'lsp_main.mazm'), mainText, 'lib_func');
ok(def && path.basename(def.file) === 'lsp_lib.mazm', 'unit: cross-INCLUDE definition resolves');

const cycText = fs.readFileSync(path.join(FIXTURES, 'cyc_a.mazm'), 'utf8');
const noDef = server.findDefinitionAcrossIncludes(
    path.join(FIXTURES, 'cyc_a.mazm'), cycText, 'does_not_exist');
ok(noDef === null, 'unit: include cycle terminates (missing symbol returns null)');

const refs = server.findIdentifiers(mainText, 'local_helper');
ok(refs.length === 2, 'unit: references finds declaration + usage of local_helper');

/* ---------------- Part 2: stdio E2E ---------------- */

if (!process.env.MAZM_PATH) {
    console.log('SKIP e2e: MAZM_PATH not set');
    process.exit(failures === 0 ? 0 : 1);
}

const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mazm-lsp-'));
const workFile = path.join(tmpDir, 'work.mazm');
const BROKEN = 'main:\n    LD nolabel R0\n';
const FIXED = 'main:\n    CP $00 R0\n    HALT\n';
fs.writeFileSync(workFile, BROKEN);

const child = spawn(process.execPath, [path.join(ROOT, 'server', 'server.js'), '--stdio'], {
    cwd: ROOT,
    env: { ...process.env, MAZM_CHECK_TIMEOUT_MS: '5000' },
    stdio: ['pipe', 'pipe', 'inherit'],
});

let nextId = 1;
const pendingResponses = new Map();
const diagnosticWaiters = [];
let buffer = Buffer.alloc(0);

child.stdout.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);

    for (;;) {
        const headerEnd = buffer.indexOf('\r\n\r\n');
        if (headerEnd < 0) { return; }

        const header = buffer.slice(0, headerEnd).toString('ascii');
        const m = /Content-Length: (\d+)/i.exec(header);
        if (!m) { throw new Error('bad LSP header: ' + header); }

        const length = parseInt(m[1], 10);
        const start = headerEnd + 4;
        if (buffer.length < start + length) { return; }

        const message = JSON.parse(buffer.slice(start, start + length).toString('utf8'));
        buffer = buffer.slice(start + length);
        dispatch(message);
    }
});

function dispatch(message) {
    if (message.id !== undefined && pendingResponses.has(message.id)) {
        pendingResponses.get(message.id)(message);
        pendingResponses.delete(message.id);
    }
    else if (message.method === 'textDocument/publishDiagnostics') {
        const waiter = diagnosticWaiters.shift();
        if (waiter) { waiter(message.params); }
    }
    // window/showMessage etc.: ignored.
}

function send(message) {
    const body = Buffer.from(JSON.stringify(message), 'utf8');
    child.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
    child.stdin.write(body);
}

function request(method, params) {
    const id = nextId++;
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(`timeout waiting for ${method} response`)), 15000);
        pendingResponses.set(id, (msg) => { clearTimeout(timer); resolve(msg); });
        send({ jsonrpc: '2.0', id, method, params });
    });
}

function notify(method, params) {
    send({ jsonrpc: '2.0', method, params });
}

function nextDiagnostics() {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('timeout waiting for diagnostics')), 15000);
        diagnosticWaiters.push((params) => { clearTimeout(timer); resolve(params); });
    });
}

function openDoc(fsPath, text) {
    notify('textDocument/didOpen', {
        textDocument: {
            uri: pathToFileURL(fsPath).toString(),
            languageId: 'mazm',
            version: 1,
            text: text !== undefined ? text : fs.readFileSync(fsPath, 'utf8'),
        },
    });
}

async function main() {
    const init = await request('initialize', {
        processId: null,
        rootUri: null,
        capabilities: {},
    });
    ok(init.result && init.result.capabilities.definitionProvider === true,
        'e2e: initialize advertises definitionProvider');
    notify('initialized', {});

    // Diagnostics: broken file -> one error on line 2 (0-based line 1).
    const diagPromise = nextDiagnostics();
    openDoc(workFile);
    const diag = await diagPromise;
    ok(diag.diagnostics.length === 1
        && diag.diagnostics[0].severity === 1
        && diag.diagnostics[0].range.start.line === 1
        && diag.diagnostics[0].source === 'mazm',
        'e2e: broken file publishes one mazm error on the reported line');

    // Fix on disk, didSave -> cleared.
    fs.writeFileSync(workFile, FIXED);
    notify('textDocument/didChange', {
        textDocument: { uri: pathToFileURL(workFile).toString(), version: 2 },
        contentChanges: [{ text: FIXED }],
    });
    const clearPromise = nextDiagnostics();
    notify('textDocument/didSave', {
        textDocument: { uri: pathToFileURL(workFile).toString() },
    });
    const cleared = await clearPromise;
    ok(cleared.diagnostics.length === 0, 'e2e: fixed + saved file clears diagnostics');

    // Symbols on hello.mazm.
    const helloPath = path.resolve(ROOT, '..', '..', 'asm', 'hello.mazm');
    const helloDiag = nextDiagnostics();
    openDoc(helloPath);
    await helloDiag; // hello.mazm is clean; consume its (empty) diagnostics
    const symbols = await request('textDocument/documentSymbol', {
        textDocument: { uri: pathToFileURL(helloPath).toString() },
    });
    const names = symbols.result.map(s => s.name).sort().join(',');
    ok(names === 'hw_string,loop_body,loop_condition,loop_exit,main,strlen',
        'e2e: documentSymbol on hello.mazm returns all six labels');

    // Cross-INCLUDE definition from lsp_main.
    const mainPath = path.join(FIXTURES, 'lsp_main.mazm');
    const mainDiag = nextDiagnostics();
    openDoc(mainPath);
    await mainDiag;
    const lines = fs.readFileSync(mainPath, 'utf8').split(/\r?\n/);
    const callLine = lines.findIndex(l => l.includes('CALL lib_func'));
    const defResp = await request('textDocument/definition', {
        textDocument: { uri: pathToFileURL(mainPath).toString() },
        position: { line: callLine, character: lines[callLine].indexOf('lib_func') + 2 },
    });
    ok(defResp.result && defResp.result.uri.endsWith('lsp_lib.mazm'),
        'e2e: definition of lib_func lands in lsp_lib.mazm');

    // References on local_helper.
    const helperLine = lines.findIndex(l => l.includes('CALL local_helper'));
    const refResp = await request('textDocument/references', {
        textDocument: { uri: pathToFileURL(mainPath).toString() },
        position: { line: helperLine, character: lines[helperLine].indexOf('local_helper') + 2 },
        context: { includeDeclaration: true },
    });
    ok(refResp.result && refResp.result.length === 2,
        'e2e: references on local_helper returns declaration + usage');

    // Cycle guard through the request path: definition of a missing symbol in
    // a cyclic include pair must return null promptly (not hang).
    const cycPath = path.join(FIXTURES, 'cyc_a.mazm');
    const cycDiag = nextDiagnostics();
    openDoc(cycPath);
    await cycDiag; // whatever mazm makes of the cycle, the server must respond
    const cycLines = fs.readFileSync(cycPath, 'utf8').split(/\r?\n/);
    const cycLabelLine = cycLines.findIndex(l => l.includes('cyc_a_label:'));
    const started = Date.now();
    const missing = await request('textDocument/definition', {
        textDocument: { uri: pathToFileURL(cycPath).toString() },
        position: { line: cycLabelLine + 1, character: 5 }, // on RET: no label named RET anywhere
    });
    ok((missing.result === null || missing.result === undefined) && Date.now() - started < 5000,
        'e2e: definition across an include cycle terminates with null');

    notify('exit', {});
    child.kill();
    console.log(failures === 0 ? '\nALL LSP CHECKS PASSED' : `\n${failures} LSP CHECK(S) FAILED`);
    process.exit(failures === 0 ? 0 : 1);
}

main().catch((e) => {
    console.error(e);
    child.kill();
    process.exit(2);
});
