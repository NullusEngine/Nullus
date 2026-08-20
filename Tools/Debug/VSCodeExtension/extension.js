'use strict';

const fs = require('fs');
const path = require('path');
const cp = require('child_process');
const vscode = require('vscode');

function readJson(filePath) {
    return JSON.parse(fs.readFileSync(filePath, 'utf8'));
}

function isAbsolutePath(value) {
    return typeof value === 'string' && path.isAbsolute(value);
}

function findManifestFrom(startPath) {
    if (!startPath) {
        return undefined;
    }
    let current = fs.existsSync(startPath) && fs.statSync(startPath).isFile()
        ? path.dirname(startPath)
        : startPath;
    for (;;) {
        const candidate = path.join(current, 'Library', 'IDE', 'Nullus.Debug.json');
        if (fs.existsSync(candidate)) {
            return candidate;
        }
        const parent = path.dirname(current);
        if (parent === current) {
            return undefined;
        }
        current = parent;
    }
}

function resolveManifest() {
    const configured = vscode.workspace.getConfiguration('nullus').get('projectManifest');
    if (typeof configured === 'string' && configured.length > 0) {
        const candidate = isAbsolutePath(configured)
            ? configured
            : path.resolve(vscode.workspace.workspaceFolders?.[0]?.uri.fsPath || process.cwd(), configured);
        if (fs.existsSync(candidate)) {
            return candidate;
        }
    }

    if (vscode.workspace.workspaceFile) {
        const workspaceDirectory = path.dirname(vscode.workspace.workspaceFile.fsPath);
        const fromWorkspace = path.join(workspaceDirectory, '..', 'Nullus.Debug.json');
        if (fs.existsSync(fromWorkspace)) {
            return path.normalize(fromWorkspace);
        }
    }

    for (const folder of vscode.workspace.workspaceFolders || []) {
        const found = findManifestFrom(folder.uri.fsPath);
        if (found) {
            return found;
        }
    }
    return findManifestFrom(vscode.window.activeTextEditor?.document.uri.fsPath);
}

function runBroker(manifestPath) {
    let manifest;
    try {
        manifest = readJson(manifestPath);
    } catch (error) {
        throw new Error(`Nullus debug manifest is invalid: ${manifestPath}\n${error.message}`);
    }
    if (!isAbsolutePath(manifest.brokerExecutable) || !fs.existsSync(manifest.brokerExecutable)) {
        throw new Error(`NullusDebugBroker is missing from the project manifest: ${manifestPath}`);
    }

    return new Promise((resolve, reject) => {
        const child = cp.spawn(
            manifest.brokerExecutable,
            ['--manifest', manifestPath, '--timeout-ms', '120000'],
            {
                cwd: manifest.projectRoot,
                windowsHide: true,
                shell: false
            });
        let stdout = '';
        let stderr = '';
        child.stdout.setEncoding('utf8');
        child.stderr.setEncoding('utf8');
        child.stdout.on('data', chunk => { stdout += chunk; });
        child.stderr.on('data', chunk => { stderr += chunk; });
        child.on('error', error => reject(new Error(`Unable to run NullusDebugBroker: ${error.message}`)));
        child.on('close', code => {
            stdout = stdout.trim();
            if (code !== 0) {
                const detail = (stderr || stdout || `exit code ${code}`).trim();
                reject(new Error(`Nullus F5 preparation failed: ${detail}`));
                return;
            }
            let response;
            try {
                response = JSON.parse(stdout.split(/\r?\n/).filter(Boolean).pop() || '');
            } catch (error) {
                reject(new Error(`NullusDebugBroker returned invalid JSON.\n${stdout}`));
                return;
            }
            const data = response && response.data && typeof response.data === 'object'
                ? Object.assign({}, response, response.data)
                : response;
            if (!Number.isInteger(data.processId) || data.processId <= 0 || !data.managedReady) {
                reject(new Error(data.message || response.error || 'Nullus Editor is not ready for managed debugging.'));
                return;
            }
            resolve(data.processId);
        });
    });
}

async function resolveEditorProcess() {
    const manifestPath = resolveManifest();
    if (!manifestPath) {
        throw new Error('No project-scoped Nullus.Debug.json was found. Open the project Library/IDE workspace first.');
    }
    try {
        return await runBroker(manifestPath);
    } catch (error) {
        vscode.window.showErrorMessage(error.message);
        throw error;
    }
}

function activate(context) {
    context.subscriptions.push(
        vscode.commands.registerCommand('nullus.resolveEditorProcess', resolveEditorProcess),
        vscode.commands.registerCommand('nullus.openDebugWorkspace', async () => {
            const manifestPath = resolveManifest();
            if (!manifestPath) {
                vscode.window.showErrorMessage('No Nullus project debug manifest was found.');
                return;
            }
            const manifest = readJson(manifestPath);
            const workspace = vscode.Uri.file(manifest.visualStudioCodeWorkspace);
            await vscode.commands.executeCommand('vscode.openFolder', workspace, false);
        }));
}

function deactivate() {
    // The Broker and Editor own their lifetimes. The extension does not leave
    // sockets, polling timers, or processes behind when VS Code closes.
}

module.exports = { activate, deactivate };
