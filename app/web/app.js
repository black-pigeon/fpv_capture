'use strict';
const $ = id => document.getElementById(id);
const phases = { idle: 'Ready', starting: 'Starting', initializing: 'Initializing RF', tuning: 'Tuning', observing: 'Observing sync', capturing: 'Validating frame', rf_settle: 'RF settling', ddc_tuning: 'Digital tuning', error: 'Scan error' };
let selected = null, currentKey = '', firstStatus = true, busy = false, latest = null;
function notice(text) { $('message').textContent = text; $('message').hidden = !text; }
function frequency(hz) { return (hz / 1e6).toFixed(3); }
async function command(path, body = '') {
    const r = await fetch(path, { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
    if (!r.ok) throw new Error(await r.text());
}
function selectTrack(id) { selected = id; currentKey = ''; if (latest) renderTracks(latest); }
function renderTracks(s) {
    const tracks = [...s.tracks].sort((a, b) => a.frequency_hz - b.frequency_hz);
    if (!tracks.some(t => t.id === selected)) selected = tracks.length ? tracks[0].id : null;
    const rows = $('results'); rows.replaceChildren();
    if (!tracks.length) {
        const row = rows.insertRow(), td = row.insertCell(); td.colSpan = 6; td.className = 'empty-row'; td.textContent = 'No confirmed records yet';
        $('frame').hidden = true; $('empty').hidden = false; $('download').hidden = true;
        $('selected-frequency').textContent = 'No image yet'; $('selected-detail').textContent = 'Select a record below to view its image'; currentKey = '';
    }
    for (const t of tracks) {
        const row = rows.insertRow(); row.dataset.id = t.id; row.tabIndex = 0; row.setAttribute('aria-label', `View frame at ${frequency(t.frequency_hz)} MHz`);
        row.className = t.id === selected ? 'selected' : '';
        const values = [frequency(t.frequency_hz), t.standard, `CH ${t.channel}`, `${t.sync_quality}%`, `${(t.age_ms / 1000).toFixed(1)} s`];
        values.forEach((value, i) => { const cell = row.insertCell(); cell.textContent = value; if (i === 4) cell.className = 'age'; });
        const cell = row.insertCell(), button = document.createElement('button'); button.type = 'button'; button.textContent = 'View →'; cell.append(button);
        row.addEventListener('click', () => selectTrack(t.id));
        row.addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); selectTrack(t.id); } });
        if (t.id === selected) {
            const key = `${t.id}-${t.revision}`, url = `/image/${t.id}.bmp?revision=${t.revision}`;
            if (key !== currentKey) { $('frame').src = url; currentKey = key; }
            $('frame').hidden = false; $('empty').hidden = true;
            $('selected-frequency').textContent = `${frequency(t.frequency_hz)} MHz · ${t.standard}`;
            $('selected-detail').textContent = `CH ${t.channel} · Epoch #${t.epoch} · ${t.width} × ${t.height} · ${t.age_ms < 1000 ? 'Updated just now' : `${(t.age_ms / 1000).toFixed(1)} s ago`}`;
            $('frame-size').textContent = `${t.width} × ${t.height}`;
            $('download').href = url; $('download').download = `fpv-${frequency(t.frequency_hz)}-${t.standard}.bmp`; $('download').hidden = false;
        }
    }
    $('result-count').textContent = String(tracks.length).padStart(2, '0'); $('track-count').textContent = tracks.length;
}
function render(s) {
    latest = s; $('led').className = 'led online'; $('connection').textContent = 'Connected'; $('simulation').hidden = !s.simulate;
    $('phase').textContent = s.stopping ? 'Stopping' : s.phase === 'idle' && s.rounds ? 'Scan stopped' : phases[s.phase] || s.phase;
    $('start').disabled = s.running || busy; $('stop').disabled = !s.running || s.stopping || busy; $('clear').disabled = s.running || busy;
    for (const el of $('scan-form').elements) if (el.tagName !== 'BUTTON' || el.id.startsWith('preset-')) el.disabled = s.running;
    $('progress-label').textContent = `${s.completed_points} / ${s.total_points} points`;
    $('progress').max = s.total_points || 1; $('progress').value = s.completed_points;
    $('lo').textContent = s.lo_hz ? `${frequency(s.lo_hz)} MHz` : '— MHz'; $('rounds').textContent = s.rounds;
    $('tune-counts').textContent = `${s.rf_tunes} RF changes · ${s.ddc_tunes} DDC settings`;
    $('window-phase').textContent = `Digital phase ${s.window_phase} / ${s.window_phases}`;
    const centers = $('channel-centers'); centers.replaceChildren();
    for (const ch of s.channels) {
        const item = document.createElement('span');
        item.textContent = `CH ${ch.channel} · ${frequency(ch.frequency_hz)} MHz (${ch.offset_hz >= 0 ? '+' : ''}${frequency(ch.offset_hz)} MHz)`;
        centers.append(item);
    }
    $('capture-errors').textContent = `Incomplete / rejected DMA frames: ${s.capture_errors}`;
    if (firstStatus) {
        const c = s.config; $('bands').value = c.bands.map(b => `${b[0] / 1e6}-${b[1] / 1e6}`).join('\n');
        $('step').value = c.step_mhz; $('gain').value = c.gain_db; $('dwell').value = c.dwell_ms; $('settle').value = c.settle_ms; $('repeat').checked = !!c.repeat; firstStatus = false; gridNote();
    }
    if (s.error) notice(s.error);
    renderTracks(s);
}
async function poll() {
    try {
        const r = await fetch('/api/status', { cache: 'no-store' }); if (!r.ok) throw new Error('Failed to read device status'); render(await r.json());
    } catch (e) { $('led').className = 'led'; $('connection').textContent = 'Disconnected'; notice(`Cannot connect to device: ${e.message}`); }
    finally { setTimeout(poll, 700); }
}
function gridNote() {
    $('grid-note').textContent = Number($('step').value) > 2
        ? 'Coarse grid: faster search, but off-center signals may be missed. Use Fine to investigate a suspected range.'
        : 'Fine grid: more observation time, smaller carrier offsets.';
}
function preset(step, dwell) {
    if (latest && latest.running) return;
    $('step').value = step; $('dwell').value = dwell; gridNote();
}
$('preset-fine').addEventListener('click', () => preset(2, 150));
$('preset-fast').addEventListener('click', () => preset(8, 100));
$('step').addEventListener('input', gridNote);
$('scan-form').addEventListener('submit', async e => {
    e.preventDefault(); if (busy) return; busy = true; $('start').disabled = true; notice('');
    const params = new URLSearchParams(new FormData($('scan-form'))); params.set('repeat', $('repeat').checked ? '1' : '0');
    try { await command('/api/start', params.toString()); } catch (err) { notice(err.message); }
    finally { busy = false; }
});
$('stop').addEventListener('click', async () => { try { await command('/api/stop'); } catch (e) { notice(e.message); } });
$('clear').addEventListener('click', async () => { try { await command('/api/clear'); } catch (e) { notice(e.message); } });
$('frame').addEventListener('error', () => { currentKey = ''; notice('Selected frame has changed or expired. Waiting for refresh.'); });
poll();
