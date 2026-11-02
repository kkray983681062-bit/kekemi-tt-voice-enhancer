const assert = require('assert');
const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const root = process.env.TT_TEST_ROOT || '/tmp/tt-controller';
const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'tt-locator-test-'));
const linuxA = path.join(temp, '路径 A');
const linuxB = path.join(temp, '路径 B');
fs.mkdirSync(linuxA); fs.mkdirSync(linuxB);
for (const dir of [linuxA, linuxB]) {
    fs.copyFileSync(path.join(root, 'TTVoice.exe'), path.join(dir, 'TTVoice.exe'));
    fs.copyFileSync(path.join(root, 'ZegoExpressEngine.dll'), path.join(dir, 'ZegoExpressEngine.dll'));
}
const win = p => 'Z:' + p.replace(/\//g, '\\');
const targetA = win(path.join(linuxA, 'TTVoice.exe'));
const targetB = win(path.join(linuxB, 'TTVoice.exe'));
const configRoot = path.join(temp, 'local');
fs.mkdirSync(configRoot);
const env = {...process.env, LANG:'C.UTF-8', LC_ALL:'C.UTF-8', WINEDEBUG:'-all',
             TT_LOCATOR_TEST_DIR:win(configRoot), TT_LOCATOR_TEST_MODE:'1'};
const regBase = 'HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TTLocatorTest' + process.pid;
let launchedPid = null;
let outputIndex = 0;

function run(...args) {
    const outputPath = path.join(temp, 'command-' + (++outputIndex) + '.log');
    const output = fs.openSync(outputPath, 'w');
    const out = cp.spawnSync('wine', args, {cwd:root, env, stdio:['ignore',output,output], timeout:30000});
    fs.closeSync(output);
    const text = fs.readFileSync(outputPath, 'utf8').trim();
    if (out.error) throw out.error;
    if (out.status !== 0) throw new Error('wine command failed: ' + args.join(' ') + '\n' + text);
    return text;
}
const cli = (...args) => run(path.join(root, 'locator-test.exe'), ...args);
function regAdd(key, property, value) {
    run('reg', 'add', key, '/v', property, '/t', 'REG_SZ', '/d', value, '/f');
}
function removeSaved() {
    const file = path.join(configRoot, 'KekemiTTVoiceEnhancer', 'tt-path.txt');
    if (fs.existsSync(file)) fs.unlinkSync(file);
}

try {
    assert.strictEqual(cli('save', win(path.join(linuxA, 'wrong.exe'))), 'INVALID_OR_ERROR');
    assert.strictEqual(cli('save', targetA), 'SAVED', 'manual selection must persist');
    assert.strictEqual(cli('load'), 'SAVED ' + targetA, 'Unicode path must survive restart');
    assert.strictEqual(cli('find'), 'SAVED ' + targetA, 'remembered choice takes priority');

    fs.renameSync(path.join(linuxA, 'TTVoice.exe'), path.join(linuxA, 'temporarily-gone.exe'));
    assert.strictEqual(cli('load'), 'NONE', 'a stale path must not be returned');
    fs.renameSync(path.join(linuxA, 'temporarily-gone.exe'), path.join(linuxA, 'TTVoice.exe'));
    removeSaved();

    regAdd(regBase + 'A', 'DisplayName', 'TT语音 测试 A');
    regAdd(regBase + 'A', 'InstallLocation', win(linuxA));
    assert.strictEqual(cli('find'), 'REGISTRY ' + targetA, 'finds a registered installation');
    regAdd(regBase + 'B', 'DisplayName', 'TT语音 测试 B');
    regAdd(regBase + 'B', 'InstallLocation', win(linuxB));
    assert.strictEqual(cli('find'), 'MULTIPLE', 'multiple installations require a choice');
    assert.strictEqual(cli('save', targetA), 'SAVED');
    assert.strictEqual(cli('find'), 'SAVED ' + targetA, 'saved choice resolves ambiguity');
    removeSaved();

    const started = cli('start', targetA);
    const match = /^STARTED (\d+)$/.exec(started);
    assert(match, 'launches the selected TT fixture: ' + started);
    launchedPid = match[1];
    assert.strictEqual(cli('find'), 'RUNNING ' + targetA, 'discovers the running executable');
    assert.strictEqual(cli('start', targetB), 'ALREADY_RUNNING', 'does not launch a duplicate');
    assert.strictEqual(cli('start', win(path.join(linuxA, 'missing.exe'))), 'ALREADY_RUNNING',
                       'recognizes an already running TT even if the saved path is invalid');
    console.log('PASS: Unicode path, persistence, stale paths, registry discovery, ambiguity and controlled launch');
} finally {
    if (launchedPid) {
        cp.spawnSync('wine', ['taskkill', '/PID', launchedPid, '/F'], {cwd:root, env, encoding:'utf8', timeout:15000});
    }
    for (const key of [regBase + 'A', regBase + 'B'])
        cp.spawnSync('wine', ['reg','delete',key,'/f'], {cwd:root, env, encoding:'utf8', timeout:15000});
    fs.rmSync(temp, {recursive:true, force:true});
}
