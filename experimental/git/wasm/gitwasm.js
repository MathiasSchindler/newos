const decoder = new TextDecoder();
const encoder = new TextEncoder();

function concatBytes(chunks) {
  const size = chunks.reduce((sum, chunk) => sum + chunk.length, 0);
  const out = new Uint8Array(size);
  let offset = 0;
  for (const chunk of chunks) {
    out.set(chunk, offset);
    offset += chunk.length;
  }
  return out;
}

function hexLength(bytes, offset) {
  let value = 0;
  for (let index = 0; index < 4; index += 1) {
    const ch = bytes[offset + index];
    let digit = -1;
    if (ch >= 48 && ch <= 57) digit = ch - 48;
    else if (ch >= 97 && ch <= 102) digit = ch - 87;
    else if (ch >= 65 && ch <= 70) digit = ch - 55;
    if (digit < 0) throw new Error('invalid pkt-line length');
    value = (value << 4) | digit;
  }
  return value;
}

function pktLine(payload) {
  const bytes = typeof payload === 'string' ? encoder.encode(payload) : payload;
  const length = bytes.length + 4;
  if (length > 0xffff) throw new Error('pkt-line too large');
  const out = new Uint8Array(length);
  const hex = length.toString(16).padStart(4, '0');
  out.set(encoder.encode(hex), 0);
  out.set(bytes, 4);
  return out;
}

function parsePktLines(bytes) {
  const packets = [];
  let offset = 0;
  while (offset < bytes.length) {
    if (offset + 4 > bytes.length) throw new Error('truncated pkt-line length');
    const length = hexLength(bytes, offset);
    offset += 4;
    if (length === 0) {
      packets.push(null);
      continue;
    }
    if (length < 4 || offset + length - 4 > bytes.length) throw new Error('truncated pkt-line payload');
    packets.push(bytes.slice(offset, offset + length - 4));
    offset += length - 4;
  }
  return packets;
}

function serviceUrl(remoteUrl, suffix) {
  return `${remoteUrl.replace(/\/+$/, '')}${suffix}`;
}

function parseAdvertisedRefs(bytes) {
  const refs = [];
  let capabilities = '';
  let headRef = '';
  let headOid = '';
  let sawFirstRef = false;
  for (const packet of parsePktLines(bytes)) {
    if (!packet || packet.length === 0) continue;
    const text = decoder.decode(packet);
    if (text.startsWith('# service=')) continue;
    if (packet.length < 42) continue;
    const oid = text.slice(0, 40);
    if (!/^[0-9a-f]{40}$/i.test(oid) || text[40] !== ' ') continue;
    let nameEnd = packet.indexOf(0, 41);
    if (nameEnd < 0) {
      const newline = packet.indexOf(10, 41);
      nameEnd = newline >= 0 ? newline : packet.length;
    }
    const name = decoder.decode(packet.slice(41, nameEnd)).replace(/[\r\n]+$/, '');
    if (!name) continue;
    if (name === 'HEAD') headOid = oid;
    refs.push({ name, oid });
    if (!sawFirstRef && nameEnd < packet.length && packet[nameEnd] === 0) {
      const capText = decoder.decode(packet.slice(nameEnd + 1)).replace(/[\r\n]+$/, '');
      capabilities = capText;
      for (const part of capText.split(' ')) {
        if (part.startsWith('symref=HEAD:')) headRef = part.slice('symref=HEAD:'.length);
      }
    }
    sawFirstRef = true;
  }
  const selected = refs.find((ref) => ref.name === headRef)
    || refs.find((ref) => ref.name === 'refs/heads/main')
    || refs.find((ref) => ref.name === 'refs/heads/master')
    || refs.find((ref) => ref.name.startsWith('refs/heads/'))
    || refs.find((ref) => ref.name === 'HEAD')
    || refs[0];
  if (!selected) throw new Error('remote did not advertise refs');
  return { refs, selected, capabilities, headRef, headOid };
}

function buildUploadPackRequest(oid) {
  return concatBytes([
    pktLine(`want ${oid} multi_ack_detailed side-band-64k ofs-delta agent=newos-git-wasm\n`),
    encoder.encode('0000'),
    pktLine('done\n'),
  ]);
}

function parseUploadPackResult(bytes) {
  const packChunks = [];
  const progressChunks = [];
  for (const packet of parsePktLines(bytes)) {
    if (!packet || packet.length === 0) continue;
    const text = decoder.decode(packet);
    if (text === 'NAK\n') continue;
    if (packet[0] === 1) {
      packChunks.push(packet.slice(1));
    } else if (packet[0] === 2) {
      progressChunks.push(packet.slice(1));
    } else if (packet[0] === 3) {
      throw new Error(decoder.decode(packet.slice(1)).trim() || 'remote reported upload-pack error');
    } else if (packet.length >= 4 && decoder.decode(packet.slice(0, 4)) === 'PACK') {
      packChunks.push(packet);
    }
  }
  const pack = concatBytes(packChunks);
  if (pack.length < 4 || decoder.decode(pack.slice(0, 4)) !== 'PACK') throw new Error('upload-pack response did not contain a pack');
  return { pack, progress: decoder.decode(concatBytes(progressChunks)) };
}

export async function createGitWasm(url = 'build/git.wasm') {
  if (typeof process !== 'undefined' && process.versions?.node) {
    const spec = url instanceof URL ? url : (typeof url === 'string' && url.startsWith('file:') ? new URL(url) : null);
    if (spec?.protocol === 'file:') {
      const { readFile } = await import('node:fs/promises');
      const wasm = await readFile(spec);
      const module = await WebAssembly.instantiate(wasm, {});
      return createGitWasmApi(module.instance.exports);
    }
  }
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`failed to load ${url}: ${response.status}`);
  }
  const module = await WebAssembly.instantiateStreaming(response, {});
  return createGitWasmApi(module.instance.exports);
}

function createGitWasmApi(exports) {
  function memory() {
    return new Uint8Array(exports.memory.buffer);
  }

  function writeString(text) {
    const bytes = encoder.encode(`${text}\0`);
    const ptr = exports.newos_wasm_alloc(bytes.length);
    memory().set(bytes, ptr);
    return ptr;
  }

  function readBytes(ptr, length) {
    return memory().slice(ptr, ptr + length);
  }

  function readString(ptr, length) {
    return decoder.decode(readBytes(ptr, length));
  }

  function writeFile(path, content) {
    const pathPtr = writeString(path);
    const bytes = typeof content === 'string' ? encoder.encode(content) : content;
    const dataPtr = exports.newos_wasm_alloc(bytes.length || 1);
    memory().set(bytes, dataPtr);
    const result = exports.newos_wasm_write_file(pathPtr, dataPtr, bytes.length);
    exports.newos_wasm_free(dataPtr);
    exports.newos_wasm_free(pathPtr);
    if (result !== 0) {
      throw new Error(`write failed: ${path}`);
    }
  }

  function readFile(path) {
    const pathPtr = writeString(path);
    const size = exports.newos_wasm_file_size(pathPtr);
    if (size < 0) {
      exports.newos_wasm_free(pathPtr);
      return null;
    }
    const dataPtr = exports.newos_wasm_alloc(size || 1);
    const got = exports.newos_wasm_read_file(pathPtr, dataPtr, size);
    const text = got >= 0 ? readString(dataPtr, got) : null;
    exports.newos_wasm_free(dataPtr);
    exports.newos_wasm_free(pathPtr);
    return text;
  }

  function listFiles({ includeGit = false } = {}) {
    const capacity = 1024 * 1024;
    const outPtr = exports.newos_wasm_alloc(capacity);
    const got = exports.newos_wasm_list_files(outPtr, capacity, includeGit ? 1 : 0);
    const text = got >= 0 ? readString(outPtr, got) : '';
    exports.newos_wasm_free(outPtr);
    return text
      .split('\n')
      .filter(Boolean)
      .map((line) => {
        const tab = line.indexOf('\t');
        return { type: line.slice(0, tab), path: line.slice(tab + 1) };
      })
      .sort((left, right) => {
        if (left.type !== right.type) return left.type === 'd' ? -1 : 1;
        return left.path.localeCompare(right.path);
      });
  }

  function deletePath(path) {
    const pathPtr = writeString(path);
    const result = exports.newos_wasm_remove_path(pathPtr);
    exports.newos_wasm_free(pathPtr);
    if (result !== 0) {
      throw new Error(`delete failed: ${path}`);
    }
  }

  function run(command) {
    const commandPtr = writeString(command);
    const status = exports.newos_git_run_line(commandPtr);
    exports.newos_wasm_free(commandPtr);
    return {
      status,
      stdout: readString(exports.newos_wasm_stdout_ptr(), exports.newos_wasm_stdout_size()),
      stderr: readString(exports.newos_wasm_stderr_ptr(), exports.newos_wasm_stderr_size()),
    };
  }

  function reset() {
    exports.newos_wasm_reset();
  }

  async function cloneFromSmartHttp(remoteUrl, destination = '/repo') {
    const refsResponse = await fetch(serviceUrl(remoteUrl, '/info/refs?service=git-upload-pack'), {
      headers: { Accept: 'application/x-git-upload-pack-advertisement' },
    });
    if (!refsResponse.ok) throw new Error(`ref discovery failed: HTTP ${refsResponse.status}`);
    const refs = parseAdvertisedRefs(new Uint8Array(await refsResponse.arrayBuffer()));
    const request = buildUploadPackRequest(refs.selected.oid);
    const packResponse = await fetch(serviceUrl(remoteUrl, '/git-upload-pack'), {
      method: 'POST',
      headers: {
        Accept: 'application/x-git-upload-pack-result',
        'Content-Type': 'application/x-git-upload-pack-request',
      },
      body: request,
    });
    if (!packResponse.ok) throw new Error(`upload-pack failed: HTTP ${packResponse.status}`);
    const result = parseUploadPackResult(new Uint8Array(await packResponse.arrayBuffer()));
    reset();
    const remotePtr = writeString(remoteUrl);
    const destPtr = writeString(destination);
    const refPtr = writeString(refs.selected.name);
    const oidPtr = writeString(refs.selected.oid);
    const packPtr = exports.newos_wasm_alloc(result.pack.length || 1);
    memory().set(result.pack, packPtr);
    const status = exports.newos_git_clone_from_pack(remotePtr, destPtr, refPtr, oidPtr, packPtr, result.pack.length);
    exports.newos_wasm_free(packPtr);
    exports.newos_wasm_free(oidPtr);
    exports.newos_wasm_free(refPtr);
    exports.newos_wasm_free(destPtr);
    exports.newos_wasm_free(remotePtr);
    return {
      status,
      selected: refs.selected,
      progress: result.progress,
      stdout: readString(exports.newos_wasm_stdout_ptr(), exports.newos_wasm_stdout_size()),
      stderr: readString(exports.newos_wasm_stderr_ptr(), exports.newos_wasm_stderr_size()),
    };
  }

  return { exports, reset, run, writeFile, readFile, listFiles, deletePath, cloneFromSmartHttp };
}
