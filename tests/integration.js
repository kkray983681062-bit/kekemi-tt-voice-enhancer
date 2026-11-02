const assert = require('assert');
const fs = require('fs');
const path = require('path');
const cp = require('child_process');

const root = process.env.TT_TEST_ROOT || '/tmp/tt-controller';
const controller = path.join(root, 'controller-test.exe');
const fixture = path.join(root, 'TTVoice.exe');
const ready = '/tmp/tt-bitrate-fixture-ready.log';
const events = '/tmp/tt-bitrate-fixture-events.log';
const env = {...process.env, LANG: 'C.UTF-8', LC_ALL: 'C.UTF-8', WINEDEBUG: '-all'};
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

function run(...args) {
    const result = cp.spawnSync('wine', [controller, ...args], {
        cwd: root, env, encoding: 'utf8', timeout: 30000,
    });
    if (result.error) throw result.error;
    return {status: result.status, text: result.stdout + result.stderr};
}

async function startMock(extraEnv = {}) {
    for (const name of [ready, events]) {
        try { fs.unlinkSync(name); } catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    const process = cp.spawn('wine', [fixture], {
        cwd: root, env: {...env, ...extraEnv}, stdio: 'ignore',
    });
    for (let i = 0; i < 100; i++) {
        if (fs.existsSync(ready)) {
            assert.match(fs.readFileSync(ready, 'utf8'), /ready/, '模拟引擎没有加载');
            return process;
        }
        if (process.exitCode !== null) throw new Error('模拟 TT 意外退出');
        await sleep(100);
    }
    process.kill('SIGTERM');
    throw new Error('模拟 TT 启动超时');
}

async function stopMock(process) {
    process.kill('SIGTERM');
    await sleep(1200);
}

(async () => {
    assert.ok(fs.existsSync(controller), '未找到控制程序，尚不能读取或设置码率');
    assert.ok(fs.existsSync(fixture), '未找到模拟 TT');
    assert.ok(fs.existsSync(path.join(root, 'ZegoExpressEngine.dll')), '未找到模拟 ZEGO 引擎');

    let result = run('--read');
    assert.notStrictEqual(result.status, 0, '没有 TT 时不能报告读取成功');
    assert.match(result.text, /NO_TARGET/);

    let mock = await startMock();
    try {
        result = run('--read');
        assert.strictEqual(result.status, 0, result.text);
        assert.match(result.text, /READ 64 1 3/);
        assert.doesNotMatch(fs.readFileSync(events, 'utf8'), /SET /, '读取不能修改配置');

        for (const invalid of ['193', '320']) {
            result = run('--set', invalid);
            assert.notStrictEqual(result.status, 0, '超出范围不能报告成功');
            assert.match(result.text, /OUT_OF_RANGE/);
            assert.doesNotMatch(fs.readFileSync(events, 'utf8'), /SET /, '无效码率不能写入');
        }

        result = run('--set', '192');
        assert.strictEqual(result.status, 0, result.text);
        assert.match(result.text, /APPLIED 192 1 3/);
        assert.match(fs.readFileSync(events, 'utf8'), /SET 192 1 3/, '须保留原声道和编码格式');
    } finally {
        await stopMock(mock);
    }

    mock = await startMock({TT_MOCK_CLAMP: '1'});
    try {
        result = run('--set', '192');
        assert.notStrictEqual(result.status, 0, '引擎回读不一致不能报告成功');
        assert.match(result.text, /READBACK_MISMATCH/);
    } finally {
        await stopMock(mock);
    }

    mock = await startMock({TT_MOCK_GET_ERROR: '1'});
    try {
        result = run('--set', '192');
        assert.notStrictEqual(result.status, 0, 'SDK 返回错误时不能报告成功');
        assert.match(result.text, /SDK_ERROR/);
        assert.doesNotMatch(fs.readFileSync(events, 'utf8'), /SET /, '读取出错时不能继续写入');
    } finally {
        await stopMock(mock);
    }
    console.log('PASS: 进程发现、只读读取、输入限制、设置与回读、回读不一致、SDK 返回错误');
})().catch(error => {
    console.error(error.stack || error);
    process.exitCode = 1;
});
