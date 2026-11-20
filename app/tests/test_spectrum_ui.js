'use strict';
// Execute the production renderer with a recording canvas and a controlled clock.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
let now = 10000, pending, texts = [];
const ctx = {
    scale() {}, beginPath() {}, moveTo() {}, lineTo() {}, stroke() {}, setLineDash() {},
    measureText(text) { return {width: text.length * 6}; },
    fillText(text, x, y) { texts.push({text, x, y}); }
};
const canvas = {clientWidth: 600, getContext: () => ctx};
const elements = {spectrum: canvas, 'spectrum-status': {}, 'spectrum-detail': {}};
const context = vm.createContext({
    latest: null, latestReceivedMs: now,
    document: {getElementById: id => elements[id]},
    window: {devicePixelRatio: 1},
    Date: {now: () => now},
    ResizeObserver: class {observe() {}},
    fetch: async () => ({ok: true, json: async () => ({available:true, age_ms:0, gain_db:20,
        bands:[{start_hz: 5780e6, step_hz: 1e6, values: Array(41).fill(-70), age_ms: Array(41).fill(0)}]})}),
    setTimeout: fn => {pending = fn;}
});
const track = (frequency_hz, standard, age_ms, active = true) => ({frequency_hz, standard, age_ms, active, sync_quality: 95});
async function render(tracks, state = {}) {
    context.latest = {running: true, mode: 'scan', tracks, ...state};
    context.latestReceivedMs = now;
    texts = [];
    await pending();
    return texts.filter(t => /PAL|NTSC/.test(t.text));
}
(async () => {
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../web/spectrum.js'), 'utf8'), context);
    await new Promise(resolve => setImmediate(resolve));
    let labels = await render([track(5800e6, 'PAL', 1000), track(5800e6, 'NTSC', 20)]);
    assert.equal(labels.length, 1, 'one label per frequency even when two standards are recorded');
    assert.match(labels[0].text, /NTSC/);
    labels = await render([track(5800e6, 'PAL', 0, false)]);
    assert.equal(labels.length, 0, 'lost sync must remove a marker without deleting the record');
    labels = await render([track(5800e6, 'PAL', 0)], {running:false});
    assert.equal(labels.length, 0, 'stopped scans have no live markers');
    labels = await render([track(5800e6, 'PAL', 0), track(5801e6, 'NTSC', 0), track(5802e6, 'PAL', 0)]);
    for (let i=0; i<labels.length; i++) for (let j=i+1; j<labels.length; j++) {
        const a=labels[i], b=labels[j];
        assert.ok(Math.abs(a.y-b.y)>=12 || Math.abs(a.x-b.x)>=(a.text.length+b.text.length)*3+4,
            'nearby frequency labels must not overlap');
    }
    now += 4000; texts=[]; await pending();
    assert.equal(texts.filter(t=>/PAL|NTSC/.test(t.text)).length, 0, 'stale status cannot keep live markers');
    console.log('PASS spectrum markers: frequency deduplication, lost sync, stop, collision layout, stale status');
})().catch(e => {console.error(e); process.exitCode=1;});
