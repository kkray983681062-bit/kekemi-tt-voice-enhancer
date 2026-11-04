const assert = require('assert');
const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const root = process.env.TT_TEST_ROOT || '/tmp/tt-legacy-test';
const ready = '/tmp/tt-legacy-rate-ready.log';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function check(width, inactive) {
    try { fs.unlinkSync(ready); } catch (error) { if (error.code !== 'ENOENT') throw error; }
    const env = {...process.env, WINEDEBUG: '-all'};
    if (inactive) env.TT_FIXTURE_INACTIVE = '1';
    const fixture = cp.spawn('wine', [path.join(root, `rate-fixture-${width}.exe`)], {env, stdio:'ignore'});
    try {
        for (let i = 0; i < 150 && !fs.existsSync(ready); ++i) {
            if (fixture.exitCode !== null) throw new Error(`fixture ${width} exited`);
            await sleep(100);
        }
        const info = fs.readFileSync(ready, 'utf8').trim().split(/\s+/);
        assert.strictEqual(info[2], String(width));
        const result = cp.spawnSync('wine', [path.join(root, 'rate-process-probe.exe'), ...info],
                                    {env, encoding:'utf8', timeout:30000});
        if (result.error) throw result.error;
        assert.strictEqual(result.status, 0, result.stdout + result.stderr);
        assert.match(result.stdout, inactive ? /^WAITING/m : /^ACTIVE 137\.25/m);
    } finally {
        fixture.kill('SIGTERM');
        await sleep(500);
    }
}

(async () => {
    for (const width of [8, 4]) {
        await check(width, false);
        await check(width, true);
    }
    console.log('PASS: 32-bit and 64-bit simulated processes report live values and reject inactive streams');
})().catch(error => { console.error(error.stack || error); process.exitCode = 1; });
