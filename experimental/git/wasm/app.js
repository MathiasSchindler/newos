import { createGitWasm } from './gitwasm.js';
import { detectLanguage, highlightSyntax } from './syntax.js';

const elements = {
  status: document.querySelector('#status'),
  explorer: document.querySelector('#explorer'),
  changes: document.querySelector('#changes'),
  tabs: document.querySelector('#tabs'),
  editorTitle: document.querySelector('#editorTitle'),
  highlight: document.querySelector('#highlight'),
  editor: document.querySelector('#editor'),
  output: document.querySelector('#output'),
  history: document.querySelector('#history'),
  commitMessage: document.querySelector('#commitMessage'),
  remoteUrl: document.querySelector('#remoteUrl'),
  command: document.querySelector('#command'),
  includeGit: document.querySelector('#includeGit'),
};

const state = {
  git: null,
  activePath: null,
  tabs: new Map(),
  files: [],
  statusRows: [],
  historyRows: [],
  collapsedDirs: new Set(),
};

function quoteArg(text) {
  return `'${String(text).replaceAll("'", "'\\''")}'`;
}

function normalizePath(path) {
  return path.startsWith('/repo/') ? path.slice('/repo/'.length) : path.replace(/^\/+/, '');
}

function fullPath(path) {
  const normalized = normalizePath(path);
  return `/repo/${normalized}`;
}

function print(title, result) {
  const stderr = result.stderr ? `\n[stderr]\n${result.stderr}` : '';
  elements.output.textContent = `$ git ${title}\nexit ${result.status}\n\n${result.stdout}${stderr}`;
  return result;
}

function syncHighlight() {
  const tab = state.activePath ? state.tabs.get(state.activePath) : null;
  const text = tab ? elements.editor.value : '';
  elements.highlight.innerHTML = highlightSyntax(tab?.path || '', text);
  elements.highlight.dataset.language = tab ? detectLanguage(tab.path) : 'plain';
  elements.highlight.scrollTop = elements.editor.scrollTop;
  elements.highlight.scrollLeft = elements.editor.scrollLeft;
}

function run(commandLine) {
  saveActive({ quiet: true });
  const result = state.git.run(commandLine);
  print(commandLine, result);
  refreshAll();
  return result;
}

function parseStatus(text) {
  return text.split('\n').filter(Boolean).map((line) => {
    const code = line.slice(0, 2);
    const path = line.slice(3).trim();
    return { code, path };
  });
}

function refreshStatus() {
  const result = state.git.run('status --short');
  state.statusRows = parseStatus(result.stdout);
  renderChanges();
  elements.status.textContent = result.status === 0 ? 'ready' : 'status failed';
}

function parseHistory(text) {
  return text.split('\n').filter(Boolean).map((line) => {
    const parts = line.split('|');
    return {
      shortOid: parts[0] || '',
      oid: parts[1] || '',
      author: parts[2] || '',
      subject: parts.slice(3).join('|') || '',
    };
  });
}

function refreshHistory() {
  const result = state.git.run('log --format=%h|%H|%an|%s -n 40');
  state.historyRows = result.status === 0 ? parseHistory(result.stdout) : [];
  renderHistory();
}

function refreshFiles() {
  state.files = state.git.listFiles({ includeGit: elements.includeGit.checked });
  renderExplorer();
}

function refreshAll() {
  refreshStatus();
  refreshFiles();
  refreshHistory();
  renderTabs();
}

function openFile(path) {
  const normalized = normalizePath(path);
  expandPathParents(normalized);
  let tab = state.tabs.get(normalized);
  if (!tab) {
    const content = state.git.readFile(fullPath(normalized));
    tab = { path: normalized, content: content ?? '', savedContent: content ?? '', dirty: false };
    state.tabs.set(normalized, tab);
  }
  state.activePath = normalized;
  elements.editor.value = tab.content;
  elements.editor.disabled = false;
  syncHighlight();
  renderTabs();
}

function closeFile(path) {
  const normalized = normalizePath(path);
  state.tabs.delete(normalized);
  if (state.activePath === normalized) {
    state.activePath = state.tabs.size ? state.tabs.keys().next().value : null;
    if (state.activePath) {
      elements.editor.value = state.tabs.get(state.activePath).content;
      elements.editor.disabled = false;
    } else {
      elements.editor.value = '';
      elements.editor.disabled = true;
    }
    syncHighlight();
  }
  renderTabs();
}

function saveActive({ quiet = false } = {}) {
  if (!state.activePath) return;
  const tab = state.tabs.get(state.activePath);
  if (!tab) return;
  tab.content = elements.editor.value;
  state.git.writeFile(fullPath(tab.path), tab.content);
  tab.savedContent = tab.content;
  tab.dirty = false;
  if (!quiet) elements.output.textContent = `saved /repo/${tab.path}`;
  syncHighlight();
  refreshStatus();
  refreshFiles();
  renderTabs();
}

function saveAll() {
  for (const tab of state.tabs.values()) {
    state.git.writeFile(fullPath(tab.path), tab.content);
    tab.savedContent = tab.content;
    tab.dirty = false;
  }
  elements.output.textContent = 'saved all open files';
  refreshAll();
}

function createFile() {
  const name = window.prompt('Path under /repo');
  if (!name) return;
  const path = normalizePath(name);
  state.git.writeFile(fullPath(path), '');
  openFile(path);
  refreshAll();
}

function deleteActive() {
  if (!state.activePath) return;
  const path = state.activePath;
  if (!window.confirm(`Delete /repo/${path}?`)) return;
  state.git.deletePath(fullPath(path));
  closeFile(path);
  refreshAll();
}

function fileStatus(path) {
  const row = state.statusRows.find((entry) => entry.path === path);
  return row ? row.code.trim() || 'M' : '';
}

function directoryStatus(path) {
  const prefix = `${path}/`;
  return state.statusRows.some((entry) => entry.path === path || entry.path.startsWith(prefix)) ? '*' : '';
}

function expandPathParents(path) {
  const parts = normalizePath(path).split('/').filter(Boolean);
  let current = '';
  for (let index = 0; index + 1 < parts.length; index += 1) {
    current = current ? `${current}/${parts[index]}` : parts[index];
    state.collapsedDirs.delete(current);
  }
}

function buildExplorerTree() {
  const root = { path: '', name: '', type: 'd', children: new Map() };

  function ensureDir(path) {
    const parts = normalizePath(path).split('/').filter(Boolean);
    let node = root;
    let current = '';
    for (const part of parts) {
      current = current ? `${current}/${part}` : part;
      let child = node.children.get(part);
      if (!child || child.type !== 'd') {
        child = { path: current, name: part, type: 'd', children: new Map() };
        node.children.set(part, child);
      }
      node = child;
    }
    return node;
  }

  for (const entry of state.files) {
    const path = normalizePath(entry.path);
    if (!path) continue;
    if (entry.type === 'd') {
      ensureDir(path);
      continue;
    }
    const parts = path.split('/').filter(Boolean);
    const name = parts.pop();
    const parent = ensureDir(parts.join('/'));
    parent.children.set(name, { path, name, type: 'f' });
  }

  const rows = [];
  function visit(node, depth) {
    const children = [...node.children.values()].sort((left, right) => {
      if (left.type !== right.type) return left.type === 'd' ? -1 : 1;
      return left.name.localeCompare(right.name);
    });
    for (const child of children) {
      rows.push({ node: child, depth });
      if (child.type === 'd' && !state.collapsedDirs.has(child.path)) {
        visit(child, depth + 1);
      }
    }
  }

  visit(root, 0);
  return rows;
}

function renderExplorer() {
  elements.explorer.textContent = '';
  const visible = buildExplorerTree();
  if (!visible.some((entry) => entry.node.type === 'f')) {
    const empty = document.createElement('div');
    empty.className = 'empty';
    empty.textContent = 'No files';
    elements.explorer.append(empty);
    return;
  }
  for (const entry of visible) {
    const node = entry.node;
    const isDirectory = node.type === 'd';
    const collapsed = isDirectory && state.collapsedDirs.has(node.path);
    const row = document.createElement('button');
    row.className = `tree-row ${isDirectory ? 'directory' : 'file'}${state.activePath === node.path ? ' active' : ''}`;
    row.style.setProperty('--depth', entry.depth);
    row.type = 'button';
    row.dataset.path = node.path;
    row.innerHTML = '<span class="tree-caret"></span><span class="file-mark"></span><span class="file-name"></span>';
    row.querySelector('.tree-caret').textContent = isDirectory ? (collapsed ? '>' : 'v') : '';
    row.querySelector('.file-mark').textContent = isDirectory ? directoryStatus(node.path) : fileStatus(node.path);
    row.querySelector('.file-name').textContent = node.name;
    row.title = `/repo/${node.path}`;
    if (isDirectory) {
      row.setAttribute('aria-expanded', collapsed ? 'false' : 'true');
      row.addEventListener('click', () => {
        if (collapsed) state.collapsedDirs.delete(node.path);
        else state.collapsedDirs.add(node.path);
        renderExplorer();
      });
    } else {
      row.addEventListener('click', () => openFile(node.path));
    }
    elements.explorer.append(row);
  }
}

function renderChanges() {
  elements.changes.textContent = '';
  if (state.statusRows.length === 0) {
    const clean = document.createElement('div');
    clean.className = 'empty';
    clean.textContent = 'Clean working tree';
    elements.changes.append(clean);
    return;
  }
  for (const entry of state.statusRows) {
    const row = document.createElement('button');
    row.type = 'button';
    row.className = 'change-row';
    row.innerHTML = '<span class="change-code"></span><span class="change-path"></span>';
    row.querySelector('.change-code').textContent = entry.code;
    row.querySelector('.change-path').textContent = entry.path;
    row.addEventListener('click', () => {
      if (!entry.code.includes('D')) openFile(entry.path);
      run(`diff -- ${quoteArg(entry.path)}`);
    });
    elements.changes.append(row);
  }
}

function showCommitDetails(row) {
  if (!row.oid) return;
  const result = state.git.run(`show --stat ${row.oid}`);
  print(`show --stat ${row.oid}`, result);
}

function renderHistory() {
  elements.history.textContent = '';
  if (state.historyRows.length === 0) {
    const empty = document.createElement('div');
    empty.className = 'empty';
    empty.textContent = 'No commits';
    elements.history.append(empty);
    return;
  }
  for (const entry of state.historyRows) {
    const row = document.createElement('button');
    row.type = 'button';
    row.className = 'history-row';
    row.innerHTML = '<span class="history-dot"></span><span class="history-main"><span class="history-subject"></span><span class="history-meta"></span></span>';
    row.querySelector('.history-subject').textContent = entry.subject || '(no subject)';
    row.querySelector('.history-meta').textContent = `${entry.shortOid} ${entry.author}`.trim();
    row.title = entry.oid;
    row.addEventListener('click', () => showCommitDetails(entry));
    elements.history.append(row);
  }
}

function renderTabs() {
  elements.tabs.textContent = '';
  for (const tab of state.tabs.values()) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = `tab${state.activePath === tab.path ? ' active' : ''}`;
    button.textContent = `${tab.dirty ? '* ' : ''}${tab.path}`;
    button.addEventListener('click', () => openFile(tab.path));
    const close = document.createElement('span');
    close.className = 'tab-close';
    close.textContent = ' x';
    close.addEventListener('click', (event) => {
      event.stopPropagation();
      closeFile(tab.path);
    });
    button.append(close);
    elements.tabs.append(button);
  }
  elements.editorTitle.textContent = state.activePath ? `/repo/${state.activePath}` : 'No file open';
  syncHighlight();
}

function seedDemoRepo() {
  state.git.reset();
  state.git.writeFile('/repo/README.md', '# Browser Git workspace\n\nThis repository lives inside a WebAssembly module.\n');
  state.git.writeFile('/repo/src/main.c', '#include <stddef.h>\n\nint main(void) {\n    return 0;\n}\n');
  state.git.writeFile('/repo/docs/notes.md', '# Notes\n\nEdit several files, stage them, and commit.\n');
  state.git.run('init');
  state.git.run('add README.md src/main.c docs/notes.md');
  state.git.run('commit -m initial');
  state.tabs.clear();
  state.activePath = null;
  state.collapsedDirs.clear();
  refreshAll();
  openFile('README.md');
  elements.output.textContent = 'demo repository initialized';
}

function stageAllPaths() {
  saveAll();
  refreshStatus();
  const paths = new Set();
  for (const file of state.files) {
    if (file.type === 'f' && !file.path.startsWith('.git/')) paths.add(file.path);
  }
  for (const row of state.statusRows) {
    if (row.path && !row.path.startsWith('.git/')) paths.add(row.path);
  }
  if (paths.size === 0) {
    elements.output.textContent = 'nothing to stage';
    return { status: 0, stdout: '', stderr: '' };
  }
  return run(`add -- ${[...paths].map(quoteArg).join(' ')}`);
}

function commitAll() {
  const message = elements.commitMessage.value.trim() || 'browser edit';
  const stageResult = stageAllPaths();
  if (stageResult.status !== 0) return;
  const result = run(`commit -m ${quoteArg(message)}`);
  refreshHistory();
  return result;
}

async function cloneRemote() {
  const url = elements.remoteUrl.value.trim();
  if (!url) return;
  saveAll();
  elements.status.textContent = 'cloning';
  elements.output.textContent = `$ git clone ${url} /repo\nfetching refs...`;
  try {
    const result = await state.git.cloneFromSmartHttp(url, '/repo');
    const stderr = result.stderr ? `\n[stderr]\n${result.stderr}` : '';
    const progress = result.progress ? `\n[remote]\n${result.progress}` : '';
    elements.output.textContent = `$ git clone ${url} /repo\nexit ${result.status}\nselected ${result.selected.name}\n\n${result.stdout}${progress}${stderr}`;
    state.tabs.clear();
    state.activePath = null;
    state.collapsedDirs.clear();
    refreshAll();
    const firstFile = state.files.find((entry) => entry.type === 'f' && !entry.path.startsWith('.git/'));
    if (firstFile) openFile(firstFile.path);
  } catch (error) {
    elements.status.textContent = 'clone failed';
    elements.output.textContent = `$ git clone ${url} /repo\n\n${String(error.stack || error)}`;
  }
}

async function boot() {
  state.git = await createGitWasm();
  elements.status.textContent = 'ready';
  elements.editor.disabled = true;
  seedDemoRepo();
}

document.querySelector('#newFile').addEventListener('click', createFile);
document.querySelector('#save').addEventListener('click', () => saveActive());
document.querySelector('#saveAll').addEventListener('click', saveAll);
document.querySelector('#deleteFile').addEventListener('click', deleteActive);
document.querySelector('#resetDemo').addEventListener('click', seedDemoRepo);
document.querySelector('#statusButton').addEventListener('click', refreshStatus);
document.querySelector('#refresh').addEventListener('click', refreshAll);
document.querySelector('#stageAll').addEventListener('click', stageAllPaths);
document.querySelector('#commit').addEventListener('click', commitAll);
document.querySelector('#stash').addEventListener('click', () => run('stash'));
document.querySelector('#pull').addEventListener('click', () => run('pull'));
document.querySelector('#clone').addEventListener('click', cloneRemote);
document.querySelector('#monaRepo').addEventListener('click', () => {
  elements.remoteUrl.value = 'http://127.0.0.1:8093/mona-isa.git';
  cloneRemote();
});
document.querySelector('#log').addEventListener('click', () => run('log --oneline -n 8'));
document.querySelector('#diff').addEventListener('click', () => run('diff'));
document.querySelector('#run').addEventListener('click', () => run(elements.command.value));
elements.includeGit.addEventListener('change', refreshFiles);
elements.editor.addEventListener('scroll', syncHighlight);
elements.editor.addEventListener('input', () => {
  if (!state.activePath) return;
  const tab = state.tabs.get(state.activePath);
  tab.content = elements.editor.value;
  tab.dirty = tab.content !== tab.savedContent;
  syncHighlight();
  renderTabs();
});

boot().catch((error) => {
  elements.status.textContent = 'load failed';
  elements.output.textContent = String(error.stack || error);
});
