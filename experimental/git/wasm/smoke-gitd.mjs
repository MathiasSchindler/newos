import { createGitWasm } from './gitwasm.js';

const url = process.env.GITD_URL || 'http://127.0.0.1:8090/example.git';
const git = await createGitWasm(new URL('./build/git.wasm', import.meta.url));
const result = await git.cloneFromSmartHttp(url, '/repo');

console.log(`clone_status=${result.status}`);
console.log(`selected=${result.selected.name} ${result.selected.oid}`);
if (result.stdout) process.stdout.write(result.stdout);
if (result.stderr) process.stderr.write(result.stderr);
if (result.status !== 0) process.exit(result.status);

const readme = git.readFile('/repo/README.md');
if (readme == null) {
  throw new Error('clone did not check out /repo/README.md');
}
console.log(`readme=${JSON.stringify(readme)}`);

const log = git.run('log --oneline -n 1');
console.log(`log_status=${log.status}`);
if (log.stdout) process.stdout.write(log.stdout);
if (log.stderr) process.stderr.write(log.stderr);
if (log.status !== 0) process.exit(log.status);
