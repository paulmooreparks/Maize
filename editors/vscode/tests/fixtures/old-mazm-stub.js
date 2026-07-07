// Faithful stand-in for a pre-maize-49 mazm (version-skew fallback test).
//
// Mimics the committed maize-46 argv semantics exactly: bare `--flags` are
// ignored, and the FIRST bare token becomes the input file. So when the server
// passes `--check --stdin --base-path <dir> --source-name <path>`, this stub
// captures <dir> (a directory) as its input, fails to read it, and exits 0
// with no output, just like the real old binary's empty-tree trivial success.
// It never reads stdin and can never produce the mazm-stdin-probe marker.
//
// In file mode (`--check <file>`) it emulates just enough assembly checking
// for the fallback test: a file containing "LD nolabel" gets the real fatal
// line shape on stderr with exit 1; anything else passes.

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
    // Directory or unreadable: old mazm's failed open yields an empty token
    // tree, which compiles trivially. Exit 0, no output.
    process.exit(0);
}

const lines = text.split(/\r?\n/);
const badLine = lines.findIndex(l => /\bLD\s+nolabel\b/.test(l));

if (badLine >= 0) {
    process.stderr.write(
        `mazm: ${inputFile}:${badLine + 1}: error: LD reads from a memory address; ` +
        "prefix the source with '@', or use CP for a value\n");
    process.exit(1);
}

process.exit(0);
