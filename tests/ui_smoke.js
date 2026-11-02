const assert = require('assert');
const fs = require('fs');
const path = require('path');
const cp = require('child_process');

const root = process.env.TT_TEST_ROOT || '/tmp/tt-controller';
const ready = '/tmp/tt-bitrate-fixture-ready.log';
const events = '/tmp/tt-bitrate-fixture-events.log';
const env = {...process.env, LANG: 'C.UTF-8', LC_ALL: 'C.UTF-8', WINEDEBUG: '-all'};
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
function readEvents() { return fs.existsSync(events) ? fs.readFileSync(events, 'utf8') : ''; }
function driver(...args) {
    const result = cp.spawnSync('wine', [path.join(root, 'ui_driver.exe'), ...args], {
        cwd: root, env, encoding: 'utf8', timeout: 15000,
    });
    if (result.error) throw result.error;
    return result;
}
async function waitFor(predicate, message, timeout = 15000) {
    for (let elapsed = 0; elapsed < timeout; elapsed += 100) {
        if (predicate()) return;
        await sleep(100);
    }
    throw new Error(message + '\n' + readEvents());
}

(async () => {
    for (const file of [ready, events]) {
        try { fs.unlinkSync(file); } catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    const mock = cp.spawn('wine', [path.join(root, 'TTVoice.exe')], {
        cwd: root, env: {...env, TT_MOCK_RESET_ONCE: '1'}, stdio: 'ignore',
    });
    let ui;
    try {
        await waitFor(() => fs.existsSync(ready), '模拟 TT 未启动');
        ui = cp.spawn('wine', [path.join(root, 'TT-bitrate.exe')], {cwd: root, env, stdio: 'ignore'});
        await waitFor(() => driver('status').status === 0, '工具窗口未启动');
        await sleep(1200);
        assert.strictEqual(readEvents(), '', '启动窗口不能自动读写 TT');

        assert.strictEqual(driver('read').status, 0);
        await waitFor(() => /GET 64 1 3/.test(readEvents()), '读取按钮未读取配置');
        await waitFor(() => /64 kbps/.test(driver('status').stdout), '界面未显示回读值');

        const beforeInvalid = readEvents();
        for (const invalid of ['193', '320']) {
            assert.strictEqual(driver('apply', invalid).status, 0);
            await waitFor(() => /6–192/.test(driver('status').stdout), '界面没有提示正确的范围');
            assert.strictEqual(readEvents(), beforeInvalid, '越界值不能触发 TT 读写');
        }

        assert.strictEqual(driver('apply', '192').status, 0);
        await waitFor(() => /SET 192 1 3/.test(readEvents()), '应用按钮未设置码率');
        await waitFor(() => /RESET 64 1 3/.test(readEvents()), '模拟 TT 未重置码率');
        await waitFor(() => (readEvents().match(/SET 192 1 3/g) || []).length >= 2,
                      '自动保持没有重新设置码率', 18000);

        assert.strictEqual(driver('stop').status, 0);
        await waitFor(() => /自动保持已关闭/.test(driver('status').stdout), '界面未显示已停止');
        const before = readEvents();
        await sleep(11500);
        assert.strictEqual(readEvents(), before, '关闭自动保持后不能继续读取或设置');
        console.log('PASS: 界面启动不改动、读取、设置、自动恢复和停止保持');
    } finally {
        if (ui) ui.kill('SIGTERM');
        mock.kill('SIGTERM');
        await sleep(700);
    }
})().catch(error => { console.error(error.stack || error); process.exitCode = 1; });
