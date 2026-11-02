const assert = require('assert');
const fs = require('fs');
const path = require('path');
const cp = require('child_process');

const root = process.env.TT_TEST_ROOT || '/tmp/tt-controller';
const ready = '/tmp/tt-bitrate-fixture-ready.log';
const events = '/tmp/tt-bitrate-fixture-events.log';
const env = {...process.env, LANG: 'C.UTF-8', LC_ALL: 'C.UTF-8', WINEDEBUG: '-all'};
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

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
    throw new Error(message);
}

const cases = [
    {code: '1003002', expected: /1003002.*降低码率.*192/s, forbidden: /进入语音房间/},
    {code: '1000001', expected: /尚未初始化.*1000001.*进入语音房间/s},
    {code: '1000002', expected: /尚未登录语音房间.*1000002/s},
    {code: '1003005', expected: /返回错误码 1003005/, forbidden: /进入语音房间/},
];

(async () => {
    for (const item of cases) {
        for (const file of [ready, events]) {
            try { fs.unlinkSync(file); } catch (error) { if (error.code !== 'ENOENT') throw error; }
        }
        const mock = cp.spawn('wine', [path.join(root, 'TTVoice.exe')], {
            cwd: root, env: {...env, TT_MOCK_SET_ERROR: item.code}, stdio: 'ignore',
        });
        let ui;
        try {
            await waitFor(() => fs.existsSync(ready), '模拟 TT 未启动');
            ui = cp.spawn('wine', [path.join(root, 'TT-bitrate.exe')], {
                cwd: root, env, stdio: 'ignore',
            });
            await waitFor(() => driver('status').status === 0, '工具窗口未启动');
            assert.strictEqual(driver('apply', '192').status, 0);
            await waitFor(() => driver('status').stdout.includes(item.code), '未显示错误码 ' + item.code);
            const status = driver('status').stdout;
            assert.match(status, item.expected, '错误码 ' + item.code + ' 的提示不正确');
            if (item.forbidden) assert.doesNotMatch(status, item.forbidden, '错误码不应提示进入房间');
        } finally {
            if (ui) ui.kill('SIGTERM');
            mock.kill('SIGTERM');
            await sleep(1200);
        }
    }
    console.log('PASS: 码率超限、引擎未初始化、未登录房间及其他错误提示');
})().catch(error => { console.error(error.stack || error); process.exitCode = 1; });
