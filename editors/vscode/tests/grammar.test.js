// grammar.test.js (maize-429): the TextMate grammar, checked against the specification and
// against real tokenization.
//
// Run: node tests/grammar.test.js      (from editors/vscode/)
//
// Three things happen here, and the third is the one worth reading.
//
// Part 1 re-derives the 187 assigned opcodes' mnemonic spellings from
// docs/spec-v2/appendix-a-opcode-map.md, independently of the grammar, and fails naming the
// exact spellings when the checked-in grammar and the appendix disagree.
//
// Part 2 tokenizes source through vscode-textmate, the same engine VS Code itself runs, and
// asserts the SCOPE NAMES produced. Most of it is negative: a bare `10` must not come back as
// a number, `MOVE` must not come back as a mnemonic, `r32` must not come back as a register.
//
// Part 3 runs every Part 2 assertion a second time against a deliberately over-permissive
// grammar and requires them to FAIL. A grammar that highlights everything passes every
// "this highlights" test ever written, so a suite of positive assertions cannot tell a working
// grammar from a broken one. Part 3 is what earns the right to believe Part 2: it proves the
// negative checks have teeth by showing them bite.

'use strict';

const fs = require('fs');
const path = require('path');
const vsctm = require('vscode-textmate');
const oniguruma = require('vscode-oniguruma');
const { parseAppendix } = require('./appendix-a.js');

const ROOT = path.resolve(__dirname, '..');
const REPO = path.resolve(ROOT, '..', '..');
const APPENDIX = path.join(REPO, 'docs', 'spec-v2', 'appendix-a-opcode-map.md');
const GRAMMAR_PATH = path.join(ROOT, 'syntaxes', 'mzasm.tmLanguage.json');
const FIXTURES = path.join(__dirname, 'fixtures');

let failures = 0;
function ok(cond, label) {
    if (cond) { console.log('PASS ' + label); }
    else { failures++; console.log('FAIL ' + label); }
}

/* ---------------- the tokenizer harness ---------------- */

async function makeTokenizer(grammarObject) {
    const wasm = fs.readFileSync(
        path.join(ROOT, 'node_modules', 'vscode-oniguruma', 'release', 'onig.wasm'));
    await oniguruma.loadWASM(wasm.buffer.slice(
        wasm.byteOffset, wasm.byteOffset + wasm.byteLength));

    const registry = new vsctm.Registry({
        onigLib: Promise.resolve({
            createOnigScanner: (sources) => new oniguruma.OnigScanner(sources),
            createOnigString: (str) => new oniguruma.OnigString(str),
        }),
        loadGrammar: () => Promise.resolve(
            vsctm.parseRawGrammar(JSON.stringify(grammarObject), 'mzasm.tmLanguage.json')),
    });

    const grammar = await registry.loadGrammar('source.mzasm');

    /* Tokenize a whole document, returning one array of tokens per line. Each token is
       { text, startIndex, endIndex, scopes }. */
    return function tokenize(text) {
        let ruleStack = vsctm.INITIAL;
        return text.split(/\r\n|\r|\n/).map((line) => {
            const result = grammar.tokenizeLine(line, ruleStack);
            ruleStack = result.ruleStack;
            return result.tokens.map(t => ({
                text: line.substring(t.startIndex, t.endIndex),
                startIndex: t.startIndex,
                endIndex: t.endIndex,
                scopes: t.scopes,
            }));
        });
    };
}

/* Every scope carried by the token covering `column` on a single-line source. */
function scopesAt(tokenize, line, column) {
    const tokens = tokenize(line)[0];
    const hit = tokens.find(t => t.startIndex <= column && column < t.endIndex);
    return hit ? hit.scopes : [];
}

/* The token covering `column`, or null. */
function tokenAt(tokenize, line, column) {
    const tokens = tokenize(line)[0];
    return tokens.find(t => t.startIndex <= column && column < t.endIndex) || null;
}

const hasScope = (scopes, prefix) => scopes.some(s => s === prefix || s.startsWith(prefix + '.'));

/* Every distinct scope appearing anywhere in a document. */
function allScopes(tokenize, text) {
    const out = new Set();
    for (const line of tokenize(text)) {
        for (const token of line) {
            for (const scope of token.scopes) {
                out.add(scope);
            }
        }
    }
    return out;
}

/* ---------------- Part 1: the grammar against the specification ---------------- */

function testAgainstAppendix() {
    const parsed = parseAppendix(APPENDIX);

    ok(parsed.errors.length === 0,
        'appendix: parses clean' +
        (parsed.errors.length ? ' -- ' + parsed.errors.join('; ') : ''));
    ok(parsed.assignedRows === parsed.statedAssigned && parsed.assignedRows > 0,
        'appendix: walked ' + parsed.assignedRows + ' assigned rows, A.1 states ' +
        parsed.statedAssigned);

    // Recover the spellings the checked-in grammar actually carries, by reading its mnemonic
    // rule rather than by re-running the generator. The generator is not consulted here at
    // all: if it were, a bug in the generator would check itself and agree.
    const grammar = JSON.parse(fs.readFileSync(GRAMMAR_PATH, 'utf8'));
    const match = grammar.repository.mnemonic.match;
    const inner = /^\\b\(\?:(.*)\)\\b$/.exec(match);

    if (!inner) {
        ok(false, 'appendix: the grammar mnemonic rule has the expected alternation shape');
        return;
    }

    const inGrammar = new Set(inner[1].split('|').map(s => s.replace(/\\(.)/g, '$1')));
    const inSpec = new Set(parsed.mnemonics);

    const missing = [...inSpec].filter(m => !inGrammar.has(m));
    const extra = [...inGrammar].filter(m => !inSpec.has(m));

    ok(missing.length === 0 && extra.length === 0,
        'appendix: the grammar mnemonic table equals appendix A' +
        (missing.length ? ' -- MISSING from the grammar: ' + missing.join(', ') : '') +
        (extra.length ? ' -- NOT IN the appendix: ' + extra.join(', ') : '') +
        (missing.length || extra.length
            ? '. Rerun: node tests/generate-grammar.js'
            : ' (' + inSpec.size + " spellings)"));

    // The alternation must be longest-first, or a longer spelling gets clipped by a shorter
    // prefix of itself. `add` matches the head of `add.h` with a word boundary after it,
    // because the dot is not a word character.
    const order = inner[1].split('|').map(s => s.replace(/\\(.)/g, '$1'));
    let ordered = true;
    for (let i = 1; i < order.length; i++) {
        if (order[i].length > order[i - 1].length) { ordered = false; break; }
    }
    ok(ordered, 'appendix: the mnemonic alternation is ordered longest-first');
}

/* ---------------- Part 2: scope assertions ---------------- */

/* Every behavioral assertion lives here, as a function of a tokenizer, so that Part 3 can run
   the identical set against a broken grammar and require the negatives to fail. Returns a list
   of { label, pass }. */
function scopeChecks(tokenize) {
    const results = [];
    const check = (pass, label) => results.push({ label, pass });

    // ---- AC-4: the mandatory-base rule ----
    // assembler.md "Numeric literals": "a token that begins with a digit is a syntax error".
    // A bare digit run must not be painted as a number of any base.
    // A STANDALONE digit run, so the column must skip the digits inside register names: the
    // first digit of "add r1 10 r3" belongs to r1, and an assertion aimed there would be
    // testing the register rule while claiming to test the base rule.
    const bareDigitColumn = (line) => line.search(/(?<![A-Za-z0-9_.])\d/);

    for (const line of ['    add r1 10 r3', '    data_byte 100', '    origin 4096']) {
        const column = bareDigitColumn(line);
        const scopes = scopesAt(tokenize, line, column);
        check(!hasScope(scopes, 'constant.numeric'),
            'AC-4: unmarked digit run in "' + line.trim() + '" gets no constant.numeric scope' +
            ' (got: ' + (scopes.join(' ') || 'none') + ')');
    }

    // The same digit run must not be rescued by some other rule painting it as valid, either.
    const bareLine = '    add r1 10 r3';
    const bare = scopesAt(tokenize, bareLine, bareDigitColumn(bareLine));
    check(!hasScope(bare, 'keyword') && !hasScope(bare, 'variable.language') &&
        !hasScope(bare, 'entity.name'),
        'AC-4: unmarked digit run is not painted valid by any other rule (got: ' +
        (bare.join(' ') || 'none') + ')');

    // ---- AC-5: case sensitivity ----
    check(!hasScope(scopesAt(tokenize, '    MOVE r1 r2', 4), 'keyword.other.mnemonic'),
        'AC-5: uppercase MOVE is not a mnemonic');
    check(!hasScope(scopesAt(tokenize, '    SECTION code', 4), 'keyword.control.directive'),
        'AC-5: uppercase SECTION is not a directive');
    check(!hasScope(scopesAt(tokenize, '    Load r1 r2', 4), 'keyword.other.mnemonic'),
        'AC-5: mixed-case Load is not a mnemonic');
    check(hasScope(scopesAt(tokenize, '    move r1 r2', 4), 'keyword.other.mnemonic'),
        'AC-5: lowercase move IS a mnemonic');
    check(hasScope(scopesAt(tokenize, '    section code', 4), 'keyword.control.directive'),
        'AC-5: lowercase section IS a directive');

    // ---- AC-6: digit separators ----
    // assembler.md "Numeric literals": the comma and the back-tick group digits; the
    // underscore does not, because the underscore is an identifier character.
    const underscore = tokenAt(tokenize, '    data_word #1_000', 17);
    check(!underscore || underscore.text !== '#1_000',
        'AC-6: #1_000 is NOT one numeric token (got: ' +
        (underscore ? JSON.stringify(underscore.text) : 'no token') + ')');

    for (const [source, literal] of [
        ['    data_word #1,000,000', '#1,000,000'],
        ['    data_word $FEDC`BA98`7654`3210', '$FEDC`BA98`7654`3210'],
        ['    data_word %0100`0001', '%0100`0001'],
    ]) {
        const column = source.indexOf(literal);
        const token = tokenAt(tokenize, source, column);
        check(token !== null && token.text === literal && hasScope(token.scopes, 'constant.numeric'),
            'AC-6: ' + literal + ' IS one numeric-literal token (got: ' +
            (token ? JSON.stringify(token.text) : 'no token') + ')');
    }

    // A separator at the end of a literal is a diagnostic, so it must not be swallowed.
    const trailing = tokenAt(tokenize, '    data_word #100,', 17);
    check(trailing !== null && trailing.text === '#100',
        'AC-6: a trailing separator is not part of the literal (got: ' +
        (trailing ? JSON.stringify(trailing.text) : 'no token') + ')');

    // ---- AC-7: the register range and the ABI aliases ----
    for (const reg of ['r0', 'r1', 'r9', 'r10', 'r19', 'r20', 'r29', 'r30', 'r31']) {
        const line = '    move ' + reg + ' r2';
        const token = tokenAt(tokenize, line, 9);
        check(token !== null && token.text === reg &&
            hasScope(token.scopes, 'variable.language.register'),
            'AC-7: ' + reg + ' is a register');
    }
    for (const reg of ['r05', 'r32', 'r33', 'r99', 'r100']) {
        const line = '    move ' + reg + ' r2';
        check(!hasScope(scopesAt(tokenize, line, 9), 'variable.language.register'),
            'AC-7: ' + reg + ' is NOT a register');
    }
    const aliases = ['zero', 'tp', 'a0', 'a1', 'a2', 'a3', 'a4', 'a5', 'a6', 'a7',
        't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7', 't8', 't9',
        's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 's8', 'fp', 'sp', 'ra'];
    const badAliases = aliases.filter(a =>
        !hasScope(scopesAt(tokenize, '    move ' + a + ' r2', 9), 'variable.language.register'));
    check(badAliases.length === 0,
        'AC-7: all 32 ABI aliases tokenize as registers' +
        (badAliases.length ? ' -- missing: ' + badAliases.join(', ') : ''));

    // ---- slices, which exist only because the dot left the identifier alphabet ----
    for (const slice of ['r3.b5', 'r3.q2', 'r3.h1', 'sp.b0']) {
        const line = '    extract.zb ' + slice + ' r7';
        // The width letter and index sit two characters past the dot, which is itself two
        // characters into the slice token.
        const column = line.indexOf(slice) + 3;
        check(hasScope(scopesAt(tokenize, line, column), 'variable.language.slice'),
            'slice: ' + slice + ' carries a slice scope on its index');
        check(hasScope(scopesAt(tokenize, line, line.indexOf(slice)),
            'variable.language.register'),
            'slice: ' + slice + ' carries a register scope on its register');
    }

    // No column of `foo.b5` may be painted as a register or a slice. The dot is not in the
    // identifier alphabet, so this is not source with a legal reading the grammar is missing.
    const dotted = '    move foo.b5 r2';
    const dottedBad = [];
    for (let c = dotted.indexOf('foo'); c < dotted.indexOf('foo') + 6; c++) {
        if (hasScope(scopesAt(tokenize, dotted, c), 'variable.language')) { dottedBad.push(c); }
    }
    check(dottedBad.length === 0,
        'slice: no column of a dotted bare identifier is painted as a register or slice' +
        (dottedBad.length ? ' -- columns ' + dottedBad.join(', ') : ''));

    for (const bad of ['r3.b8', 'r3.q4', 'r3.h2']) {
        const line = '    move ' + bad + ' r2';
        check(!hasScope(scopesAt(tokenize, line, line.indexOf(bad) + 3),
            'variable.language.slice'),
            'slice: out-of-range index in ' + bad + ' is not painted as a slice');
    }

    // ---- escapes: the eight the spec allows, and nothing else ----
    check(hasScope(scopesAt(tokenize, '    data_string "a\\nb"', 19),
        'constant.character.escape'),
        'escape: \\n is a legal escape');
    check(hasScope(scopesAt(tokenize, '    data_string "a\\x41b"', 19),
        'constant.character.escape'),
        'escape: \\xHH is a legal escape');
    check(hasScope(scopesAt(tokenize, '    data_string "a\\zb"', 19), 'invalid.illegal'),
        'escape: \\z is flagged illegal rather than painted as an escape');
    check(hasScope(scopesAt(tokenize, '    data_string "a\\x4gb"', 19), 'invalid.illegal'),
        'escape: \\x with a non-hex digit is flagged illegal');

    return results;
}

/* ---------------- Part 2b: the real fixtures highlight (AC-3) ---------------- */

function fixtureChecks(tokenize) {
    // The three sources between them carry every token class AC-3 names.
    const hello = fs.readFileSync(path.join(REPO, 'asm', 'v2', 'hello.mzasm'), 'utf8');
    const devices = fs.readFileSync(path.join(REPO, 'asm', 'v2', 'devices.mzasm'), 'utf8');
    const showcase = fs.readFileSync(path.join(FIXTURES, 'highlight_showcase.mzasm'), 'utf8');

    const scopes = new Set([
        ...allScopes(tokenize, hello),
        ...allScopes(tokenize, devices),
        ...allScopes(tokenize, showcase),
    ]);

    const required = [
        ['keyword.other.mnemonic.mzasm', 'mnemonics'],
        ['keyword.control.directive.mzasm', 'directives'],
        ['constant.language.section-kind.mzasm', 'section-kind words'],
        ['variable.language.register.mzasm', 'registers'],
        ['variable.language.slice.mzasm', 'register slices'],
        ['constant.numeric.hex.mzasm', 'hexadecimal literals'],
        ['constant.numeric.decimal.mzasm', 'decimal literals'],
        ['constant.numeric.binary.mzasm', 'binary literals'],
        ['entity.name.label.mzasm', 'labels'],
        ['comment.line.semicolon.mzasm', 'comments'],
        ['string.quoted.double.mzasm', 'strings'],
        ['string.quoted.single.mzasm', 'character literals'],
        ['keyword.operator.address.mzasm', 'the address operator'],
        ['constant.language.here.mzasm', 'the reserved identifier here'],
    ];

    const results = [];
    for (const [scope, what] of required) {
        results.push({
            label: 'AC-3: real v2 sources highlight ' + what + ' (' + scope + ')',
            pass: scopes.has(scope),
        });
    }
    return results;
}

/* ---------------- Part 2c: the annotated syntax-test fixture ---------------- */

/* tests/mzasm.test.mzasm carries its expectations inline, in the vscode-tmgrammar-test
   convention: a comment line of carets asserts the scope of the columns beneath them on the
   preceding source line, and `<-` asserts column zero. Reading the file is how those
   annotations stop being decoration. */
function annotatedChecks(tokenize) {
    const text = fs.readFileSync(path.join(__dirname, 'mzasm.test.mzasm'), 'utf8');
    const lines = text.split(/\r\n|\r|\n/);
    const tokens = tokenize(text);
    const results = [];
    let lastSource = -1;

    for (let i = 0; i < lines.length; i++) {
        const annotation = /^(\s*;\s*)(<-|\^+)\s+(\S+)\s*$/.exec(lines[i]);

        if (!annotation) {
            if (lines[i].trim() !== '' && !/^\s*;/.test(lines[i])) {
                lastSource = i;
            }
            continue;
        }

        if (lastSource < 0) {
            results.push({ label: 'syntax-test line ' + (i + 1) + ': no source line above it', pass: false });
            continue;
        }

        const expected = annotation[3];
        const columns = annotation[2] === '<-'
            ? [0]
            : Array.from({ length: annotation[2].length }, (_, k) => annotation[1].length + k);

        for (const column of columns) {
            const hit = tokens[lastSource].find(t => t.startIndex <= column && column < t.endIndex);
            const got = hit ? hit.scopes : [];
            results.push({
                label: 'syntax-test ' + path.basename('mzasm.test.mzasm') + ':' + (lastSource + 1) +
                    ' col ' + column + ' is ' + expected,
                pass: got.includes(expected),
            });
        }
    }

    results.push({
        label: 'syntax-test: the annotated fixture actually carried assertions',
        pass: results.length > 0,
    });
    return results;
}

/* ---------------- Part 3: prove the negative checks can fail ---------------- */

/* A grammar that paints everything. Any suite whose assertions all pass against THIS is a
   suite that is not testing anything. */
function permissiveGrammar() {
    const real = JSON.parse(fs.readFileSync(GRAMMAR_PATH, 'utf8'));
    const broken = JSON.parse(JSON.stringify(real));

    // Bases become optional, which is exactly the silent approximation the mandatory-base
    // rule exists to prevent.
    broken.repository['dec-literal'].match = '[$#%]?[+-]?[0-9][0-9,`_]*';
    // Case stops mattering, and any lowercase-ish word becomes a mnemonic.
    broken.repository.mnemonic.match = '(?i)\\b[a-z][a-z_.0-9]*\\b';
    // Register numbers stop being bounded.
    broken.repository.register.match =
        '(?i)\\b(?:r[0-9]+|zero|tp|a[0-9]|t[0-9]|s[0-9]|fp|sp|ra)\\b(?:(\\.)([a-z][0-9]))?';
    // The mnemonic rule now outranks everything word-shaped, so it has to be tried first for
    // the over-permissiveness to actually show up in the output.
    broken.patterns = [
        { include: '#comment' },
        { include: '#string' },
        { include: '#character' },
        { include: '#dec-literal' },
        { include: '#register' },
        { include: '#mnemonic' },
    ];
    return broken;
}

/* ---------------- run ---------------- */

(async () => {
    testAgainstAppendix();

    const grammar = JSON.parse(fs.readFileSync(GRAMMAR_PATH, 'utf8'));

    // AC-5's structural half: no rule may carry a case-insensitive flag.
    const flagged = Object.entries(grammar.repository)
        .filter(([, rule]) => JSON.stringify(rule).includes('(?i)'))
        .map(([name]) => name);
    ok(flagged.length === 0,
        'AC-5: no grammar rule carries a (?i) flag' +
        (flagged.length ? ' -- found in: ' + flagged.join(', ') : ''));

    const tokenize = await makeTokenizer(grammar);

    for (const r of scopeChecks(tokenize)) { ok(r.pass, r.label); }
    for (const r of fixtureChecks(tokenize)) { ok(r.pass, r.label); }
    for (const r of annotatedChecks(tokenize)) { ok(r.pass, r.label); }

    // Part 3. The negative assertions are re-run against a grammar built to violate them, and
    // the run is required to produce failures. If a future edit turned one of them into a
    // tautology, it would pass here too, and this check would go red.
    const permissive = await makeTokenizer(permissiveGrammar());
    const broken = scopeChecks(permissive);
    const brokenFailures = broken.filter(r => !r.pass);

    ok(brokenFailures.length > 0,
        'armed: the scope assertions detect an over-permissive grammar (' +
        brokenFailures.length + ' of ' + broken.length + ' went red)');

    // Name the specific negatives that must bite, so a check going toothless is caught
    // individually rather than hidden behind the aggregate count above.
    for (const fragment of [
        'AC-4: unmarked digit run in "add r1 10 r3"',
        'AC-5: uppercase MOVE is not a mnemonic',
        'AC-7: r32 is NOT a register',
    ]) {
        ok(brokenFailures.some(r => r.label.startsWith(fragment)),
            'armed: "' + fragment + '" fails against the over-permissive grammar');
    }

    console.log(failures === 0
        ? '\nALL GRAMMAR CHECKS PASSED'
        : '\n' + failures + ' GRAMMAR CHECK(S) FAILED');
    process.exit(failures === 0 ? 0 : 1);
})().catch((e) => {
    console.error(e);
    process.exit(2);
});
