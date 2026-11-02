const assert = require('assert');
const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const root = process.env.TT_TEST_ROOT || '/tmp/tt-controller';
const ready = '/tmp/tt-live-test-ready.log';
const events = '/tmp/tt-live-test-events.log';
const env = {...process.env, LANG:'C.UTF-8', LC_ALL:'C.UTF-8', WINEDEBUG:'-all',
             TT_DUAL_READY:'Z:\\tmp\\tt-live-test-ready.log',
             TT_DUAL_EVENTS:'Z:\\tmp\\tt-live-test-events.log'};
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function check(initial) {
    for (const file of [ready, events]) {
        try { fs.unlinkSync(file); } catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    const dir = path.join(root, 'dual-entertainment');
    const mock = cp.spawn('wine', [path.join(dir, 'TT语音娱乐版.exe')],
        {cwd:dir, env:{...env, TT_DUAL_INITIAL:String(initial)}, stdio:'ignore'});
    try {
        for (let i = 0; i < 100 && !fs.existsSync(ready); i++) await sleep(100);
        assert.ok(fs.existsSync(ready), '模拟娱乐版未启动');
        const r = cp.spawnSync('wine', [path.join(root, 'live-validation.exe'), '--confirm-exercise'],
            {cwd:root, env, encoding:'utf8', timeout:30000});
        if (r.error) throw r.error;
        const log = fs.existsSync(events) ? fs.readFileSync(events, 'utf8') : '';
        if (initial === 64) {
            assert.strictEqual(r.status, 0, r.stdout + r.stderr);
            assert.match(r.stdout, /ORIGINAL 64 1 3[\s\S]*READBACK 96 1 3[\s\S]*RESTORED 64 1 3/);
            assert.match(log, /SET 96 1 3[\s\S]*SET 64 1 3/);
        } else {
            assert.strictEqual(r.status, 2, r.stdout + r.stderr);
            assert.match(r.stdout, /ORIGINAL_OUTSIDE_RESTORABLE_RANGE/);
            assert.doesNotMatch(log, /SET /, '无法恢复的原值不能被修改');
        }
    } finally {
        mock.kill('SIGTERM');
        await sleep(700);
    }
}
(async () => {
    await check(64);
    await check(320);
    console.log('PASS: 实测探针同一进程内临时设置、回读、恢复；无法恢复时不设置');
})().catch(error => { console.error(error.stack || error); process.exitCode = 1; });
