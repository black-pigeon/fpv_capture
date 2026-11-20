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
        // Use configured endpoints: the final downsampled FFT bucket can stop
        // short of the band edge. Merge coverage before marking unscanned gaps.
        const coverage = [];
        for (const b of [...bands].sort((a, b) => a.start_hz - b.start_hz)) {
            const end = b.end_hz ?? b.start_hz + b.step_hz * Math.max(1, b.values.length - 1);
            const previous = coverage[coverage.length - 1];
            if (previous && b.start_hz <= previous.end) previous.end = Math.max(previous.end, end);
            else coverage.push({start: b.start_hz, end});
        }
        const lo = coverage.length ? coverage[0].start : 5650e6;
        const hi = coverage.length ? coverage[coverage.length - 1].end : 5950e6;
        // Compress unscanned gaps; retain a proportional scale within all
        // scanned regions. Gap shading and // make the broken axis explicit.
        const gapWidths = coverage.slice(1).map((b, i) =>
            Math.min(28, (b.start - coverage[i].end) / Math.max(1, hi - lo) * (right - left)));
        const gapTotal = gapWidths.reduce((sum, width) => sum + width, 0);
        const gapScale = gapTotal ? Math.min(1, (right - left) * 0.15 / gapTotal) : 1;
        const scanWidth = right - left - gapTotal * gapScale;
        const scanHz = coverage.reduce((sum, b) => sum + Math.max(1, b.end - b.start), 0);
        let cursor = left;
        coverage.forEach((b, i) => {
            if (i) cursor += gapWidths[i - 1] * gapScale;
            b.left = cursor; cursor += Math.max(1, b.end - b.start) / scanHz * scanWidth; b.right = cursor;
        });
        const x = hz => {
            for (let i = 0; i < coverage.length; i++) {
                const b = coverage[i];
                if (hz <= b.end) {
                    if (hz < b.start && i) {
                        const previous = coverage[i - 1];
                        return previous.right + (hz - previous.end) / (b.start - previous.end) * (b.left - previous.right);
                    }
                    return b.left + (hz - b.start) / Math.max(1, b.end - b.start) * (b.right - b.left);
                }
            }
            return coverage.length ? right : left + (hz - lo) / Math.max(1, hi - lo) * (right - left);
        };
        const y = db => top + (0 - Math.max(-100, Math.min(0, db))) / 100 * (bottom - top);
        ctx.font = '11px system-ui'; ctx.lineWidth = 1;
        for (let i = 1; i < coverage.length; i++) {
            const start = x(coverage[i - 1].end), end = x(coverage[i].start);
            ctx.fillStyle = '#20303d'; ctx.fillRect(start, top, end - start, bottom - top);
            ctx.strokeStyle = '#3b4d5b'; ctx.beginPath();
            // Bound each hatch segment explicitly, including subpixel gaps.
            for (let px = start - (bottom - top); px < end; px += 12) {
                const a = Math.max(start, px), b = Math.min(end, px + bottom - top);
                ctx.moveTo(a, bottom - (a - px)); ctx.lineTo(b, bottom - (b - px));
            }
            ctx.stroke();
            if (end - start >= ctx.measureText('//').width + 6) {
                ctx.fillStyle = '#cbd5e1'; ctx.textAlign = 'center';
                ctx.fillText('//', (start + end) / 2, (top + bottom) / 2 + 4);
            }
        }
        for (let db = -100; db <= 0; db += 20) {
            ctx.strokeStyle = '#263744';ctx.beginPath();ctx.moveTo(left, y(db));ctx.lineTo(right, y(db));ctx.stroke();
            ctx.fillStyle = '#94a3b8';ctx.textAlign = 'right';ctx.fillText(String(db), left - 7, y(db) + 4);
        }
        ctx.textAlign = 'center';
        const ticks = [];
        if (coverage.length > 1) {
            for (const b of coverage) {
                const count = Math.max(1, Math.floor((b.right - b.left) / 100));
                for (let i = 0; i <= count; i++) ticks.push(b.start + (b.end - b.start) * i / count);
            }
        } else for (let i = 0; i <= 4; i++) ticks.push(lo + (hi - lo) * i / 4);
        let lastTickRight = -Infinity;
        for (const hz of ticks) {
            const text = (hz / 1e6).toFixed(hi - lo < 1e8 ? 1 : 0), half = ctx.measureText(text).width / 2;
            if (x(hz) - half < lastTickRight + 8) continue;
            ctx.fillText(text, x(hz), h - 8); lastTickRight = x(hz) + half;
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
        if (bands.length > 1) {
            const edgeLanes = [[], []];
            for (const hz of coverage.flatMap(b => [b.start, b.end])) {
                const px = x(hz), text = `${Number((hz / 1e6).toFixed(3))} MHz`;
                ctx.strokeStyle = '#67e8f9'; ctx.lineWidth = 1.5; ctx.setLineDash([5, 4]);
                ctx.beginPath(); ctx.moveTo(px, top); ctx.lineTo(px, bottom); ctx.stroke(); ctx.setLineDash([]);
                ctx.fillStyle = '#67e8f9'; ctx.fillRect(px - 2, top, 4, 6); ctx.fillRect(px - 2, bottom - 6, 4, 6);
                const half = ctx.measureText(text).width / 2;
                const labelX = Math.max(left + half + 4, Math.min(right - half - 4, px));
                const lane = edgeLanes.findIndex(spans => spans.every(span => labelX - half > span[1] + 8 || labelX + half < span[0] - 8));
                if (lane < 0 || half * 2 + 8 > right - left) continue;
                edgeLanes[lane].push([labelX - half, labelX + half]);
                const labelY = bottom - 8 - lane * 16;
                ctx.fillStyle = '#101d29'; ctx.fillRect(labelX - half - 3, labelY - 11, half * 2 + 6, 14);
                ctx.fillStyle = '#a5f3fc'; ctx.textAlign = 'center'; ctx.fillText(text, labelX, labelY);
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
