const assert = require('assert');
const fs = require('fs');
const path = require('path');
const cp = require('child_process');

const root = process.env.TT_TEST_ROOT || '/tmp/tt-controller';
const env = {...process.env, LANG: 'C.UTF-8', LC_ALL: 'C.UTF-8', WINEDEBUG: '-all'};
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const fixtures = {
    game: {dir: path.join(root, 'dual-game'), exe: 'TTVoice.exe', initial: 64},
    entertainment: {dir: path.join(root, 'dual-entertainment'), exe: 'TT语音娱乐版.exe', initial: 96},
};
for (const [name, fixture] of Object.entries(fixtures)) {
    fixture.ready = `/tmp/tt-dual-${name}-ready.log`;
    fixture.events = `/tmp/tt-dual-${name}-events.log`;
}
function events(fixture) { return fs.existsSync(fixture.events) ? fs.readFileSync(fixture.events, 'utf8') : ''; }
function driver(...args) {
    const result = cp.spawnSync('wine', [path.join(root, 'ui_driver.exe'), ...args], {
        cwd: root, env, encoding: 'utf8', timeout: 15000,
    });
    if (result.error) throw result.error;
    return result;
}
async function waitFor(predicate, message, timeout = 18000) {
    for (let elapsed = 0; elapsed < timeout; elapsed += 100) {
        if (predicate()) return;
        await sleep(100);
    }
    throw new Error(message + '\ngame: ' + events(fixtures.game) + '\nentertainment: ' + events(fixtures.entertainment));
}
function launch(fixture, helper = false) {
    const ready = helper ? '/tmp/tt-dual-helper-ready.log' : fixture.ready;
    const variables = {...env, TT_DUAL_READY: 'Z:' + ready.replaceAll('/', '\\'),
        TT_DUAL_EVENTS: 'Z:' + fixture.events.replaceAll('/', '\\'),
        TT_DUAL_INITIAL: String(fixture.initial), TT_DUAL_RESET: '1'};
    if (helper) variables.TT_DUAL_HELPER = '1';
    return cp.spawn('wine', [path.join(fixture.dir, fixture.exe)], {cwd: fixture.dir, env: variables, stdio: 'ignore'});
}
function requireDriver(...args) {
    const r = driver(...args);
    assert.strictEqual(r.status, 0, `界面操作 ${args.join(' ')} 失败，退出码 ${r.status}: ${r.stdout}${r.stderr}`);
}

(async () => {
    for (const fixture of Object.values(fixtures)) for (const file of [fixture.ready, fixture.events]) {
        try { fs.unlinkSync(file); } catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    try { fs.unlinkSync('/tmp/tt-dual-helper-ready.log'); } catch (error) { if (error.code !== 'ENOENT') throw error; }
    const processes = [];
    try {
        processes.push(launch(fixtures.game), launch(fixtures.entertainment), launch(fixtures.entertainment, true));
        await waitFor(() => Object.values(fixtures).every(f => fs.existsSync(f.ready)) &&
                            fs.existsSync('/tmp/tt-dual-helper-ready.log'), '模拟客户端未启动');
        for (const fixture of Object.values(fixtures)) assert.match(fs.readFileSync(fixture.ready, 'utf8'), /ready/);
        processes.push(cp.spawn('wine', [path.join(root, 'TT-bitrate.exe')], {cwd: root, env, stdio: 'ignore'}));
        await waitFor(() => driver('status').status === 0, '工具窗口未启动');
        requireDriver('edition', 'game');
        await waitFor(() => /TTVoice\.exe/.test(driver('location').stdout), '未识别开黑版安装位置');
        requireDriver('apply', '128');
        await waitFor(() => /SET 128 1 3/.test(events(fixtures.game)), '开黑版没有被单独设置');
        assert.doesNotMatch(events(fixtures.entertainment), /SET /, '设置开黑版时不能修改娱乐版');
        requireDriver('edition', 'entertainment');
        await waitFor(() => /TT语音娱乐版\.exe/.test(driver('location').stdout), '未识别娱乐版安装位置');
        requireDriver('apply', '160');
        await waitFor(() => /SET 160 1 3/.test(events(fixtures.entertainment)), '娱乐版没有被单独设置');
        assert.doesNotMatch(events(fixtures.game), /SET 160 /, '娱乐版的数值不能写入开黑版');
        await waitFor(() => /RESET 64/.test(events(fixtures.game)) && /RESET 96/.test(events(fixtures.entertainment)), '测试客户端未重置');
        await waitFor(() => (events(fixtures.game).match(/SET 128/g) || []).length >= 2 &&
                            (events(fixtures.entertainment).match(/SET 160/g) || []).length >= 2,
                      '两个版本未独立自动保持', 25000);
        requireDriver('stop');
        requireDriver('edition', 'game');
        requireDriver('stop');
        const before = [events(fixtures.game), events(fixtures.entertainment)];
        await sleep(12000);
        assert.deepStrictEqual([events(fixtures.game), events(fixtures.entertainment)], before, '关闭各自保持后不能继续读写');
        const duplicate = {...fixtures.entertainment, ready:'/tmp/tt-dual-duplicate-ready.log',
                           events:'/tmp/tt-dual-duplicate-events.log'};
        for (const file of [duplicate.ready, duplicate.events]) {
            try { fs.unlinkSync(file); } catch (error) { if (error.code !== 'ENOENT') throw error; }
        }
        processes.push(launch(duplicate));
        await waitFor(() => fs.existsSync(duplicate.ready), '第二个娱乐版未启动');
        requireDriver('edition', 'entertainment');
        requireDriver('apply', '100');
        await waitFor(() => /多个音频实例/.test(driver('status').stdout), '多个同版音频实例时没有阻止写入');
        assert.doesNotMatch(events(duplicate), /SET /, '同版多实例不应任意选中写入');
        assert.strictEqual(events(fixtures.entertainment), before[1], '同版多实例不应修改原实例');
        console.log('PASS: 两版位置识别、分别设置和保持、辅助进程识别、同版多实例安全提示');
    } finally {
        for (const process of processes) process.kill('SIGTERM');
        await sleep(600);
    }
})().catch(error => { console.error(error.stack || error); process.exitCode = 1; });
