import { readFile } from 'node:fs/promises';

const decoder = new TextDecoder();
const encoder = new TextEncoder();
const wasm = await readFile(new URL('./build/git.wasm', import.meta.url));
const { instance } = await WebAssembly.instantiate(wasm, {});
const exports = instance.exports;

function memory() {
  return new Uint8Array(exports.memory.buffer);
}

function writeString(text) {
  const bytes = encoder.encode(`${text}\0`);
  const ptr = exports.newos_wasm_alloc(bytes.length);
  memory().set(bytes, ptr);
  return ptr;
}

function readString(ptr, size) {
  return decoder.decode(memory().slice(ptr, ptr + size));
}

function writeFile(path, content) {
  const pathPtr = writeString(path);
  const bytes = encoder.encode(content);
  const dataPtr = exports.newos_wasm_alloc(bytes.length || 1);
  memory().set(bytes, dataPtr);
  const result = exports.newos_wasm_write_file(pathPtr, dataPtr, bytes.length);
  exports.newos_wasm_free(dataPtr);
  exports.newos_wasm_free(pathPtr);
  if (result !== 0) throw new Error(`write failed: ${path}`);
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

function show(name, result) {
  console.log(`--- ${name} status=${result.status}`);
  if (result.stdout) process.stdout.write(result.stdout);
  if (result.stderr) process.stderr.write(result.stderr);
  if (result.status !== 0) process.exitCode = result.status;
}

exports.newos_wasm_reset();
writeFile('/repo/hello.txt', 'hello from wasm git\n');
show('init', run('init'));
show('status', run('status --short'));
show('add', run('add hello.txt'));
show('commit', run('commit -m initial'));
show('log', run('log --oneline -n 1'));
