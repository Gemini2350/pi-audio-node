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
}
document.querySelectorAll(".tab").forEach(tab => { tab.onclick = () => showPage(tab.dataset.page); });
if (location.hash.length > 1) showPage(location.hash.slice(1));

/* ---------- helpers ---------- */
const STATE_NAMES = ["Inaktiv", "Gesund", "Teilgestört", "Gestört"];
const STATE_CLASS = ["off", "ok", "warn", "bad"];
const LINK_NAMES = {1: "Alle up", 2: "Teilweise down", 3: "Alle down"};

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
    ws.onmessage = e => { status = JSON.parse(e.data); render(); };
}

/* ---------- render ---------- */
function render() {
    const ptp = status.ptp || {};
    const rx = status.receiver || {};
    const tx = status.sender || {};
    const mon = status.monitors || {};

    /* dashboard */
    pill($("d-ptp-state"), ptp.synced ? "Locked" : (ptp.masters && ptp.masters.length ? "Sync…" : "Kein Master"),
         ptp.synced ? "ok" : "warn");
    const gm = (ptp.masters || []).find(m => m.selected);
    $("d-ptp-gm").textContent = gm ? gm.grandmaster : "–";
    $("d-ptp-offset").textContent = ptp.synced ? fmtNs(ptp.offset_ns) : "–";

    pill($("d-rx-state"), rx.running ? (rx.receiving ? "Empfängt" : "Kein Signal") : "Aus",
         rx.running ? (rx.receiving ? "ok" : "bad") : "off");
    $("d-rx-meter-l").style.width = meterWidth(rx.running && rx.meters ? rx.meters.left_db : -120);
    $("d-rx-meter-r").style.width = meterWidth(rx.running && rx.meters ? rx.meters.right_db : -120);
    $("d-rx-info").textContent = rx.running
        ? `${rx.session_name || ""} · ${(rx.legs || []).length} Leg(s) · ${rx.played || 0} Pakete`
        : "über NMOS patchen oder SDP einfügen";

    pill($("d-tx-state"), tx.running ? (tx.waiting_for_ptp ? "Wartet auf PTP" : "Sendet") : "Aus",
         tx.running ? (tx.waiting_for_ptp ? "warn" : "ok") : "off");
    $("d-tx-meter-l").style.width = meterWidth(tx.running && tx.meters ? tx.meters.left_db : -120);
    $("d-tx-meter-r").style.width = meterWidth(tx.running && tx.meters ? tx.meters.right_db : -120);
    $("d-tx-info").textContent = tx.running ? `${tx.source} · ${tx.packets_sent} Pakete` : "–";

    const nmos = status.nmos || {};
    $("d-nmos-registry").textContent = nmos.registry || "keine Registry";
    $("d-nmos-status").textContent = (nmos.status || "–") + " · Node " + (nmos.node_id || "").slice(0, 8) + "…";

    /* receiver page */
    const rmon = mon.receiver || {};
    pill($("rx-overall"), STATE_NAMES[rmon.overall || 0], STATE_CLASS[rmon.overall || 0]);
    const domains = [["link", "Link"], ["path", "Verbindung"], ["sync", "PTP-Sync"], ["stream", "Stream"]];
    $("rx-domains").innerHTML = domains.map(([key, name]) => {
        const d = rmon[key] || {status: 0, message: "", transitions: 0};
        const label = key === "link" ? (LINK_NAMES[d.status] || "–") : STATE_NAMES[d.status];
        return `<div class="domain"><div class="name">${name}</div>
                <div class="state" style="color:var(--${STATE_CLASS[d.status] === "off" ? "off" : STATE_CLASS[d.status]})">${label}</div>
                <div class="msg">${d.message || (d.transitions ? d.transitions + " Übergänge" : "")}</div></div>`;
    }).join("");

    $("rx-legs").innerHTML = (rx.legs || []).map((leg, i) =>
        `<div class="leg"><span class="pill ${leg.active ? "ok" : "bad"}">${i ? "SEC" : "PRI"}</span>
         <span class="ifname">${leg.interface}</span><span class="addr">${leg.multicast}</span>
         <span>${leg.received} rx · ${leg.lost} lost</span></div>`).join("")
        || `<div class="sub">kein aktiver Empfang</div>`;

    $("rx-stats").innerHTML =
        `<tr><td>Gespielt</td><td>${rx.played || 0}</td></tr>
         <tr><td>Verdeckt (Verlust)</td><td>${rx.concealed || 0}</td></tr>
         <tr><td>Merge-Duplikate</td><td>${rx.duplicates_merged || 0}</td></tr>
         <tr><td>Von Leg 2 gerettet</td><td>${rx.from_secondary || 0}</td></tr>
         <tr><td>ALSA-Puffer</td><td>${status.alsa ? Math.round(status.alsa.delay_frames / 48) : 0} ms</td></tr>
         <tr><td>Underruns</td><td>${status.alsa ? status.alsa.underruns : 0}</td></tr>`;

    /* sender page */
    pill($("tx-state"), tx.running ? (tx.essence_ok ? "Sendet" : "Quelle leer") : "Aus",
         tx.running ? (tx.essence_ok ? "ok" : "warn") : "off");
    $("tx-stats").innerHTML =
        `<tr><td>Pakete</td><td>${tx.packets_sent || 0}</td></tr>
         <tr><td>Sendefehler</td><td>${tx.send_errors || 0}</td></tr>
         <tr><td>Quelle</td><td>${tx.source || "–"}</td></tr>` +
        (tx.legs || []).map((leg, i) => `<tr><td>Leg ${i + 1}</td><td>${leg.multicast}:${leg.port} @ ${leg.interface}</td></tr>`).join("");

    /* ptp page */
    pill($("ptp-state"), ptp.synced ? "Locked" : "Nicht synchron", ptp.synced ? "ok" : "warn");
    $("ptp-identity").textContent = ptp.identity || "–";
    $("ptp-offset").textContent = fmtNs(ptp.offset_ns);
    $("ptp-delay").textContent = fmtNs(ptp.mean_path_delay_ns);
    const tbody = $("ptp-masters").querySelector("tbody");
    tbody.innerHTML = (ptp.masters || []).map(m =>
        `<tr class="${m.selected ? "selected" : ""}">
         <td>${m.selected ? '<span class="crown">★ Master</span>' : ""}</td>
         <td>${m.port_identity}<br><span class="sub">${m.address}</span></td>
         <td>${m.grandmaster}</td>
         <td>${m.priority1}</td><td>${m.clock_class}</td><td>0x${m.clock_accuracy.toString(16)}</td>
         <td>0x${m.variance.toString(16)}</td><td>${m.priority2}</td><td>${m.steps_removed}</td>
         <td>${m.announces}</td><td class="sub">${m.bmca}</td></tr>`).join("")
        || `<tr><td colspan="11" class="sub">keine Announce-Messages auf Domain ${ptp.domain}</td></tr>`;
    drawChart(ptp.offset_history || []);
}

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

/* ---------- receiver controls ---------- */
$("rx-connect").onclick = () => {
    const sdp = $("rx-sdp").value.trim();
    if (sdp) api("/api/action/receiver-connect", "POST", {sdp});
};
$("rx-disconnect").onclick = () => api("/api/action/receiver-disconnect", "POST", {});
$("rx-gain").oninput = () => { $("rx-gain-val").textContent = $("rx-gain").value + " dB"; };
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
$("tx-upload").onchange = async () => {
    const file = $("tx-upload").files[0];
    if (!file) return;
    await fetch("/api/files/" + encodeURIComponent(file.name), {method: "PUT", body: file});
    await loadFiles();
    $("tx-file").value = file.name;
};

async function loadFiles() {
    const files = await api("/api/files");
    $("tx-file").innerHTML = files.map(f => `<option>${f}</option>`).join("") || "<option value=''>keine Dateien</option>";
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
connectWs();
