'use strict';
const $ = id => document.getElementById(id);
const phases = { idle: 'Ready', starting: 'Starting', initializing: 'Initializing RF', tuning: 'Tuning', observing: 'Observing sync', capturing: 'Validating frame', rf_settle: 'RF settling', ddc_tuning: 'Digital tuning', waiting_signal: 'Waiting for video sync', gain_update: 'Applying gain', error: 'Error' };
let latestReceivedMs = 0;
let selected = null, currentKey = '', firstStatus = true, busy = false, latest = null, frameLoading = false, gainBusy = false, gainDirty = false;
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
    if (s.mode === 'lock') {
        const newest = tracks.filter(t => t.target_hz === s.lock_hz).sort((a, b) => a.age_ms - b.age_ms)[0];
        if (newest) selected = newest.id;
    }
    const rows = $('results'); rows.replaceChildren();
    if (!tracks.length) {
        const row = rows.insertRow(), td = row.insertCell(); td.colSpan = 6; td.className = 'empty-row'; td.textContent = 'No records';
        $('frame').hidden = true; $('empty').hidden = false; $('download').hidden = true;
        $('selected-frequency').textContent = 'No image'; $('selected-detail').textContent = ''; currentKey = '';
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
            if (key !== currentKey && !frameLoading) { frameLoading = true; $('frame').src = url; currentKey = key; }
            $('frame').hidden = false; $('empty').hidden = true;
            $('selected-frequency').textContent = `${frequency(t.frequency_hz)} MHz · ${t.standard}`;
            $('selected-detail').textContent = `CH ${t.channel} · ${t.width} × ${t.height} · ${t.age_ms < 1000 ? 'Updated just now' : `${(t.age_ms / 1000).toFixed(1)} s ago`}`;
            $('frame-size').textContent = `${t.width} × ${t.height}`;
            $('download').href = url; $('download').download = `fpv-${frequency(t.frequency_hz)}-${t.standard}.bmp`; $('download').hidden = false;
        }
    }
    $('lock-selected').disabled = selected === null || busy || s.stopping || s.switching;
    $('result-count').textContent = String(tracks.length).padStart(2, '0'); $('track-count').textContent = tracks.length;
}
function render(s, fresh = false) {
    latest = s; if (fresh) latestReceivedMs = Date.now(); $('led').className = 'led online'; $('connection').textContent = 'Connected'; $('simulation').hidden = !s.simulate;
    const locked = s.mode === 'lock', switching = s.switching || s.stopping;
    $('phase').textContent = s.stopping ? 'Stopping' : s.switching ? 'Switching mode' :
        s.frequency_locked ? (s.signal_present ? 'Locked · Video' : 'Locked · Waiting') : phases[s.phase] || s.phase;
    $('start').disabled = (s.running && !locked) || switching || busy;
    $('start').firstChild.textContent = locked && s.running ? 'Resume scan ' : 'Start scan ';
    $('stop').disabled = !s.running || switching || busy;
    $('stop').textContent = locked ? 'Stop capture' : 'Stop scan';
    $('clear').disabled = s.running || busy;
    $('lock').disabled = switching || busy || gainBusy;
    $('unlock').disabled = !locked || !s.running || switching || busy;
    $('lock-frequency').disabled = switching || busy;
    for (const el of $('scan-form').elements)
        if (el.tagName !== 'BUTTON' || el.id.startsWith('preset-')) el.disabled = s.running || switching || busy;
    $('progress-title').textContent = locked ? 'Fixed frequency' : 'Scan progress';
    const percent = s.total_points ? Math.floor(100 * s.completed_points / s.total_points) : 0;
    $('progress-label').textContent = locked ? `${frequency(s.lock_hz)} MHz` : `${percent}%`;
    $('progress').hidden = locked;
    $('progress').max = s.total_points || 1; $('progress').value = s.completed_points;
    $('lo').textContent = s.lo_hz ? `${frequency(s.lo_hz)} MHz` : '— MHz';
    $('rounds-title').textContent = locked ? 'Frames' : 'Passes';
    $('rounds').textContent = locked ? s.captured_frames : s.rounds;
    $('tune-counts').textContent = `${s.rf_tunes} RF changes · ${s.ddc_tunes} DDC settings`;
    $('window-phase').textContent = locked ? 'Fixed CH 0 · automatic PAL / NTSC' : `Digital phase ${s.window_phase} / ${s.window_phases}`;
    $('lock-status').textContent = locked && s.running ? `${frequency(s.lock_hz)} MHz` : 'Not locked';
    $('gain').disabled = switching || busy || gainBusy;
    $('apply-gain').disabled = switching || busy || gainBusy || s.gain_pending;
    $('gain-status').textContent = s.gain_pending ? `Applying ${s.gain_target_db} dB…` : `${s.config.gain_db} dB${s.running ? ' applied' : ''}`;
    if (!gainDirty && !gainBusy && document.activeElement !== $('gain')) $('gain').value = s.gain_target_db;
    const centers = $('channel-centers'); centers.replaceChildren();
    for (const ch of s.channels) {
        const item = document.createElement('span');
        item.textContent = `CH ${ch.channel} · ${frequency(ch.frequency_hz)} MHz (${ch.offset_hz >= 0 ? '+' : ''}${frequency(ch.offset_hz)} MHz)`;
        centers.append(item);
    }
    $('capture-errors').textContent = `Capture errors: ${s.capture_errors}`;
    if (firstStatus) {
        const c = s.config; $('bands').value = c.bands.map(b => `${b[0] / 1e6}-${b[1] / 1e6}`).join('\n');
        $('step').value = c.step_mhz; $('gain').value = c.gain_db; $('dwell').value = c.dwell_ms; $('settle').value = c.settle_ms; $('repeat').checked = !!c.repeat; if (s.mode === 'lock') $('lock-frequency').value = s.lock_hz / 1e6; firstStatus = false;
    }
    if (s.error) notice(s.error);
    renderTracks(s);
}
async function poll() {
    try {
        const r = await fetch('/api/status', { cache: 'no-store' }); if (!r.ok) throw new Error('Failed to read device status'); render(await r.json(), true);
    } catch (e) { $('led').className = 'led'; $('connection').textContent = 'Disconnected'; notice(`Cannot connect to device: ${e.message}`); }
    finally { setTimeout(poll, latest && latest.running && latest.mode === 'lock' ? 200 : 700); }
}
function preset(step, dwell) {
    if (latest && latest.running) return;
    $('step').value = step; $('dwell').value = dwell;
}
$('preset-fine').addEventListener('click', () => preset(2, 150));
$('preset-fast').addEventListener('click', () => preset(8, 100));

// Read values explicitly: scan controls are disabled during a running scan
// but their settings must still accompany a transition to fixed capture.
function scanParams() {
    return new URLSearchParams({ bands: $('bands').value, step_mhz: $('step').value,
        gain_db: $('gain').value, dwell_ms: $('dwell').value, settle_ms: $('settle').value,
        repeat: $('repeat').checked ? '1' : '0' });
}
async function startMode(locked) {
    if (busy) return;
    busy = true; notice(''); if (latest) render(latest);
    const params = scanParams(); params.set('mode', locked ? 'lock' : 'scan');
    if (locked) params.set('frequency_mhz', $('lock-frequency').value);
    try {
        await command('/api/start', params.toString());
        const response = await fetch('/api/status', { cache: 'no-store' });
        if (response.ok) { latest = await response.json(); latestReceivedMs = Date.now(); }
    } catch (err) { notice(err.message); }
    finally { busy = false; if (latest) render(latest); }
}
$('gain').addEventListener('input', () => { gainDirty = true; });
$('gain-form').addEventListener('submit', async e => {
    e.preventDefault();
    if (gainBusy || !$('gain-form').reportValidity()) return;
    const value = $('gain').value;
    gainBusy = true; notice(''); if (latest) render(latest);
    try {
        await command('/api/gain', new URLSearchParams({ gain_db: value }).toString());
        gainDirty = false;
        const response = await fetch('/api/status', { cache: 'no-store' });
        if (response.ok) { latest = await response.json(); latestReceivedMs = Date.now(); }
    } catch (err) { notice(err.message); }
    finally { gainBusy = false; if (latest) render(latest); }
});
$('scan-form').addEventListener('submit', e => { e.preventDefault(); startMode(false); });
$('lock-form').addEventListener('submit', e => { e.preventDefault(); startMode(true); });
$('unlock').addEventListener('click', () => startMode(false));
$('lock-selected').addEventListener('click', () => {
    const track = latest && latest.tracks.find(t => t.id === selected);
    if (!track) return;
    $('lock-frequency').value = track.target_hz / 1e6;
    startMode(true);
});
$('stop').addEventListener('click', async () => { try { await command('/api/stop'); } catch (e) { notice(e.message); } });
$('clear').addEventListener('click', async () => { try { await command('/api/clear'); } catch (e) { notice(e.message); } });
$('frame').addEventListener('load', () => { frameLoading = false; });
$('frame').addEventListener('error', () => { frameLoading = false; currentKey = ''; notice('Selected frame has changed or expired. Waiting for refresh.'); });
poll();
