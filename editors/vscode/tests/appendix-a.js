// appendix-a.js (maize-429): the mnemonic set of Maize v2, read from the specification.
//
// This is a Node port of the parsing approach tests/v2/appendix_a.cpp (maize-422) already
// implements against the same file, and it exists for the same reason that one does. The
// grammar in syntaxes/mzasm.tmLanguage.json carries 187 mnemonic spellings, and a list that
// large cannot be trusted to a transcription: two hand-copied lists checking each other agree
// on every error they share. So the check has to originate from the appendix itself, and this
// module is the thing that reads it. It deliberately reads NOTHING else. Not mnemonic_v2.h,
// not the opcode table in the assembler, not the grammar it will be compared against.
//
// What is ported, and why each part matters:
//
//   - Tables are classified by the NAMES of their header columns, never by position, so a
//     column inserted into one band's table shifts nothing. A.9 already carries an extra
//     `Slots` column that the other bands do not.
//   - Four row shapes are dispatched (band summary, opcode band, escape roles, and the
//     unknown case). An unrecognized shape is REPORTED rather than skipped, because a table
//     nobody parses is indistinguishable from a table with nothing in it.
//   - Cell contents are unwrapped from the back-ticks the appendix uses throughout, so a
//     comparison is against the value and not against the presentation.
//   - $FF is stated in A.14's prose with no table row at all, so `breakpoint` is declared
//     here. That is a hand transcription and it is admitted as one; it is one mnemonic with
//     no operands, and the count assertion downstream cannot let it be quietly dropped.

'use strict';

const fs = require('fs');

/* Markdown cells wrap their content in back-ticks throughout this appendix. */
function stripTicks(text) {
    return text.replace(/`/g, '').trim();
}

/* A markdown row opens and closes with a pipe, so the split leaves an empty cell at each end
   and both are dropped. */
function splitRow(line) {
    const cells = line.split('|');
    cells.shift();
    cells.pop();
    return cells.map(c => c.trim());
}

function isSeparatorRow(line) {
    return /^[|:\-\s]+$/.test(line) && line.includes('-');
}

/* One table's header row, reduced to a set of column names. Every read goes through this, so
   A.9's inserted sixth column shifts nothing. */
function classify(columns) {
    const has = (name) => columns.includes(name);

    if (has('Range') && has('Family') && has('Assigned') && has('Reserved')) {
        return 'band-summary';
    }
    if (has('Byte') && has('Mnemonic') && has('Operands') && has('Form') && has('Len')) {
        return 'opcode-band';   // with or without A.9's Slots column
    }
    if (has('Byte') && has('Role') && columns.length === 2) {
        return 'escape-roles';
    }
    return 'unknown';
}

/* Parse the appendix. Returns { mnemonics, assignedRows, statedAssigned, errors }.

   `mnemonics` is the set of Assigned mnemonic spellings in first-appearance order. It is
   SHORTER than the number of assigned opcodes, and deliberately so: several operations carry
   a register form and an immediate form at two different bytes under one spelling, and a
   grammar highlights spellings rather than bytes.

   `assignedRows` and `statedAssigned` are the completeness check, and they are the reason a
   dropped table cannot pass unnoticed. The first counts the assigned rows this parse actually
   walked; the second is the total A.1's own prose states. Two independent statements inside
   the appendix, compared against each other. A band table that stopped being parsed (a header
   reworded, a shape unrecognized) moves the first and not the second.

   `errors` is every complaint the parse accumulated. A caller that ignores it gets a silently
   short list, which is the exact failure this module exists to prevent, so the caller is
   expected to fail on any entry. */
function parseAppendix(specPath) {
    const errors = [];
    let text;

    try {
        text = fs.readFileSync(specPath, 'utf8');
    }
    catch (e) {
        return { mnemonics: [], errors: ['cannot read ' + specPath + ': ' + e.message] };
    }

    const seen = new Set();
    const mnemonics = [];
    const claim = (mnemonic) => {
        if (!seen.has(mnemonic)) {
            seen.add(mnemonic);
            mnemonics.push(mnemonic);
        }
    };

    let section = '(preamble)';
    let columns = null;
    let expectingSeparator = false;
    let assignedRows = 0;
    let statedAssigned = -1;

    for (const raw of text.split(/\r\n|\r|\n/)) {
        // A.1's closing prose states the totals independently of every band table above it.
        const totals = /^The totals are (\d+) assigned instruction opcodes\b/.exec(raw.trim());
        if (totals) {
            statedAssigned = parseInt(totals[1], 10);
        }

        if (raw.startsWith('## ')) {
            section = raw.slice(3).trim();
            columns = null;
            continue;
        }

        const trimmed = raw.trim();

        if (trimmed === '' || trimmed[0] !== '|') {
            columns = null;
            continue;
        }

        if (columns === null) {
            columns = splitRow(trimmed).map(stripTicks);
            expectingSeparator = true;

            if (classify(columns) === 'unknown') {
                errors.push(
                    'an unrecognized table shape in ' + section + ': columns are ' +
                    columns.map(c => '[' + c + ']').join(' ') +
                    '. Three shapes are known (band summary, opcode band with or without ' +
                    'Slots, escape roles); a fourth needs handling rather than skipping');
            }
            continue;
        }

        if (expectingSeparator) {
            expectingSeparator = false;
            if (isSeparatorRow(trimmed)) {
                continue;
            }
            // Not a separator, so this table had none and the row above was data rather than
            // a header. Say so instead of silently treating data as column names.
            errors.push('a table in ' + section + ' has no separator row');
            continue;
        }

        if (classify(columns) !== 'opcode-band') {
            continue;   // band summaries and escape-role tables carry no mnemonic
        }

        const cells = splitRow(trimmed).map(stripTicks);
        const mnemonicColumn = columns.indexOf('Mnemonic');
        const byteColumn = columns.indexOf('Byte');

        if (cells.length !== columns.length) {
            errors.push(
                'a row in ' + section + ' has ' + cells.length + ' cells and its header has ' +
                columns.length);
            continue;
        }

        const mnemonic = cells[mnemonicColumn];

        if (mnemonic === 'reserved' || mnemonic === '') {
            continue;
        }

        // An assigned row names exactly one byte. A range means a reserved run, and a range
        // carrying a mnemonic means the appendix says something this parser does not model.
        if (cells[byteColumn].includes('..')) {
            errors.push(
                'an assigned row in ' + section + " spans a range of bytes: '" +
                cells[byteColumn] + "'");
            continue;
        }

        assignedRows++;
        claim(mnemonic);
    }

    // A.14 gives $FF as `breakpoint` in prose with no table row at all. See the header comment:
    // this one spelling is declared rather than parsed, and the count assertion below is what
    // keeps the declaration from being quietly dropped.
    claim('breakpoint');
    assignedRows++;

    if (statedAssigned < 0) {
        errors.push(
            "A.1's prose statement of the totals ('The totals are N assigned instruction " +
            "opcodes') was not found, so the parse has nothing to check its own completeness " +
            'against');
    }
    else if (assignedRows !== statedAssigned) {
        errors.push(
            'this parse walked ' + assignedRows + ' assigned opcode rows and A.1 states ' +
            statedAssigned + '. A band table is being missed, or the appendix disagrees with ' +
            'itself');
    }

    return { mnemonics, assignedRows, statedAssigned, errors };
}

module.exports = { parseAppendix };
