// Scripted verification for the Maize language server (maize-46 + maize-49).
//
// Part 1: unit checks on the pure helpers (require'd, no connection started).
// Part 2: probe contract against the real mazm binary.
// Part 3: end-to-end stdio session in LIVE mode (on-type diagnostics, symbols,
//         definition, references, cycle guard, burst settling).
// Part 4: end-to-end stdio session against a faithful pre-maize-49 stub,
//         proving the version-skew fallback to save-time diagnostics.
//
// Env: MAZM_PATH must point at a built mazm executable.
// Run: node tests/lsp.test.js   (from editors/vscode/)

'use strict';

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

ok(server.parseMazmErrors('').length === 0, 'unit: parseMazmErrors on empty input');
ok(server.parseMazmErrors('mazm: a.mazm:3: error: x').length === 1, 'unit: parseMazmErrors single line');
const multi = server.parseMazmErrors(
    'mazm: a.mazm:3: error: one\nmazm: b.mazm:7: error: two\nnoise line\nmazm: a.mazm:9: error: three\n');
ok(multi.length === 3 && multi[0].line === 3 && multi[1].file === 'b.mazm' && multi[2].message === 'three',
    'unit: parseMazmErrors returns all lines in order, mixed files, noise ignored');

ok(server.classifyProbe(1, 'mazm: mazm-stdin-probe:1: error: unterminated string literal starting at line 1'),
    'unit: classifyProbe accepts exit 1 + marker');
ok(!server.classifyProbe(0, ''), 'unit: classifyProbe rejects exit 0');
ok(!server.classifyProbe(1, 'mazm: somewhere.mazm:1: error: nope'),
    'unit: classifyProbe rejects exit 1 without the marker');

/* ---------------- shared LSP session harness ---------------- */

if (!process.env.MAZM_PATH) {
    console.log('SKIP e2e: MAZM_PATH not set');
    process.exit(failures === 0 ? 0 : 1);
}

function startSession(env) {
    const child = spawn(process.execPath, [path.join(ROOT, 'server', 'server.js'), '--stdio'], {
        cwd: ROOT,
        env: { ...process.env, MAZM_CHECK_TIMEOUT_MS: '5000', ...env },
        stdio: ['pipe', 'pipe', 'inherit'],
    });

    const session = {
        child,
        nextId: 1,
        pendingResponses: new Map(),
        received: new Map(),   // uri -> [publishDiagnostics params]
        buffer: Buffer.alloc(0),
    };

    child.stdout.on('data', (chunk) => {
        session.buffer = Buffer.concat([session.buffer, chunk]);

        for (;;) {
            const headerEnd = session.buffer.indexOf('\r\n\r\n');
            if (headerEnd < 0) { return; }

            const header = session.buffer.slice(0, headerEnd).toString('ascii');
            const m = /Content-Length: (\d+)/i.exec(header);
            if (!m) { throw new Error('bad LSP header: ' + header); }

            const length = parseInt(m[1], 10);
            const start = headerEnd + 4;
            if (session.buffer.length < start + length) { return; }

            const message = JSON.parse(session.buffer.slice(start, start + length).toString('utf8'));
            session.buffer = session.buffer.slice(start + length);

            if (message.id !== undefined && session.pendingResponses.has(message.id)) {
                session.pendingResponses.get(message.id)(message);
                session.pendingResponses.delete(message.id);
            }
            else if (message.method === 'textDocument/publishDiagnostics') {
                const uri = message.params.uri;
                if (!session.received.has(uri)) { session.received.set(uri, []); }
                session.received.get(uri).push(message.params);
            }
            // window/showMessage etc.: ignored.
        }
    });

    session.send = (message) => {
        const body = Buffer.from(JSON.stringify(message), 'utf8');
        child.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
        child.stdin.write(body);
    };

    session.request = (method, params) => {
        const id = session.nextId++;
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error(`timeout waiting for ${method}`)), 15000);
            session.pendingResponses.set(id, (msg) => { clearTimeout(timer); resolve(msg); });
            session.send({ jsonrpc: '2.0', id, method, params });
        });
    };

    session.notify = (method, params) => session.send({ jsonrpc: '2.0', method, params });

    session.countFor = (uri) => (session.received.get(uri) || []).length;

    /* Wait until no new publish for `uri` arrives for quietMs; return the last
       params seen (or null). Absorbs duplicate open+debounce validations. */
    session.settle = (uri, quietMs = 900, maxMs = 15000) => new Promise((resolve, reject) => {
        const startTime = Date.now();
        let lastCount = session.countFor(uri);
        let lastChange = Date.now();

        const poll = setInterval(() => {
            const count = session.countFor(uri);

            if (count !== lastCount) {
                lastCount = count;
                lastChange = Date.now();
            }
            else if (count > 0 && Date.now() - lastChange >= quietMs) {
                clearInterval(poll);
                const list = session.received.get(uri);
                resolve(list[list.length - 1]);
            }

            if (Date.now() - startTime > maxMs) {
                clearInterval(poll);
                reject(new Error('settle timeout for ' + uri));
            }
        }, 50);
    });

    session.open = (fsPath, text) => {
        session.notify('textDocument/didOpen', {
            textDocument: {
                uri: pathToFileURL(fsPath).toString(),
                languageId: 'mazm',
                version: 1,
                text: text !== undefined ? text : fs.readFileSync(fsPath, 'utf8'),
            },
        });
    };

    session.change = (fsPath, version, text) => {
        session.notify('textDocument/didChange', {
            textDocument: { uri: pathToFileURL(fsPath).toString(), version },
            contentChanges: [{ text }],
        });
    };

    session.stop = () => { session.notify('exit', {}); child.kill(); };

    return session;
}

const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const BROKEN = 'main:\n    LD nolabel R0\n';
const FIXED = 'main:\n    CP $00 R0\n    HALT\n';

/* ---------------- Part 2: probe contract vs the real binary ---------------- */

async function testProbeContract() {
    const result = await new Promise((resolve) => {
        const p = spawn(process.env.MAZM_PATH,
            ['--check', '--stdin', '--base-path', os.tmpdir(), '--source-name', server.PROBE_SOURCE_NAME]);
        let stderr = '';
        p.stderr.on('data', d => { stderr += d; });
        p.on('close', code => resolve({ code, stderr }));
        p.stdin.end(server.PROBE_INPUT);
    });

    ok(result.code === 1 && server.classifyProbe(result.code, result.stderr),
        'probe: real mazm produces exit 1 + mazm-stdin-probe marker');
}

/* ---------------- Part 3: live-mode e2e ---------------- */

async function testLiveMode() {
    const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mazm-lsp-'));
    const workFile = path.join(tmpDir, 'work.mazm');
    fs.writeFileSync(workFile, FIXED);   // disk is FIXED the whole test
    const workUri = pathToFileURL(workFile).toString();

    const s = startSession({});
    const init = await s.request('initialize', { processId: null, rootUri: null, capabilities: {} });
    ok(init.result && init.result.capabilities.definitionProvider === true,
        'live: initialize advertises definitionProvider');
    s.notify('initialized', {});

    // Buffer-over-disk: disk is FIXED, buffer is BROKEN. A diagnostic proves
    // the buffer is what gets checked.
    s.open(workFile, BROKEN);
    let last = await s.settle(workUri);
    ok(last.diagnostics.length === 1
        && last.diagnostics[0].severity === 1
        && last.diagnostics[0].range.start.line === 1
        && last.diagnostics[0].source === 'mazm',
        'live: broken BUFFER over clean disk publishes the error (buffer wins)');

    // Typing the fix clears it, no save involved.
    s.change(workFile, 2, FIXED);
    last = await s.settle(workUri);
    ok(last.diagnostics.length === 0, 'live: typing the fix clears diagnostics without saving');

    // Rapid burst settles on the final (broken) state with no stale overwrite.
    s.change(workFile, 3, FIXED);
    s.change(workFile, 4, BROKEN);
    s.change(workFile, 5, FIXED);
    s.change(workFile, 6, BROKEN);
    last = await s.settle(workUri);
    ok(last.diagnostics.length === 1 && last.diagnostics[0].range.start.line === 1,
        'live: rapid burst settles on the final state');

    // Multi-error buffer (maize-50): three squiggles at once, dropping as fixed.
    const multiFile = path.join(tmpDir, 'multi.mazm');
    fs.writeFileSync(multiFile, FIXED);
    const multiUri = pathToFileURL(multiFile).toString();
    const MULTI3 = 'main:\n    LD a R0\n    LD b R1\n    CALL nowhere\n    RET\n';
    const MULTI2 = 'main:\n    CP $01 R0\n    LD b R1\n    CALL nowhere\n    RET\n';

    s.open(multiFile, MULTI3);
    last = await s.settle(multiUri);
    ok(last.diagnostics.length === 3
        && last.diagnostics.map(d => d.range.start.line).join(',') === '1,2,3',
        'multi: three-error buffer publishes three diagnostics on the right lines');

    s.change(multiFile, 2, MULTI2);
    last = await s.settle(multiUri);
    ok(last.diagnostics.length === 2, 'multi: fixing one error drops the count to two');

    s.change(multiFile, 3, FIXED);
    last = await s.settle(multiUri);
    ok(last.diagnostics.length === 0, 'multi: fixing the rest clears all diagnostics');

    // Mixed buffer + include errors: anchored plus line-mapped in one publish.
    fs.writeFileSync(path.join(tmpDir, 'bad_inc.mazm'), 'lib:\n    FROB R0\n    RET\n');
    const mixedFile = path.join(tmpDir, 'mixed.mazm');
    fs.writeFileSync(mixedFile, FIXED);
    const mixedUri = pathToFileURL(mixedFile).toString();
    s.open(mixedFile, 'INCLUDE "bad_inc.mazm"\nmain:\n    LD x R0\n    RET\n');
    last = await s.settle(mixedUri);
    const anchored = last.diagnostics.filter(d => d.message.startsWith('in included file'));
    const mapped = last.diagnostics.filter(d => !d.message.startsWith('in included file'));
    ok(last.diagnostics.length === 2 && anchored.length === 1
        && anchored[0].range.start.line === 0
        && mapped.length === 1 && mapped[0].range.start.line === 2,
        'multi: mixed buffer+include errors split into anchored and line-mapped diagnostics');

    // Symbols on hello.mazm.
    const helloPath = path.resolve(ROOT, '..', '..', 'asm', 'hello.mazm');
    const helloUri = pathToFileURL(helloPath).toString();
    s.open(helloPath);
    await s.settle(helloUri);
    const symbols = await s.request('textDocument/documentSymbol', {
        textDocument: { uri: helloUri },
    });
    ok(symbols.result.map(sym => sym.name).sort().join(',') === 'hw_string,loop_body,loop_condition,loop_exit,main,strlen',
        'live: documentSymbol on hello.mazm returns all six labels');

    // Cross-INCLUDE definition + references from lsp_main.
    const mainPath = path.join(FIXTURES, 'lsp_main.mazm');
    const mainUri = pathToFileURL(mainPath).toString();
    s.open(mainPath);
    await s.settle(mainUri);
    const lines = fs.readFileSync(mainPath, 'utf8').split(/\r?\n/);
    const callLine = lines.findIndex(l => l.includes('CALL lib_func'));
    const defResp = await s.request('textDocument/definition', {
        textDocument: { uri: mainUri },
        position: { line: callLine, character: lines[callLine].indexOf('lib_func') + 2 },
    });
    ok(defResp.result && defResp.result.uri.endsWith('lsp_lib.mazm'),
        'live: definition of lib_func lands in lsp_lib.mazm');

    const helperLine = lines.findIndex(l => l.includes('CALL local_helper'));
    const refResp = await s.request('textDocument/references', {
        textDocument: { uri: mainUri },
        position: { line: helperLine, character: lines[helperLine].indexOf('local_helper') + 2 },
        context: { includeDeclaration: true },
    });
    ok(refResp.result && refResp.result.length === 2,
        'live: references on local_helper returns declaration + usage');

    // Cycle guard through the request path.
    const cycPath = path.join(FIXTURES, 'cyc_a.mazm');
    const cycUri = pathToFileURL(cycPath).toString();
    s.open(cycPath);
    await s.settle(cycUri);
    const started = Date.now();
    const missing = await s.request('textDocument/definition', {
        textDocument: { uri: cycUri },
        position: { line: 4, character: 5 },   // on RET: no such label anywhere
    });
    ok((missing.result === null || missing.result === undefined) && Date.now() - started < 5000,
        'live: definition across an include cycle terminates with null');

    s.stop();
}

/* ---------------- Part 4: version-skew fallback e2e ---------------- */

async function testFallback() {
    const stub = path.join(FIXTURES, process.platform === 'win32' ? 'old-mazm-stub.cmd' : 'old-mazm-stub.js');
    const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mazm-lsp-fb-'));
    const workFile = path.join(tmpDir, 'work.mazm');
    fs.writeFileSync(workFile, BROKEN);   // disk starts BROKEN
    const workUri = pathToFileURL(workFile).toString();

    const s = startSession({ MAZM_PATH: stub });
    await s.request('initialize', { processId: null, rootUri: null, capabilities: {} });
    s.notify('initialized', {});

    // Open: probe fails against the stub (it captures the --base-path dir as
    // its input and exits 0, no marker) -> fallback -> file-mode check of the
    // BROKEN disk content.
    s.open(workFile);
    let last = await s.settle(workUri);
    ok(last.diagnostics.length === 1 && last.diagnostics[0].range.start.line === 1,
        'fallback: save-time diagnostics work against the old-mazm stub');

    // didChange must NOT trigger any validation in fallback mode.
    const before = s.countFor(workUri);
    s.change(workFile, 2, FIXED);
    await sleep(1200);
    ok(s.countFor(workUri) === before, 'fallback: didChange publishes nothing (no dirty-buffer checking)');

    // Fix on disk + didSave clears.
    fs.writeFileSync(workFile, FIXED);
    s.notify('textDocument/didSave', { textDocument: { uri: workUri } });
    last = await s.settle(workUri);
    ok(last.diagnostics.length === 0, 'fallback: disk fix + didSave clears diagnostics');

    s.stop();
}

(async () => {
    try {
        await testProbeContract();
        await testLiveMode();
        await testFallback();
    }
    catch (e) {
        console.error(e);
        process.exit(2);
    }

    console.log(failures === 0 ? '\nALL LSP CHECKS PASSED' : `\n${failures} LSP CHECK(S) FAILED`);
    process.exit(failures === 0 ? 0 : 1);
})();
