// Maize v2 assembly language client (maize-46, rewritten for v2 by maize-429). Starts the
// server in server/server.js for .mzasm documents; all language smarts live server-side.

'use strict';

const path = require('path');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;

function activate(context) {
    const serverModule = context.asAbsolutePath(path.join('server', 'server.js'));

    const serverOptions = {
        run: { module: serverModule, transport: TransportKind.ipc },
        debug: {
            module: serverModule,
            transport: TransportKind.ipc,
            options: { execArgv: ['--nolazy', '--inspect=6009'] },
        },
    };

    const clientOptions = {
        documentSelector: [{ scheme: 'file', language: 'mzasm' }],
        synchronize: { configurationSection: 'maize' },
    };

    client = new LanguageClient('mzasm', 'Maize', serverOptions, clientOptions);
    client.start();
}

function deactivate() {
    return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
