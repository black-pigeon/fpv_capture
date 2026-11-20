'use strict';
// Execute the production renderer with a recording canvas and a controlled clock.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
let now = 10000, pending, texts = [], fills = [];
const band = (start, end) => ({start_hz: start * 1e6, end_hz: end * 1e6,
    step_hz: 1e6, values: Array(Math.floor(end - start) + 1).fill(-70),
    age_ms: Array(Math.floor(end - start) + 1).fill(0)});
let bands = [band(5780, 5820)];
const ctx = {
    fillRect(x, y, width, height) { fills.push({x, y, width, height}); },
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
        bands})}),
    setTimeout: fn => {pending = fn;}
});
const track = (frequency_hz, standard, age_ms, active = true) => ({frequency_hz, standard, age_ms, active, sync_quality: 95});
async function render(tracks, state = {}) {
    context.latest = {running: true, mode: 'scan', tracks, ...state};
    context.latestReceivedMs = now;
    texts = []; fills = [];
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
    // Exact configured edges survive JSON display downsampling.
    bands = [band(5650, 5950), band(5200, 5400)];
    bands[1].values.pop(); bands[1].age_ms.pop();
    await render([]);
    assert.ok(texts.some(t => t.text === '//'), 'compressed gaps need an axis break');
    for (const edge of [5200, 5400, 5650, 5950]) {
        assert.ok(texts.some(t => t.text === `${edge} MHz`), `show exact ${edge} MHz boundary`);
    }
    const gap = fills.find(f => Math.abs(f.x - (44 + 200 / 500 * 512)) < .01 &&
        Math.abs(f.width - 28) < .01 && f.height === 182);
    assert.ok(gap, 'cap the gap at 28 pixels and allocate remaining space to scanned bandwidth');
    assert.ok(!texts.some(t => t.y > 202 && Number(t.text) > 5400 && Number(t.text) < 5650),
        'axis ticks must not imply a continuous scale through the compressed gap');
    labels = await render([track(5300e6, 'PAL', 0), track(5800e6, 'NTSC', 0)]);
    assert.ok(Math.abs(labels[0].x - (44 + 100 / 500 * 512)) < .01,
        'FPV markers follow the expanded first band');
    assert.ok(Math.abs(labels[1].x - (44 + 350 / 500 * 512 + 28)) < .01,
        'FPV markers follow the expanded second band');
    bands = [band(5200, 5500), band(5400, 5650), band(5650, 5950)];
    await render([]);
    assert.ok(!texts.some(t => t.text === '//'), 'overlapping and touching bands leave no gap');
    assert.ok(!texts.some(t => t.text === '5500 MHz'), 'interior overlap endpoints are not coverage boundaries');
    bands = [band(5200, 5400), band(5401, 5950)]; canvas.clientWidth = 320;
    await render([]);
    assert.ok(!texts.some(t => t.text === '//'), 'omit gap text when it cannot fit');
    const edges = texts.filter(t => / MHz$/.test(t.text));
    for (let i=0; i<edges.length; i++) for (let j=i+1; j<edges.length; j++) {
        const a=edges[i], b=edges[j];
        assert.ok(Math.abs(a.y-b.y)>=12 || Math.abs(a.x-b.x)>=(a.text.length+b.text.length)*3+4,
            'edge labels must not overlap on narrow screens');
    }
    canvas.clientWidth = 600;
    bands = Array.from({length:8}, (_, i) => band(100 + i * 700, 110 + i * 700));
    await render([]);
    const gapPixels = fills.filter(f => f.height === 182).reduce((sum, f) => sum + f.width, 0);
    assert.ok(Math.abs(gapPixels - 540 * .15) < .01, 'many gaps consume at most 15% of the plot');
    bands = [band(5780, 5820)]; await render([]);
    assert.ok(!texts.some(t => t.text === '//' || / MHz$/.test(t.text)),
        'a single band keeps the compact layout');
    console.log('PASS spectrum band boundaries, gap shading, exact edges, overlaps, narrow screens');
    console.log('PASS spectrum markers: frequency deduplication, lost sync, stop, collision layout, stale status');
})().catch(e => {console.error(e); process.exitCode=1;});
