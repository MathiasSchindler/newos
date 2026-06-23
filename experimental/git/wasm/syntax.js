const C_KEYWORDS = new Set([
  'auto', 'break', 'case', 'char', 'const', 'continue', 'default', 'do', 'double',
  'else', 'enum', 'extern', 'float', 'for', 'goto', 'if', 'inline', 'int', 'long',
  'register', 'restrict', 'return', 'short', 'signed', 'sizeof', 'static', 'struct',
  'switch', 'typedef', 'union', 'unsigned', 'void', 'volatile', 'while', '_Bool',
  '_Noreturn', 'size_t', 'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t', 'intptr_t',
  'uintptr_t', 'NULL', 'true', 'false',
]);

const PYTHON_KEYWORDS = new Set([
  'and', 'as', 'assert', 'async', 'await', 'break', 'class', 'continue', 'def',
  'del', 'elif', 'else', 'except', 'False', 'finally', 'for', 'from', 'global',
  'if', 'import', 'in', 'is', 'lambda', 'None', 'nonlocal', 'not', 'or', 'pass',
  'raise', 'return', 'True', 'try', 'while', 'with', 'yield',
]);

export function detectLanguage(path) {
  const lower = String(path || '').toLowerCase();
  if (/\.(c|h|cc|cpp|cxx|hpp)$/.test(lower)) return 'c';
  if (/\.(py|pyw)$/.test(lower)) return 'python';
  if (/\.(md|markdown|mkd)$/.test(lower)) return 'markdown';
  return 'plain';
}

function escapeHtml(text) {
  return String(text)
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;');
}

function span(className, text) {
  return `<span class="${className}">${escapeHtml(text)}</span>`;
}

function isIdentStart(ch) {
  return /[A-Za-z_]/.test(ch);
}

function isIdent(ch) {
  return /[A-Za-z0-9_]/.test(ch);
}

function highlightCode(text, language) {
  const keywords = language === 'python' ? PYTHON_KEYWORDS : C_KEYWORDS;
  const commentStart = language === 'python' ? '#' : '//';
  const lines = String(text).split(/(\n)/);
  let out = '';
  let inBlockComment = false;

  for (let partIndex = 0; partIndex < lines.length; partIndex += 1) {
    const line = lines[partIndex];
    if (line === '\n') {
      out += '\n';
      continue;
    }
    if (language === 'c' && /^\s*#/.test(line)) {
      out += span('syntax-preprocessor', line);
      continue;
    }
    let index = 0;
    while (index < line.length) {
      if (language === 'c' && inBlockComment) {
        const end = line.indexOf('*/', index);
        if (end < 0) {
          out += span('syntax-comment', line.slice(index));
          index = line.length;
        } else {
          out += span('syntax-comment', line.slice(index, end + 2));
          index = end + 2;
          inBlockComment = false;
        }
        continue;
      }

      if (language === 'c' && line.startsWith('/*', index)) {
        const end = line.indexOf('*/', index + 2);
        if (end < 0) {
          out += span('syntax-comment', line.slice(index));
          inBlockComment = true;
          break;
        }
        out += span('syntax-comment', line.slice(index, end + 2));
        index = end + 2;
        continue;
      }

      if (line.startsWith(commentStart, index)) {
        out += span('syntax-comment', line.slice(index));
        break;
      }

      const ch = line[index];
      if (ch === '"' || ch === "'") {
        const quote = ch;
        let end = index + 1;
        while (end < line.length) {
          if (line[end] === '\\') {
            end += 2;
          } else if (line[end] === quote) {
            end += 1;
            break;
          } else {
            end += 1;
          }
        }
        out += span('syntax-string', line.slice(index, end));
        index = end;
        continue;
      }

      if (/[0-9]/.test(ch)) {
        const match = line.slice(index).match(/^(0x[0-9A-Fa-f]+|\d+(\.\d+)?([eE][+-]?\d+)?)/);
        if (match) {
          out += span('syntax-number', match[0]);
          index += match[0].length;
          continue;
        }
      }

      if (isIdentStart(ch)) {
        let end = index + 1;
        while (end < line.length && isIdent(line[end])) end += 1;
        const word = line.slice(index, end);
        out += keywords.has(word) ? span('syntax-keyword', word) : escapeHtml(word);
        index = end;
        continue;
      }

      out += escapeHtml(ch);
      index += 1;
    }
  }
  return out;
}

function highlightMarkdownInline(text) {
  const pattern = /(`[^`]*`|\[[^\]]+\]\([^)]+\)|(\*\*|__)(.*?)\2|(\*|_)(.*?)\4)/g;
  let out = '';
  let last = 0;
  for (const match of text.matchAll(pattern)) {
    out += escapeHtml(text.slice(last, match.index));
    const token = match[0];
    if (token.startsWith('`')) out += span('syntax-code', token);
    else if (token.startsWith('[')) out += span('syntax-link', token);
    else out += span('syntax-emphasis', token);
    last = match.index + token.length;
  }
  out += escapeHtml(text.slice(last));
  return out;
}

function highlightMarkdown(text) {
  const lines = String(text).split(/(\n)/);
  let out = '';
  let inFence = false;
  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index];
    if (line === '\n') {
      out += '\n';
      continue;
    }
    if (/^\s*(```|~~~)/.test(line)) {
      inFence = !inFence;
      out += span('syntax-code', line);
    } else if (inFence) {
      out += span('syntax-code', line);
    } else if (/^#{1,6}\s/.test(line)) {
      out += span('syntax-heading', line);
    } else if (/^\s*([-*+]\s|\d+\.\s)/.test(line)) {
      out += span('syntax-list', line.match(/^\s*([-*+]\s|\d+\.\s)/)[0]);
      out += highlightMarkdownInline(line.replace(/^\s*([-*+]\s|\d+\.\s)/, ''));
    } else {
      out += highlightMarkdownInline(line);
    }
  }
  return out;
}

export function highlightSyntax(path, text) {
  const language = detectLanguage(path);
  if (language === 'markdown') return highlightMarkdown(text);
  if (language === 'c' || language === 'python') return highlightCode(text, language);
  return escapeHtml(text);
}