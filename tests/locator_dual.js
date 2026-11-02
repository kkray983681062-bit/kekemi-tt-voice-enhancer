const assert = require('assert');
const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const root = process.env.TT_TEST_ROOT || '/tmp/tt-controller';
const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'tt-dual-location-'));
const game = path.join(temp, '路径 空间 开黑');
const entertainment = path.join(temp, '路径 空间 娱乐');
const config = path.join(temp, 'local');
for (const dir of [game, entertainment, config]) fs.mkdirSync(dir);
for (const [source, dest, exe] of [
    [path.join(root, 'dual-game'), game, 'TTVoice.exe'],
    [path.join(root, 'dual-entertainment'), entertainment, 'TT语音娱乐版.exe'],
]) {
    for (const file of [exe, 'ZegoExpressEngine.dll']) fs.copyFileSync(path.join(source, file), path.join(dest, file));
}
const win = p => 'Z:' + p.replace(/\//g, '\\');
const gameExe = win(path.join(game, 'TTVoice.exe'));
const entExe = win(path.join(entertainment, 'TT语音娱乐版.exe'));
const env = {...process.env, LANG:'C.UTF-8', LC_ALL:'C.UTF-8', WINEDEBUG:'-all',
             TT_LOCATOR_TEST_DIR:win(config), TT_LOCATOR_TEST_MODE:'1'};
const pids = [];
let outputIndex = 0;
function run(...args) {
    const outputPath = path.join(temp, 'command-' + (++outputIndex) + '.log');
    const output = fs.openSync(outputPath, 'w');
    const r = cp.spawnSync('wine', [path.join(root, 'locator-dual-test.exe'), ...args],
        {cwd:root, env, stdio:['ignore', output, output], timeout:30000});
    fs.closeSync(output);
    const text = fs.readFileSync(outputPath, 'utf8').trim();
    if (r.error) throw r.error;
    assert.strictEqual(r.status, 0, text);
    return text;
}
try {
    assert.strictEqual(run('game', 'save', entExe), 'INVALID_OR_ERROR');
    assert.strictEqual(run('entertainment', 'save', gameExe), 'INVALID_OR_ERROR');
    assert.strictEqual(run('game', 'save', gameExe), 'SAVED');
    assert.strictEqual(run('entertainment', 'save', entExe), 'SAVED');
    assert.strictEqual(run('game', 'load'), 'SAVED ' + gameExe);
    assert.strictEqual(run('entertainment', 'load'), 'SAVED ' + entExe);
    for (const [edition, exe] of [['game', gameExe], ['entertainment', entExe]]) {
        const result = run(edition, 'start', exe);
        const match = /^STARTED (\d+)$/.exec(result);
        assert(match, '两个版本可以分别启动: ' + edition + ' ' + result);
        pids.push(match[1]);
        assert.strictEqual(run(edition, 'start', exe), 'ALREADY_RUNNING');
    }
    console.log('PASS: 两版中文安装路径分别记住，互不接受错选，两个客户端可分别启动且不重复启动');
} finally {
    for (const pid of pids) cp.spawnSync('wine', ['taskkill', '/PID', pid, '/F'], {cwd:root, env, timeout:15000});
    fs.rmSync(temp, {recursive:true, force:true});
}
