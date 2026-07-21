"use strict";

const $ = id => document.getElementById(id);
let config = {};
let status = {};

/* ---------- navigation ---------- */
function showPage(name) {
    if (!$("page-" + name)) return;
    document.querySelectorAll(".tab").forEach(t => t.classList.toggle("active", t.dataset.page === name));
    document.querySelectorAll(".page").forEach(p => p.classList.remove("active"));
    $("page-" + name).classList.add("active");
    history.replaceState(null, "", "#" + name);
    if (name === "settings") loadNetwork();
    if (name === "receiver") loadSenders();
}
document.querySelectorAll(".tab").forEach(tab => { tab.onclick = () => showPage(tab.dataset.page); });
if (location.hash.length > 1) showPage(location.hash.slice(1));

/* ---------- helpers ---------- */
const STATE_NAMES = ["Inactive", "Healthy", "Partially healthy", "Unhealthy"];
const STATE_CLASS = ["off", "ok", "warn", "bad"];
const LINK_NAMES = {1: "All up", 2: "Some down", 3: "All down"};

function pill(el, text, cls) {
    el.textContent = text;
    el.className = "pill " + cls;
}
function meterWidth(db) {
    return Math.max(0, Math.min(100, (db + 60) / 60 * 100)) + "%";
}
function fmtNs(ns) {
    if (ns === null || ns === undefined) return "–";
    const abs = Math.abs(ns);
    if (abs > 1e9) return (ns / 1e9).toFixed(3) + " s";
    if (abs > 1e6) return (ns / 1e6).toFixed(2) + " ms";
    if (abs > 1e3) return (ns / 1e3).toFixed(1) + " µs";
    return Math.round(ns) + " ns";
}

async function api(path, method, body) {
    const r = await fetch(path, {
        method: method || "GET",
        headers: body ? {"Content-Type": "application/json"} : {},
        body: body ? JSON.stringify(body) : undefined
    });
    return r.ok ? r.json().catch(() => ({})) : Promise.reject(await r.text());
}
const setConfig = updates => api("/api/config", "POST", updates).then(loadConfig);

/* ---------- websocket status feed ---------- */
function connectWs() {
    const ws = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws");
    ws.onopen = () => $("conn-dot").classList.add("on");
    ws.onclose = () => { $("conn-dot").classList.remove("on"); setTimeout(connectWs, 2000); };
    ws.onmessage = e => {
        const msg = JSON.parse(e.data);
        if (msg.meters && !msg.version) { applyMeters(msg.meters); return; }   //20 Hz fast path
        status = msg;
        render();
    };
}

function applyMeters(m) {
    const rxOn = status.receiver && status.receiver.running;
    const txOn = status.sender && status.sender.running;
    $("d-rx-meter-l").style.width = meterWidth(rxOn ? m.rx[0] : -120);
    $("d-rx-meter-r").style.width = meterWidth(rxOn ? m.rx[1] : -120);
    $("d-tx-meter-l").style.width = meterWidth(txOn ? m.tx[0] : -120);
    $("d-tx-meter-r").style.width = meterWidth(txOn ? m.tx[1] : -120);
}

/* ---------- render ---------- */
let buildSeen = null;
function render() {
    /* the kiosk browser never reloads on its own - pick up new ui builds */
    if (status.build) {
        if (buildSeen && status.build !== buildSeen) { location.reload(); return; }
        buildSeen = status.build;
    }
    const ptp = status.ptp || {};
    const rx = status.receiver || {};
    const tx = status.sender || {};
    const mon = status.monitors || {};

    /* dashboard */
    pill($("d-ptp-state"), ptp.synced ? "Locked" : (ptp.masters && ptp.masters.length ? "Syncing…" : "No master"),
         ptp.synced ? "ok" : "warn");
    const gm = (ptp.masters || []).find(m => m.selected);
    $("d-ptp-gm").textContent = gm ? gm.grandmaster : "–";
    $("d-ptp-offset").textContent = ptp.synced ? fmtNs(ptp.correction_ns) : "–";

    pill($("d-rx-state"), rx.running ? (rx.receiving ? "Receiving" : "No signal") : "Off",
         rx.running ? (rx.receiving ? "ok" : "bad") : "off");
    $("d-rx-meter-l").style.width = meterWidth(rx.running && rx.meters ? rx.meters.left_db : -120);
    $("d-rx-meter-r").style.width = meterWidth(rx.running && rx.meters ? rx.meters.right_db : -120);
    $("d-rx-info").textContent = rx.running
        ? `${rx.session_name || ""} · ${(rx.legs || []).length} leg(s) · ${rx.played || 0} packets`
        : "patch via NMOS or paste an SDP";

    pill($("d-tx-state"), tx.running ? (tx.waiting_for_ptp ? "Waiting for PTP" : "Sending") : "Off",
         tx.running ? (tx.waiting_for_ptp ? "warn" : "ok") : "off");
    $("d-tx-meter-l").style.width = meterWidth(tx.running && tx.meters ? tx.meters.left_db : -120);
    $("d-tx-meter-r").style.width = meterWidth(tx.running && tx.meters ? tx.meters.right_db : -120);
    $("d-tx-info").textContent = tx.running ? `${tx.source} · ${tx.packets_sent} packets` : "–";

    const nmos = status.nmos || {};
    $("d-nmos-registry").textContent = nmos.registry || "no registry";
    $("d-nmos-status").textContent = (nmos.status || "–") + " · Node " + (nmos.node_id || "").slice(0, 8) + "…";

    /* receiver page */
    const rmon = mon.receiver || {};
    pill($("rx-overall"), STATE_NAMES[rmon.overall || 0], STATE_CLASS[rmon.overall || 0]);
    const domains = [["link", "Link"], ["path", "Connection"], ["sync", "PTP sync"], ["stream", "Stream"]];
    $("rx-domains").innerHTML = domains.map(([key, name]) => {
        const d = rmon[key] || {status: 0, message: "", transitions: 0};
        const label = key === "link" ? (LINK_NAMES[d.status] || "–") : STATE_NAMES[d.status];
        return `<div class="domain"><div class="name">${name}</div>
                <div class="state" style="color:var(--${STATE_CLASS[d.status] === "off" ? "off" : STATE_CLASS[d.status]})">${label}</div>
                <div class="msg">${d.message || (d.transitions ? d.transitions + " transitions" : "")}</div></div>`;
    }).join("");

    $("rx-legs").innerHTML = (rx.legs || []).map((leg, i) =>
        `<div class="leg"><span class="pill ${leg.active ? "ok" : "bad"}">${i ? "SEC" : "PRI"}</span>
         <span class="ifname">${leg.interface}</span><span class="addr">${leg.multicast}</span>
         <span>${leg.received} rx · ${leg.lost} lost</span></div>`).join("")
        || `<div class="sub">no active reception</div>`;

    /* keep the gain slider in sync with changes made from other browsers,
       unless the user is moving it right now */
    if (rx.gain_db !== undefined && Date.now() - gainTouchedAt > 1500
        && parseFloat($("rx-gain").value) !== rx.gain_db) {
        $("rx-gain").value = rx.gain_db;
        $("rx-gain-val").textContent = rx.gain_db + " dB";
    }

    drawBufferChart(rx.buffer_history || [], rx.critical_ms || 2, rx.running && rx.receiving);
    renderMatrix(rx);

    /* the connected badge in the sender list follows is-05/registry state
       live, e.g. when an external controller patches this receiver */
    const curSender = (status.nmos || {}).connected_sender_id;
    if (curSender !== lastCurSender) {
        lastCurSender = curSender;
        if (sendersCache.length) renderSenders();
    }

    $("rx-stats").innerHTML =
        `<tr><td>Played</td><td>${rx.played || 0}</td></tr>
         <tr><td>Concealed (loss)</td><td>${rx.concealed || 0}</td></tr>
         <tr><td>Merge duplicates</td><td>${rx.duplicates_merged || 0}</td></tr>
         <tr><td>Saved by leg 2</td><td>${rx.from_secondary || 0}</td></tr>
         <tr><td>ALSA buffer</td><td>${status.alsa ? Math.round(status.alsa.delay_frames / 48) : 0} ms</td></tr>
         <tr><td>Underruns</td><td>${status.alsa ? status.alsa.underruns : 0}</td></tr>`;

    /* sender page */
    pill($("tx-state"), tx.running ? (tx.essence_ok ? "Sending" : "Source empty") : "Off",
         tx.running ? (tx.essence_ok ? "ok" : "warn") : "off");

    /* transport fields follow config changes made via nmos/is-05,
       unless the user is editing them right now */
    const cfg = status.config;
    if (cfg && Date.now() - cfgTouchedAt > 1500) {
        config = cfg;
        const sync = (el, v) => { if (document.activeElement !== el && el.value != v) el.value = v; };
        sync($("tx-mc1"), cfg.sender.multicast_primary);
        sync($("tx-mc2"), cfg.sender.multicast_secondary);
        sync($("tx-port"), cfg.sender.port);
        if (document.activeElement !== $("tx-leg1")) $("tx-leg1").checked = cfg.sender.leg1_enabled !== false;
        if (document.activeElement !== $("tx-leg2")) $("tx-leg2").checked = cfg.sender.leg2_enabled !== false;
    }
    $("tx-stats").innerHTML =
        `<tr><td>Packets</td><td>${tx.packets_sent || 0}</td></tr>
         <tr><td>Send errors</td><td>${tx.send_errors || 0}</td></tr>
         <tr><td>TX timing</td><td>${tx.running ? "avg " + (tx.late_avg_us || 0).toFixed(0) + " µs · peak " +
             (tx.late_max_us || 0).toFixed(0) + " µs (5 s)" : "–"}</td></tr>
         <tr><td>Source</td><td>${tx.source || "–"}</td></tr>` +
        (tx.legs || []).map((leg, i) =>
            `<tr><td>Leg ${i + 1}</td><td>${leg.multicast}:${leg.port} @ ${leg.interface}` +
            (leg.enabled === false ? ' <span class="pill off">RTP off</span>' : "") + `</td></tr>`).join("");

    /* ptp page */
    pill($("ptp-state"), ptp.synced ? "Locked" : "Not synced", ptp.synced ? "ok" : "warn");
    ptpClock = ptp.synced && ptp.time_ms ? {ms: ptp.time_ms, at: performance.now(), utc: ptp.utc_offset ?? 37} : null;
    $("ptp-identity").textContent = ptp.identity || "–";
    $("ptp-offset").textContent = ptp.synced ? fmtNs(ptp.correction_ns) : "–";
    $("ptp-delay").textContent = fmtNs(ptp.mean_path_delay_ns);
    /* meta bmca: do amber and blue agree on the grandmaster? */
    const meta = ptp.meta;
    $("ptp-meta").style.display = meta ? "" : "none";
    if (meta) {
        const ok = meta.match === true;
        $("ptp-meta").innerHTML = `Meta BMCA: <span class="pill amber">A</span> ${esc(meta.amber_gm) || "–"}
            · <span class="pill blue">B</span> ${esc(meta.blue_gm) || "–"}
            <span class="pill ${meta.match === null ? "warn" : ok ? "ok" : "bad"}">${
                meta.match === null ? esc(meta.detail) : ok ? (meta.detail ? "same GM · " + esc(meta.detail) : "match") : "GM MISMATCH"}</span>`;
    }

    const tbody = $("ptp-masters").querySelector("tbody");
    tbody.innerHTML = (ptp.masters || []).map(m =>
        `<tr class="${m.selected ? "selected" : ""}">
         <td>${m.selected ? '<span class="crown">★ Master</span>' : ""}</td>
         <td>${m.port_identity}<br><span class="sub">${m.address}</span></td>
         <td>${m.grandmaster}</td>
         <td>${m.priority1}</td><td>${m.clock_class}</td><td>0x${m.clock_accuracy.toString(16)}</td>
         <td>0x${m.variance.toString(16)}</td><td>${m.priority2}</td><td>${m.steps_removed}</td>
         <td>${m.announces}</td>
         <td>${m.nets ? (m.nets.amber ? '<span class="pill amber">A</span>' : "") +
                (m.nets.blue ? ' <span class="pill blue">B</span>' : "") +
                (m.net_match === false ? ' <span class="pill bad">≠</span>' : "") : ""}</td>
         <td class="sub">${m.bmca}</td></tr>`).join("")
        || `<tr><td colspan="12" class="sub">no announce messages on domain ${ptp.domain}</td></tr>`;
    drawChart(ptp.offset_history || []);
}

/* ---------- receiver buffer chart ---------- */
function drawBufferChart(history, criticalMs, live) {
    const canvas = $("rx-buffer-chart");
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    const w = canvas.width = canvas.clientWidth;
    const h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    ctx.font = "11px sans-serif";
    if (history.length < 2) {
        ctx.fillStyle = "#8b93a7";
        ctx.fillText(live ? "collecting…" : "no active reception", 6, 14);
        return;
    }

    /* quantized scale so the chart does not rescale on every sample */
    const need = Math.max(criticalMs * 2, 10, ...history.map(s => s[1])) * 1.15;
    const STEPS = [10, 20, 30, 50, 75, 100, 150, 200, 300, 500];
    const top = STEPS.find(v => v >= need) || Math.ceil(need / 500) * 500;
    const y = ms => h - 4 - (ms / top) * (h - 20);
    const x = i => i / (history.length - 1) * w;

    /* min/max band: network jitter shows as band width and dips */
    ctx.beginPath();
    history.forEach((s, i) => { i ? ctx.lineTo(x(i), y(s[1])) : ctx.moveTo(x(i), y(s[1])); });
    for (let i = history.length - 1; i >= 0; i--) ctx.lineTo(x(i), y(history[i][0]));
    ctx.closePath();
    ctx.fillStyle = "rgba(77,163,255,.22)";
    ctx.fill();

    /* last-value line */
    ctx.strokeStyle = "#4da3ff";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    history.forEach((s, i) => { i ? ctx.lineTo(x(i), y(s[2])) : ctx.moveTo(x(i), y(s[2])); });
    ctx.stroke();

    /* critical line: below this the playout starts concealing */
    ctx.fillStyle = "rgba(231,76,60,.12)";
    ctx.fillRect(0, y(criticalMs), w, h - 4 - y(criticalMs));
    ctx.strokeStyle = "#e74c3c";
    ctx.setLineDash([5, 4]);
    ctx.beginPath(); ctx.moveTo(0, y(criticalMs)); ctx.lineTo(w, y(criticalMs)); ctx.stroke();
    ctx.setLineDash([]);

    const last = history[history.length - 1];
    ctx.fillStyle = "#e74c3c";
    ctx.fillText("critical < " + criticalMs.toFixed(0) + " ms", 6, y(criticalMs) - 5);
    ctx.fillStyle = "#8b93a7";
    ctx.fillText("now " + last[2].toFixed(1) + " ms · min " + last[0].toFixed(1) + " · max " + last[1].toFixed(1)
        + " · 30 s window · scale 0–" + top + " ms", 6, 14);
}

/* ---------- monitor matrix ---------- */
let matrixCh = -1;
function renderMatrix(rx) {
    const n = rx.running ? rx.channels || 0 : 0;
    if (n < 2) { $("rx-matrix-card").style.display = "none"; matrixCh = -1; return; }
    $("rx-matrix-card").style.display = "";

    if (n !== matrixCh) {       //rebuild only when the channel count changes
        matrixCh = n;
        $("rx-matrix").innerHTML = [["L", "receiver.monitor_left"], ["R", "receiver.monitor_right"]].map(([ear, key]) =>
            `<div class="row seg"><span class="ear">${ear}</span>` +
            Array.from({length: n}, (_, c) =>
                `<button class="btn" data-key="${key}" data-ch="${c}">${c + 1}</button>`).join("") +
            `</div>`).join("");
        document.querySelectorAll("#rx-matrix .btn").forEach(btn => {
            btn.onclick = () => setConfig({[btn.dataset.key]: parseInt(btn.dataset.ch, 10)});
        });
    }
    const mon = rx.monitor || [0, 1];
    document.querySelectorAll("#rx-matrix .btn").forEach(btn => {
        const ear = btn.dataset.key === "receiver.monitor_left" ? 0 : 1;
        btn.classList.toggle("active", mon[ear] == btn.dataset.ch);
    });
}

/* ---------- ptp clock ---------- */
let ptpClock = null;    //snapshot of the last status push, extrapolated locally
setInterval(() => {
    const el = $("ptp-time");
    if (!ptpClock) { el.textContent = "PTP time: – (not synced)"; return; }
    const p = n => String(n).padStart(2, "0");
    const tai = new Date(ptpClock.ms + (performance.now() - ptpClock.at));
    const utc = new Date(tai.getTime() - ptpClock.utc * 1000);
    el.textContent = `${tai.getUTCFullYear()}-${p(tai.getUTCMonth() + 1)}-${p(tai.getUTCDate())} `
        + `${p(tai.getUTCHours())}:${p(tai.getUTCMinutes())}:${p(tai.getUTCSeconds())} TAI`
        + ` · ${p(utc.getUTCHours())}:${p(utc.getUTCMinutes())}:${p(utc.getUTCSeconds())} UTC (TAI−${ptpClock.utc}s)`;
}, 250);

/* ---------- offset chart ---------- */
function drawChart(history) {
    const canvas = $("ptp-chart");
    const ctx = canvas.getContext("2d");
    const w = canvas.width = canvas.clientWidth;
    const h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    if (history.length < 2) return;

    const values = history.map(p => p[1]);
    const max = Math.max(1000, ...values.map(Math.abs));
    ctx.strokeStyle = "#2a3245";
    ctx.beginPath(); ctx.moveTo(0, h / 2); ctx.lineTo(w, h / 2); ctx.stroke();
    ctx.strokeStyle = "#4da3ff";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    values.forEach((v, i) => {
        const x = i / (values.length - 1) * w;
        const y = h / 2 - (v / max) * (h / 2 - 6);
        i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
    });
    ctx.stroke();
    ctx.fillStyle = "#8b93a7";
    ctx.font = "11px sans-serif";
    ctx.fillText("±" + fmtNs(max), 6, 14);
}

/* ---------- nmos sender browser ---------- */
const esc = s => String(s ?? "").replace(/[&<>"]/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;"}[c]));

let sendersCache = [];
let lastCurSender = null;
const sndOpen = new Set();      //expanded device groups, kept across refreshes

async function loadSenders() {
    /* refresh silently once a list is shown - collapsing to "loading…"
       makes the whole page jump */
    if (!sendersCache.length) $("rx-senders").textContent = "loading…";
    let r;
    try { r = await api("/api/action/nmos-senders", "POST", {}); }
    catch (e) { if (!sendersCache.length) $("rx-senders").textContent = "query failed"; return; }
    if (r.error) { if (!sendersCache.length) $("rx-senders").textContent = r.error; return; }
    sendersCache = r.senders || [];

    /* the group with the active connection starts expanded */
    const cur = (status.nmos || {}).connected_sender_id;
    const conn = sendersCache.find(s => s.id === cur);
    if (conn) sndOpen.add(conn.device || "(no device)");
    renderSenders();
}

function renderSenders() {
    if (!sendersCache.length) { $("rx-senders").textContent = "no audio senders in the registry"; return; }
    const filter = ($("rx-filter").value || "").trim().toLowerCase();
    const cur = (status.nmos || {}).connected_sender_id;

    const groups = new Map();
    sendersCache.forEach((s, i) => {
        if (filter && !(s.label + " " + s.device).toLowerCase().includes(filter)) return;
        const dev = s.device || "(no device)";
        if (!groups.has(dev)) groups.set(dev, []);
        groups.get(dev).push(i);
    });
    if (!groups.size) { $("rx-senders").textContent = "no sender matches the filter"; return; }

    $("rx-senders").innerHTML = [...groups.entries()].sort((a, b) => a[0].localeCompare(b[0])).map(([dev, items]) => {
        const connected = items.some(i => sendersCache[i].id === cur);
        const open = !!filter || sndOpen.has(dev);      //an active filter opens everything
        return `<div class="sndgrp">
            <div class="sndhead" data-dev="${esc(dev)}"><span class="arrow">${open ? "▾" : "▸"}</span>
                ${esc(dev)} <span class="sub">${items.length} sender${connected ? ' · <span class="pill ok">connected</span>' : ""}</span>
            </div>` +
            (open ? `<table class="kv">` + items.map(i => {
                const s = sendersCache[i];
                return `<tr${s.active === false ? ' class="snd-inactive"' : ""}>
                <td>${esc(s.label) || "(unnamed)"}${s.is_self ? ' <span class="pill off">this device</span>' : ""}
                    ${s.active === false ? ' <span class="pill off">inactive</span>' : ""}
                    ${s.media_type ? `<span class="sub"> · ${esc(s.media_type)}</span>` : ""}</td>
                <td style="text-align:right">${s.id === cur
                    ? '<span class="pill ok">connected</span> <button class="btn" data-off="' + i + '">Disconnect</button>'
                    : '<button class="btn" data-on="' + i + '"' + (s.manifest_href ? "" : " disabled") + '>Connect</button>'}
                </td></tr>`;
            }).join("") + `</table>` : "") + `</div>`;
    }).join("");

    document.querySelectorAll("#rx-senders .sndhead").forEach(el => {
        el.onclick = () => {
            const dev = el.dataset.dev;
            sndOpen.has(dev) ? sndOpen.delete(dev) : sndOpen.add(dev);
            renderSenders();
        };
    });
    document.querySelectorAll("#rx-senders [data-on]").forEach(btn => {
        btn.onclick = async () => {
            const s = sendersCache[+btn.dataset.on];
            btn.textContent = "Connecting…";
            const result = await api("/api/action/nmos-connect", "POST",
                {sender_id: s.id, manifest_href: s.manifest_href}).catch(() => ({error: "request failed"}));
            if (result.error) alert(result.error);
            setTimeout(loadSenders, 800);
        };
    });
    document.querySelectorAll("#rx-senders [data-off]").forEach(btn => {
        btn.onclick = async () => {
            await api("/api/action/receiver-disconnect", "POST", {}).catch(() => {});
            setTimeout(loadSenders, 800);
        };
    });
}
$("rx-browse").onclick = loadSenders;
$("rx-filter").oninput = renderSenders;
/* keep the registry view current while the page is open */
setInterval(() => {
    if ($("page-receiver").classList.contains("active")) loadSenders();
}, 30000);

/* ---------- receiver controls ---------- */
$("rx-connect").onclick = () => {
    const sdp = $("rx-sdp").value.trim();
    if (sdp) api("/api/action/receiver-connect", "POST", {sdp});
};
$("rx-disconnect").onclick = () => api("/api/action/receiver-disconnect", "POST", {});
let gainTouchedAt = 0;
$("rx-gain").oninput = () => {
    gainTouchedAt = Date.now();
    $("rx-gain-val").textContent = $("rx-gain").value + " dB";
};
$("rx-gain").onchange = () => setConfig({"receiver.gain_db": parseFloat($("rx-gain").value)});

/* ---------- sender controls ---------- */
let txSource = "tone";
document.querySelectorAll("#tx-source-seg .btn").forEach(btn => {
    btn.onclick = () => {
        txSource = btn.dataset.src;
        document.querySelectorAll("#tx-source-seg .btn").forEach(b => b.classList.toggle("active", b === btn));
        $("tx-tone-row").style.display = txSource === "file" ? "none" : "";
        $("tx-file-row").style.display = txSource === "file" ? "" : "none";
    };
});
$("tx-start").onclick = () => setConfig({
    "sender.enabled": true,
    "sender.label": $("tx-label").value.trim(),
    "sender.source": txSource,
    "sender.tone_hz": parseFloat($("tx-freq").value),
    "sender.tone_level_db": parseFloat($("tx-level").value),
    "sender.file": $("tx-file").value || "",
    "sender.loop": $("tx-loop").checked,
    "sender.multicast_primary": $("tx-mc1").value,
    "sender.multicast_secondary": $("tx-mc2").value,
    "sender.port": parseInt($("tx-port").value, 10)
});
$("tx-stop").onclick = () => setConfig({"sender.enabled": false});
let cfgTouchedAt = 0;
["tx-mc1", "tx-mc2", "tx-port"].forEach(id => { $(id).oninput = () => { cfgTouchedAt = Date.now(); }; });
$("tx-leg1").onchange = () => { cfgTouchedAt = Date.now(); setConfig({"sender.leg1_enabled": $("tx-leg1").checked}); };
$("tx-leg2").onchange = () => { cfgTouchedAt = Date.now(); setConfig({"sender.leg2_enabled": $("tx-leg2").checked}); };
$("tx-show-sdp").onclick = async () => {
    const result = await api("/api/action/sender-sdp", "POST", {});
    $("tx-sdp").textContent = result.sdp || result.error || "sender is not running";
    $("tx-sdp").style.display = "";
    $("tx-copy-sdp").style.display = result.sdp ? "" : "none";
};
$("tx-copy-sdp").onclick = () => navigator.clipboard.writeText($("tx-sdp").textContent);
$("tx-upload").onchange = async () => {
    const file = $("tx-upload").files[0];
    if (!file) return;
    await fetch("/api/files/" + encodeURIComponent(file.name), {method: "PUT", body: file});
    await loadFiles();
    $("tx-file").value = file.name;
};

async function loadFiles() {
    const files = await api("/api/files");
    $("tx-file").innerHTML = files.map(f => `<option>${f}</option>`).join("") || "<option value=''>no files</option>";
}

/* ---------- interface setup ---------- */
async function loadNetwork() {
    let ifs;
    try { ifs = (await api("/api/action/network-status", "POST", {})).interfaces || []; }
    catch (e) { $("net-ifs").textContent = "network state unavailable"; return; }

    $("net-ifs").innerHTML = ifs.map((f, n) => `
        <div class="netif">
            <div class="row">
                <span class="pill ${f.role}">${f.role.toUpperCase()}</span>
                <span class="ifname">${f.interface}</span>
                <span class="pill ${f.link ? "ok" : "bad"}">${f.link ? "Link" : "No link"}</span>
                <span class="cur">${f.address || "no address"}${f.gateway ? " · gw " + f.gateway : ""}
                    ${f.method ? " · " + (f.method === "auto" ? "DHCP" : "static") : ""}</span>
            </div>
            <div class="row">
                <label class="inline">Mode <select id="net-method-${n}">
                    <option value="dhcp" ${f.method !== "manual" ? "selected" : ""}>DHCP</option>
                    <option value="static" ${f.method === "manual" ? "selected" : ""}>Static</option>
                </select></label>
                <label class="inline">IP/Prefix <input id="net-addr-${n}" size="16"
                    value="${f.address || ""}" placeholder="192.168.10.5/24"></label>
                <label class="inline">Gateway <input id="net-gw-${n}" size="12"
                    value="${f.gateway || ""}" placeholder="optional"></label>
                <button class="btn" id="net-apply-${n}">Apply</button>
            </div>
        </div>`).join("") || `<div class="sub">no interfaces configured</div>`;

    ifs.forEach((f, n) => {
        const toggle = () => {
            const dhcp = $("net-method-" + n).value === "dhcp";
            $("net-addr-" + n).disabled = dhcp;
            $("net-gw-" + n).disabled = dhcp;
        };
        $("net-method-" + n).onchange = toggle;
        toggle();
        $("net-apply-" + n).onclick = async () => {
            const body = {interface: f.interface, method: $("net-method-" + n).value,
                          address: $("net-addr-" + n).value.trim(), gateway: $("net-gw-" + n).value.trim()};
            if (body.method === "static" && !/^\d+\.\d+\.\d+\.\d+\/\d+$/.test(body.address)) {
                alert("IP must be address/prefix, e.g. 192.168.10.5/24");
                return;
            }
            $("net-apply-" + n).textContent = "Applying…";
            try {
                const r = await api("/api/action/network-apply", "POST", body);
                if (r.error) alert(r.error);
            } catch (e) {}
            setTimeout(loadNetwork, 4000);
        };
    });
}

/* ---------- settings ---------- */
$("set-save").onclick = () => setConfig({
    "device.label": $("set-label").value,
    "network.interface_primary": $("set-if1").value,
    "network.interface_secondary": $("set-if2").value,
    "receiver.alsa_device": $("set-alsa").value,
    "receiver.playout_delay_ms": parseInt($("set-delay").value, 10),
    "nmos.registry_override": $("set-registry").value
});
$("ptp-domain").onchange = () => setConfig({"ptp.domain": parseInt($("ptp-domain").value, 10)});

async function loadConfig() {
    config = await api("/api/config");
    $("tx-label").value = config.sender.label || "";
    $("tx-leg1").checked = config.sender.leg1_enabled !== false;
    $("tx-leg2").checked = config.sender.leg2_enabled !== false;
    $("tx-mc1").value = config.sender.multicast_primary;
    $("tx-mc2").value = config.sender.multicast_secondary;
    $("tx-port").value = config.sender.port;
    $("tx-freq").value = config.sender.tone_hz;
    $("tx-level").value = config.sender.tone_level_db;
    $("tx-loop").checked = config.sender.loop;
    $("rx-gain").value = config.receiver.gain_db;
    $("rx-gain-val").textContent = config.receiver.gain_db + " dB";
    $("set-label").value = config.device.label;
    $("set-if1").value = config.network.interface_primary;
    $("set-if2").value = config.network.interface_secondary;
    $("set-alsa").value = config.receiver.alsa_device;
    $("set-delay").value = config.receiver.playout_delay_ms;
    $("set-registry").value = config.nmos.registry_override;
    $("ptp-domain").value = config.ptp.domain;
    txSource = config.sender.source;
    document.querySelectorAll("#tx-source-seg .btn").forEach(b => b.classList.toggle("active", b.dataset.src === txSource));
    $("tx-tone-row").style.display = txSource === "file" ? "none" : "";
    $("tx-file-row").style.display = txSource === "file" ? "" : "none";
    $("set-version").textContent = "pi-audio-node " + (status.version || "");
}

loadConfig();
loadFiles();
loadNetwork();
connectWs();
