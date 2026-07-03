import * as fs from 'fs';
import * as path from 'path';
import {
  commands,
  StatusBarAlignment,
  window,
  workspace,
  type ExtensionContext,
  type OutputChannel,
  type StatusBarItem,
} from 'vscode';
import {
  LanguageClient,
  type LanguageClientOptions,
  RevealOutputChannelOn,
  type ServerOptions,
  TransportKind,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;
let output: OutputChannel | undefined;
let statusBar: StatusBarItem | undefined;

function tryExecutable(candidate: string): string | undefined {
  if (fs.existsSync(candidate)) {
    return candidate;
  }
  if (process.platform === 'win32' && fs.existsSync(`${candidate}.exe`)) {
    return `${candidate}.exe`;
  }
  return undefined;
}

function resolveServerCommand(configured: string, extensionPath: string): string {
  const candidates: string[] = [];

  if (configured && configured !== 'kinglet-lsp') {
    candidates.push(configured);
    if (!path.isAbsolute(configured)) {
      candidates.push(path.join(extensionPath, configured));
    }
  }

  candidates.push(path.join(extensionPath, 'bin', 'kinglet-lsp'));
  if (process.platform === 'darwin') {
    candidates.push(path.join(extensionPath, 'bin', 'kinglet-lsp-darwin'));
  }
  candidates.push(path.join(extensionPath, 'out', 'Default', 'kinglet-lsp'));
  candidates.push(path.join(extensionPath, 'kinglet-lsp'));

  for (const candidate of candidates) {
    const resolved = tryExecutable(candidate);
    if (resolved) {
      return resolved;
    }
  }

  return configured || 'kinglet-lsp';
}

function setStatus(text: string, tooltip: string): void {
  if (!statusBar) {
    return;
  }
  statusBar.text = text;
  statusBar.tooltip = tooltip;
}

function log(line: string): void {
  output?.appendLine(line);
  console.log(`[Perch] ${line}`);
}

function startLanguageServer(context: ExtensionContext, lspPath: string): void {
  if (client) {
    void client.stop().then(() => {
      client = undefined;
      startLanguageServer(context, lspPath);
    });
    return;
  }

  const serverOptions: ServerOptions = {
    run: {
      command: lspPath,
      args: [],
      transport: TransportKind.stdio,
      options: { cwd: path.dirname(lspPath) },
    },
    debug: {
      command: lspPath,
      args: [],
      transport: TransportKind.stdio,
      options: { cwd: path.dirname(lspPath) },
    },
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [
      { scheme: 'file', language: 'kinglet' },
      { scheme: 'file', language: 'kinglet-nest' },
    ],
    synchronize: {
      fileEvents: workspace.createFileSystemWatcher('**/kinglet.{toml,nest}'),
    },
    outputChannel: output,
    revealOutputChannelOn: RevealOutputChannelOn.Error,
  };

  client = new LanguageClient('perch', 'Perch', serverOptions, clientOptions);
  context.subscriptions.push({ dispose: () => void client?.stop() });

  client
    .start()
    .then(() => {
      setStatus('$(check) Perch', `Kinglet language server running\n${lspPath}`);
      log('Language server started.');
    })
    .catch((err: unknown) => {
      const message = err instanceof Error ? err.message : String(err);
      setStatus('$(error) Perch', `Failed to start Kinglet language server\n${message}`);
      log(`Failed to start language server: ${message}`);
      void window.showErrorMessage(`Perch: Kinglet language server failed to start: ${message}`);
      output?.show(true);
    });
}

export function activate(context: ExtensionContext): void {
  output = window.createOutputChannel('Perch');
  context.subscriptions.push(output);

  statusBar = window.createStatusBarItem(StatusBarAlignment.Right, 100);
  statusBar.command = 'kinglet.showServerLog';
  statusBar.show();
  context.subscriptions.push(statusBar);

  context.subscriptions.push(
    commands.registerCommand('kinglet.showServerLog', () => {
      output?.show(true);
    }),
    commands.registerCommand('kinglet.restartServer', () => {
      const configured = workspace.getConfiguration('kinglet').get<string>('lspPath', 'kinglet-lsp');
      const lspPath = resolveServerCommand(configured, context.extensionPath);
      log('Restarting language server...');
      startLanguageServer(context, lspPath);
    }),
  );

  setStatus('$(sync~spin) Perch', 'Perch loading…');
  log(`Extension activated (version ${context.extension.packageJSON.version}).`);
  log(`Extension path: ${context.extensionPath}`);

  const configured = workspace.getConfiguration('kinglet').get<string>('lspPath', 'kinglet-lsp');
  const lspPath = resolveServerCommand(configured, context.extensionPath);
  log(`Resolved language server: ${lspPath}`);

  if (!tryExecutable(lspPath)) {
    const msg =
      `Kinglet language server not found at "${lspPath}". ` +
      'Set kinglet.lspPath or reinstall the Perch VSIX built with npm run package.';
    setStatus('$(warning) Perch', msg);
    log(msg);
    void window.showWarningMessage(msg);
    output.show(true);
    return;
  }

  // Defer LSP spawn so activate() returns immediately (avoid stuck "activating").
  setImmediate(() => startLanguageServer(context, lspPath));
}

export function deactivate(): Thenable<void> | undefined {
  statusBar?.dispose();
  return client?.stop();
}
