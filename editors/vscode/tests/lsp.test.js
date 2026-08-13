// Scripted verification for the Maize v2 language server (maize-46 + maize-49, ported to v2
// by maize-429).
//
// Part 1: unit checks on the pure helpers (require'd, no connection started).
// Part 2: probe contract against the real mzasm binary.
// Part 3: end-to-end stdio session in LIVE mode (on-type diagnostics, symbols,
//         definition, references, cycle guard, burst settling).
// Part 4: end-to-end stdio session against a stub that does not support --stdin,
//         proving the version-skew fallback to save-time diagnostics.
//
// Env: MZASM_PATH must point at a built mzasm executable.
// Run: node tests/lsp.test.js   (from editors/vscode/)

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const { pathToFileURL } = require('url');

const ROOT = path.resolve(__dirname, '..');
const REPO = path.resolve(ROOT, '..', '..');
const FIXTURES = path.join(__dirname, 'fixtures');
const server = require(path.join(ROOT, 'server', 'server.js'));

let failures = 0;
function ok(cond, label) {
    if (cond) { console.log('PASS ' + label); }
    else { failures++; console.log('FAIL ' + label); }
}

/* ---------------- Part 1: unit checks ---------------- */

const parsed = server.parseMzasmError(
    "mzasm: C:\\x\\broken.mzasm:12: error: 'frob' is not a mnemonic or a directive");
ok(parsed && parsed.line === 12 && parsed.file === 'C:\\x\\broken.mzasm'
    && parsed.message.startsWith("'frob' is not"), 'unit: parseMzasmError extracts file/line/message');
ok(server.parseMzasmError('Assembling from x') === null, 'unit: parseMzasmError rejects non-error output');

// mzasm emits one `note: included from here` line per enclosing include
// (src/v2/mzasm.h). Those are not diagnostics and must not become squiggles.
const withNotes = server.parseMzasmErrors(
    "mzasm: lib.mzasm:2: error: 'bogus' is not a mnemonic or a directive\n" +
    'mzasm: main.mzasm:1: note: included from here\n' +
    "mzasm: main.mzasm:3: error: 'other' is not a mnemonic or a directive\n");
ok(withNotes.length === 2 && withNotes[0].file === 'lib.mzasm' && withNotes[1].line === 3,
    'unit: parseMzasmErrors ignores note: continuation lines');

// Asserted as a property rather than as a hand-counted literal: the point is that the comment
// is gone and every column still lines up, and a miscounted run of spaces in the expectation
// would fail a correct implementation.
const maskedComment = server.maskLine('    move hw r0 ; hw is a label');
ok(maskedComment.length === '    move hw r0 ; hw is a label'.length
    && maskedComment.indexOf(';') === -1
    && maskedComment.startsWith('    move hw r0 ')
    && maskedComment.trimEnd() === '    move hw r0',
    'unit: maskLine blanks comments, preserves columns');
ok(server.maskLine('    data_string "a;b\\"c" ; tail').indexOf(';') === -1,
    'unit: maskLine blanks string bodies including escaped quotes and the comment');
// A semicolon inside a character literal is an ordinary character, not a comment start.
ok(server.maskLine("    data_byte ';' r0").endsWith('r0'),
    'unit: maskLine does not treat a semicolon inside a character literal as a comment');

const mainText = fs.readFileSync(path.join(FIXTURES, 'lsp_main.mzasm'), 'utf8');
const idx = server.indexText(mainText);
ok(idx.labels.map(l => l.name).join(',') === 'entry,local_helper',
    'unit: indexText finds colon labels in lsp_main');
ok(idx.includes.length === 1 && idx.includes[0].target === 'lsp_lib.mzasm',
    'unit: indexText finds the lowercase include target');
// v2's include is lowercase, and the v1 spelling is not a directive in this language.
ok(server.indexText('INCLUDE "lsp_lib.mzasm"\n').includes.length === 0,
    'unit: indexText does not accept the uppercase v1 INCLUDE spelling');

const libText = fs.readFileSync(path.join(FIXTURES, 'lsp_lib.mzasm'), 'utf8');
const libIdx = server.indexText(libText);
ok(libIdx.labels.map(l => l.name).sort().join(',') === 'lib_data,lib_func',
    'unit: indexText finds both colon declarations in lsp_lib');
// AC-12's unit half: no label carries a directive kind, because v2 has no LABEL directive.
ok(libIdx.labels.every(l => l.kind === undefined),
    'unit: no label carries a directive-shaped kind');

const def = server.findDefinitionAcrossIncludes(
    path.join(FIXTURES, 'lsp_main.mzasm'), mainText, 'lib_func');
ok(def && path.basename(def.file) === 'lsp_lib.mzasm', 'unit: cross-include definition resolves');

const cycText = fs.readFileSync(path.join(FIXTURES, 'cyc_a.mzasm'), 'utf8');
const noDef = server.findDefinitionAcrossIncludes(
    path.join(FIXTURES, 'cyc_a.mzasm'), cycText, 'does_not_exist');
ok(noDef === null, 'unit: include cycle terminates (missing symbol returns null)');

const refs = server.findIdentifiers(mainText, 'local_helper');
ok(refs.length === 2, 'unit: references finds declaration + usage of local_helper');

ok(server.parseMzasmErrors('').length === 0, 'unit: parseMzasmErrors on empty input');
ok(server.parseMzasmErrors('mzasm: a.mzasm:3: error: x').length === 1, 'unit: parseMzasmErrors single line');
const multi = server.parseMzasmErrors(
    'mzasm: a.mzasm:3: error: one\nmzasm: b.mzasm:7: error: two\nnoise line\nmzasm: a.mzasm:9: error: three\n');
ok(multi.length === 3 && multi[0].line === 3 && multi[1].file === 'b.mzasm' && multi[2].message === 'three',
    'unit: parseMzasmErrors returns all lines in order, mixed files, noise ignored');

ok(server.classifyProbe(1, "mzasm: mzasm-stdin-probe:1: error: 'no_such_instruction' is not a mnemonic or a directive"),
    'unit: classifyProbe accepts exit 1 + marker');
ok(!server.classifyProbe(0, ''), 'unit: classifyProbe rejects exit 0');
ok(!server.classifyProbe(1, 'mzasm: somewhere.mzasm:1: error: nope'),
    'unit: classifyProbe rejects exit 1 without the marker');

/* ---------------- shared LSP session harness ---------------- */

if (!process.env.MZASM_PATH) {
    console.log('SKIP e2e: MZASM_PATH not set');
    process.exit(failures === 0 ? 0 : 1);
}

function startSession(env) {
    const child = spawn(process.execPath, [path.join(ROOT, 'server', 'server.js'), '--stdio'], {
        cwd: ROOT,
        env: { ...process.env, MZASM_CHECK_TIMEOUT_MS: '5000', ...env },
        stdio: ['pipe', 'pipe', 'inherit'],
    });

    const session = {
        child,
        nextId: 1,
        pendingResponses: new Map(),
        received: new Map(),   // uri -> [publishDiagnostics params]
        buffer: Buffer.alloc(0),
    };

    // Writes racing a child exit (e.g. during stop()) EPIPE on Windows; that
    // must not crash the test process.
    child.stdin.on('error', () => {});

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

    /* Wait for a publish newer than `afterCount`, then for no further publish
       during quietMs; return the last params seen. Requiring a NEW publish
       before the quiet window starts prevents a slow cold spawn from letting
       the window elapse on the PREVIOUS publish (the settle race that flaked
       clear-on-type checks under load). Callers capture countFor(uri) before
       the triggering action and pass it here. */
    session.settle = (uri, afterCount = 0, quietMs = 900, maxMs = 20000) => new Promise((resolve, reject) => {
        const startTime = Date.now();
        let lastCount = session.countFor(uri);
        let lastChange = Date.now();

        const poll = setInterval(() => {
            const count = session.countFor(uri);

            if (count !== lastCount) {
                lastCount = count;
                lastChange = Date.now();
            }
            else if (count > afterCount && Date.now() - lastChange >= quietMs) {
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
                languageId: 'mzasm',
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

// An unrecognized mnemonic is the error these fixtures use throughout: it rests on no lexical
// construct, so it stays an error whatever else the language settles on, and it lands on a
// predictable line.
const BROKEN = 'entry:\n    no_such_instruction\n';
const FIXED = 'entry:\n    nop\n    halt\n';

/* ---------------- Part 2: probe contract vs the real binary ---------------- */

async function testProbeContract() {
    const result = await new Promise((resolve) => {
        const p = spawn(process.env.MZASM_PATH,
            ['--check', '--stdin', '--base-path', os.tmpdir(), '--source-name', server.PROBE_SOURCE_NAME]);
        let stderr = '';
        p.stderr.on('data', d => { stderr += d; });
        p.on('close', code => resolve({ code, stderr }));
        p.stdin.on('error', () => {});
        p.stdin.end(server.PROBE_INPUT);
    });

    ok(result.code === 1 && server.classifyProbe(result.code, result.stderr),
        'probe: real mzasm produces exit 1 + mzasm-stdin-probe marker');

    // Regression class carried over from maize-50: a buffer in a natural
    // mid-typing state must be a clean exit-1 with a parseable line, never a
    // crash exit. A bare mnemonic on its own line is that state.
    const partial = await new Promise((resolve) => {
        const p = spawn(process.env.MZASM_PATH,
            ['--check', '--stdin', '--base-path', os.tmpdir(), '--source-name', 'partial.mzasm']);
        let stderr = '';
        p.stderr.on('data', d => { stderr += d; });
        p.on('close', code => resolve({ code, stderr }));
        p.stdin.on('error', () => {});
        // Trailing newline matters: it dispatches the bare mnemonic, which is
        // the state that used to crash v1's assembler.
        p.stdin.end('entry:\n    nop\n    move\n');
    });
    ok(partial.code === 1 && server.parseMzasmErrors(partial.stderr).length > 0,
        'probe: bare-mnemonic partial buffer exits 1 with a parseable diagnostic (no crash)');

    // Same class, short-operand variant: a two-operand mnemonic truncated after
    // one operand.
    const shortOp = await new Promise((resolve) => {
        const p = spawn(process.env.MZASM_PATH,
            ['--check', '--stdin', '--base-path', os.tmpdir(), '--source-name', 'short.mzasm']);
        let stderr = '';
        p.stderr.on('data', d => { stderr += d; });
        p.on('close', code => resolve({ code, stderr }));
        p.stdin.on('error', () => {});
        p.stdin.end('entry:\n    move r1\n');
    });
    ok(shortOp.code === 1 && server.parseMzasmErrors(shortOp.stderr).length > 0,
        'probe: one-of-two-operands truncation exits 1 with a parseable diagnostic (no crash)');
}

/* ---------------- Part 3: live-mode e2e ---------------- */

async function testLiveMode() {
    const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mzasm-lsp-'));
    const workFile = path.join(tmpDir, 'work.mzasm');
    fs.writeFileSync(workFile, FIXED);   // disk is FIXED the whole test
    const workUri = pathToFileURL(workFile).toString();

    const s = startSession({});
    const init = await s.request('initialize', { processId: null, rootUri: null, capabilities: {} });
    ok(init.result && init.result.capabilities.definitionProvider === true,
        'live: initialize advertises definitionProvider');
    s.notify('initialized', {});

    // Buffer-over-disk: disk is FIXED, buffer is BROKEN. A diagnostic proves
    // the buffer is what gets checked.
    let mark = s.countFor(workUri);
    s.open(workFile, BROKEN);
    let last = await s.settle(workUri, mark);
    ok(last.diagnostics.length === 1
        && last.diagnostics[0].severity === 1
        && last.diagnostics[0].range.start.line === 1
        && last.diagnostics[0].source === 'mzasm',
        'live: broken BUFFER over clean disk publishes the error at line 1, sourced mzasm');

    // Typing the fix clears it, no save involved.
    mark = s.countFor(workUri);
    s.change(workFile, 2, FIXED);
    last = await s.settle(workUri, mark);
    ok(last.diagnostics.length === 0, 'live: typing the fix clears diagnostics without saving');

    // Rapid burst settles on the final (broken) state with no stale overwrite.
    mark = s.countFor(workUri);
    s.change(workFile, 3, FIXED);
    s.change(workFile, 4, BROKEN);
    s.change(workFile, 5, FIXED);
    s.change(workFile, 6, BROKEN);
    last = await s.settle(workUri, mark);
    ok(last.diagnostics.length === 1 && last.diagnostics[0].range.start.line === 1,
        'live: rapid burst settles on the final state');

    // Multi-error buffer: three squiggles at once, dropping as fixed.
    const multiFile = path.join(tmpDir, 'multi.mzasm');
    fs.writeFileSync(multiFile, FIXED);
    const multiUri = pathToFileURL(multiFile).toString();
    const MULTI3 = 'entry:\n    bogus_one\n    bogus_two\n    bogus_three\n    halt\n';
    const MULTI2 = 'entry:\n    nop\n    bogus_two\n    bogus_three\n    halt\n';

    mark = s.countFor(multiUri);
    s.open(multiFile, MULTI3);
    last = await s.settle(multiUri, mark);
    ok(last.diagnostics.length === 3
        && last.diagnostics.map(d => d.range.start.line).join(',') === '1,2,3',
        'multi: three-error buffer publishes three diagnostics on the right lines');

    mark = s.countFor(multiUri);
    s.change(multiFile, 2, MULTI2);
    last = await s.settle(multiUri, mark);
    ok(last.diagnostics.length === 2, 'multi: fixing one error drops the count to two');

    mark = s.countFor(multiUri);
    s.change(multiFile, 3, FIXED);
    last = await s.settle(multiUri, mark);
    ok(last.diagnostics.length === 0, 'multi: fixing the rest clears all diagnostics');

    // Mixed buffer + include errors: anchored plus line-mapped in one publish.
    // mzasm also emits a `note: included from here` line here, which must not
    // become a third diagnostic.
    fs.writeFileSync(path.join(tmpDir, 'bad_inc.mzasm'), 'lib:\n    bogus_lib\n    return\n');
    const mixedFile = path.join(tmpDir, 'mixed.mzasm');
    fs.writeFileSync(mixedFile, FIXED);
    const mixedUri = pathToFileURL(mixedFile).toString();
    mark = s.countFor(mixedUri);
    s.open(mixedFile, 'include "bad_inc.mzasm"\nentry:\n    bogus_here\n    halt\n');
    last = await s.settle(mixedUri, mark);
    const anchored = last.diagnostics.filter(d => d.message.startsWith('in included file'));
    const mapped = last.diagnostics.filter(d => !d.message.startsWith('in included file'));
    ok(last.diagnostics.length === 2 && anchored.length === 1
        && anchored[0].range.start.line === 0
        && mapped.length === 1 && mapped[0].range.start.line === 2,
        'multi: mixed buffer+include errors split into anchored and line-mapped diagnostics');

    // Symbols on the real hello.mzasm.
    const helloPath = path.join(REPO, 'asm', 'v2', 'hello.mzasm');
    const helloUri = pathToFileURL(helloPath).toString();
    mark = s.countFor(helloUri);
    s.open(helloPath);
    await s.settle(helloUri, mark);
    const symbols = await s.request('textDocument/documentSymbol', {
        textDocument: { uri: helloUri },
    });
    ok(symbols.result.map(sym => sym.name).sort().join(',') === 'done,emit_byte,message,start',
        'live: documentSymbol on hello.mzasm returns its four colon labels');
    // AC-12: one label form means one symbol kind. SymbolKind.Function is 12.
    ok(symbols.result.every(sym => sym.kind === 12),
        'live: every documentSymbol is SymbolKind.Function (no directive-shaped kind)');

    // Cross-include definition + references from lsp_main.
    const mainPath = path.join(FIXTURES, 'lsp_main.mzasm');
    const mainUri = pathToFileURL(mainPath).toString();
    mark = s.countFor(mainUri);
    s.open(mainPath);
    await s.settle(mainUri, mark);
    const lines = fs.readFileSync(mainPath, 'utf8').split(/\r?\n/);
    const callLine = lines.findIndex(l => l.includes('call lib_func'));
    const defResp = await s.request('textDocument/definition', {
        textDocument: { uri: mainUri },
        position: { line: callLine, character: lines[callLine].indexOf('lib_func') + 2 },
    });
    ok(defResp.result && defResp.result.uri.endsWith('lsp_lib.mzasm'),
        'live: definition of lib_func lands in lsp_lib.mzasm');

    const helperLine = lines.findIndex(l => l.includes('call local_helper'));
    const refResp = await s.request('textDocument/references', {
        textDocument: { uri: mainUri },
        position: { line: helperLine, character: lines[helperLine].indexOf('local_helper') + 2 },
        context: { includeDeclaration: true },
    });
    ok(refResp.result && refResp.result.length === 2,
        'live: references on local_helper returns declaration + usage');

    // Cycle guard through the request path.
    const cycPath = path.join(FIXTURES, 'cyc_a.mzasm');
    const cycUri = pathToFileURL(cycPath).toString();
    mark = s.countFor(cycUri);
    s.open(cycPath);
    await s.settle(cycUri, mark);
    const started = Date.now();
    const missing = await s.request('textDocument/definition', {
        textDocument: { uri: cycUri },
        position: { line: 4, character: 5 },   // on return: no such label anywhere
    });
    ok((missing.result === null || missing.result === undefined) && Date.now() - started < 5000,
        'live: definition across an include cycle terminates with null');

    s.stop();
}

/* ---------------- Part 4: version-skew fallback e2e ---------------- */

async function testFallback() {
    const stub = path.join(FIXTURES, process.platform === 'win32' ? 'old-mzasm-stub.cmd' : 'old-mzasm-stub.js');
    const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mzasm-lsp-fb-'));
    const workFile = path.join(tmpDir, 'work.mzasm');
    fs.writeFileSync(workFile, BROKEN);   // disk starts BROKEN
    const workUri = pathToFileURL(workFile).toString();

    const s = startSession({ MZASM_PATH: stub });
    await s.request('initialize', { processId: null, rootUri: null, capabilities: {} });
    s.notify('initialized', {});

    // Open: probe fails against the stub (it captures the --base-path dir as
    // its input and exits 0, no marker) -> fallback -> file-mode check of the
    // BROKEN disk content.
    let mark = s.countFor(workUri);
    s.open(workFile);
    let last = await s.settle(workUri, mark);
    ok(last.diagnostics.length === 1 && last.diagnostics[0].range.start.line === 1,
        'fallback: save-time diagnostics work against a stub with no --stdin support');

    // didChange must NOT trigger any validation in fallback mode.
    const before = s.countFor(workUri);
    s.change(workFile, 2, FIXED);
    await sleep(1200);
    ok(s.countFor(workUri) === before, 'fallback: didChange publishes nothing (no dirty-buffer checking)');

    // Fix on disk + didSave clears.
    mark = s.countFor(workUri);
    fs.writeFileSync(workFile, FIXED);
    s.notify('textDocument/didSave', { textDocument: { uri: workUri } });
    last = await s.settle(workUri, mark);
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
