// Stand-in for an assembler that does not support --stdin (version-skew fallback test).
//
// Kept deliberately, and the reasoning is worth stating because the obvious call is to delete
// it. mzasm is a from-scratch v2 binary that has supported --stdin since its first commit, so
// there is no historical build in the wild this fixture protects a user from. What it protects
// is the SERVER's degrade path, which is still in server.js: ensureMode probes, and on anything
// other than exit 1 plus the marker it falls back to save-time diagnostics. That code runs on
// any machine whose configured path points at something unexpected, and deleting the only test
// that exercises it would leave a live branch with no coverage. The fixture is artificial; the
// branch it covers is not.
//
// It mimics the argv semantics of an assembler that ignores unknown --flags and takes the
// first bare token as its input file, which is exactly what mzasm's own --help promises for
// forward compatibility. So when the server passes
// `--check --stdin --base-path <dir> --source-name <path>`, this stub captures <dir> (a
// directory) as its input, fails to read it, and exits 0 with no output. It never reads stdin
// and can never produce the mzasm-stdin-probe marker.
//
// In file mode (`--check <file>`) it emulates just enough checking for the fallback test: a
// file containing an unrecognized mnemonic gets the real fatal line shape on stderr with
// exit 1, and anything else passes.

'use strict';

const fs = require('fs');

let inputFile = '';

for (const arg of process.argv.slice(2)) {
    if (arg.startsWith('--')) {
        continue;
    }

    if (!inputFile) {
        inputFile = arg;
    }
}

if (!inputFile) {
    process.exit(0);
}

let text;

try {
    text = fs.readFileSync(inputFile, 'utf8');
}
catch {
    // Directory or unreadable: a failed open yields an empty token tree, which assembles
    // trivially. Exit 0, no output.
    process.exit(0);
}

const lines = text.split(/\r?\n/);
const badLine = lines.findIndex(l => /\bno_such_instruction\b/.test(l));

if (badLine >= 0) {
    process.stderr.write(
        `mzasm: ${inputFile}:${badLine + 1}: error: ` +
        "'no_such_instruction' is not a mnemonic or a directive\n");
    process.exit(1);
}

process.exit(0);
