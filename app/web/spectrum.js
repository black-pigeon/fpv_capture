'use strict';
(() => {
    const canvas = document.getElementById('spectrum');
    let data = null, received = 0;
    function draw() {
        const ratio = window.devicePixelRatio || 1, w = canvas.clientWidth, h = 230;
        canvas.width = Math.round(w * ratio); canvas.height = Math.round(h * ratio);
        const ctx = canvas.getContext('2d'); ctx.scale(ratio, ratio);
        const left = 44, right = w - 16, top = 20, bottom = h - 28;
        const bands = data?.bands || [];
        const lo = bands.length ? Math.min(...bands.map(b => b.start_hz)) : 5650e6;
        const hi = bands.length ? Math.max(...bands.map(b => b.start_hz + b.step_hz * Math.max(1, b.values.length - 1))) : 5950e6;
        const x = hz => left + (hz - lo) / Math.max(1, hi - lo) * (right - left);
        const y = db => top + (0 - Math.max(-100, Math.min(0, db))) / 100 * (bottom - top);
        ctx.font = '11px system-ui'; ctx.lineWidth = 1;
        for (let db = -100; db <= 0; db += 20) {
            ctx.strokeStyle = '#263744';ctx.beginPath();ctx.moveTo(left, y(db));ctx.lineTo(right, y(db));ctx.stroke();
            ctx.fillStyle = '#94a3b8';ctx.textAlign = 'right';ctx.fillText(String(db), left - 7, y(db) + 4);
        }
        ctx.textAlign = 'center';
        for (let i = 0; i <= 4; i++) {
            const hz = lo + (hi - lo) * i / 4;
            ctx.fillText((hz / 1e6).toFixed(hi - lo < 1e8 ? 1 : 0), x(hz), h - 8);
        }
        const elapsed = Date.now() - received;
        for (const b of bands) {
            for (const stale of [true, false]) {
                ctx.strokeStyle = stale ? '#94a3b8' : '#0891b2';ctx.lineWidth = 1.4;ctx.beginPath();let connected = false;
                b.values.forEach((db, i) => {
                    const old = b.age_ms[i] == null || b.age_ms[i] + elapsed > 5000;
                    if (db == null || old !== stale) { connected = false;return; }
                    const px = x(b.start_hz + i * b.step_hz), py = y(db);
                    if (connected) ctx.lineTo(px, py);else ctx.moveTo(px, py);connected = true;
                });ctx.stroke();
            }
        }
        // Historical images may include both standards at one frequency. Live
        // markers use detector state; a failed status poll must not freeze them.
        const state = typeof latest !== 'undefined' ? latest : null;
        const fresh = typeof latestReceivedMs !== 'undefined' && Date.now() - latestReceivedMs <= 3000;
        const markers = new Map();
        if (fresh && state?.running && !state.stopping && !state.switching && !state.error) {
            for (const t of state.tracks || []) {
                if (!t.active || !Number.isFinite(t.frequency_hz) || t.frequency_hz < lo || t.frequency_hz > hi) continue;
                const key = Math.round(t.frequency_hz / 1e5), prior = markers.get(key);
                if (!prior || t.age_ms < prior.age_ms || (t.age_ms === prior.age_ms && t.sync_quality > prior.sync_quality)) markers.set(key, t);
            }
        }
        const lanes = [[], [], []];
        for (const t of [...markers.values()].sort((a, b) => a.frequency_hz - b.frequency_hz)) {
            const px = x(t.frequency_hz), text = `${(t.frequency_hz / 1e6).toFixed(1)} ${t.standard}`;
            const width = ctx.measureText(text).width, half = width / 2;
            const labelX = Math.max(left + half, Math.min(right - half, px));
            const lane = lanes.findIndex(spans => spans.every(span => labelX - half > span[1] + 6 || labelX + half < span[0] - 6));
            // Dense clusters keep their frequency lines even when all text lanes
            // are occupied. Never draw unreadable text on top of another label.
            const lineTop = lane < 0 ? top : top + lane * 14;
            ctx.strokeStyle = '#d97706';ctx.setLineDash([3, 3]);
            ctx.beginPath();ctx.moveTo(px, lineTop);ctx.lineTo(px, bottom);ctx.stroke();ctx.setLineDash([]);
            if (lane < 0) continue;
            lanes[lane].push([labelX - half, labelX + half]);
            ctx.fillStyle = '#d97706';ctx.textAlign = 'center';ctx.fillText(text, labelX, lineTop - 5);
        }
    }
    async function pollSpectrum() {
        try {
            const r = await fetch('/api/spectrum', {cache: 'no-store'});if (!r.ok) throw new Error('unavailable');
            data = await r.json();received = Date.now();
            const status = document.getElementById('spectrum-status');
            status.textContent = data.available ? `${(data.age_ms / 1000).toFixed(1)} s ago${data.error ? ' · Capture error' : ''}` : data.error ? 'Unavailable' : 'Waiting';
            document.getElementById('spectrum-detail').textContent = data.available ? `2048 FFT · 30 kHz · Gain ${data.gain_db} dB` : '2048 FFT · 30 kHz bins';
            draw();
        } catch (_) { document.getElementById('spectrum-status').textContent = 'Unavailable'; draw(); }
        setTimeout(pollSpectrum, typeof latest !== 'undefined' && latest?.running && latest?.mode === 'lock' ? 100 : 500);
    }
    new ResizeObserver(draw).observe(canvas);pollSpectrum();
})();
