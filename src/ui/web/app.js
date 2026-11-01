// Iris — l'interfaccia. Parla con il programma con window.chrome.webview:
//   dal programma: {"t": "settings" | "status" | "conversation" | ..., "v": ...}
//   al programma:  {"a": "set", "k": "wakeWordEnabled", "v": true}, {"a": "sendText", "text": "..."}...
"use strict";

const webview = window.chrome && window.chrome.webview;
const post = (msg) => { if (webview) webview.postMessage(JSON.stringify(msg)); else console.log("→", msg); };
const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

function h(tag, attrs, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k === "class") el.className = v;
    else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
    else el.setAttribute(k, v === true ? "" : v);
  }
  for (const c of children.flat()) {
    if (c === null || c === undefined || c === false) continue;
    el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return el;
}

const S = {
  settings: null,
  lists: { languages: [], mics: [], outputs: [], voices: [], effects: [] },
  status: { state: "starting" },
  game: null,
  update: null,
  capture: null,
  captureText: { target: "", text: "" },
  page: "home",
  meters: { mic: 0, voice: 0, speaking: false, prob: 0, noise: 0, state: "starting" },
};

const dec = (v, d) => Number(v).toFixed(d).replace(".", ",");
const pctOf = (alpha) => Math.round((alpha * 100) / 255);
const alphaOf = (pct) => Math.round((pct * 255) / 100);
const level01 = (v) => Math.min(1, Math.max(0, (20 * Math.log10(Math.max(v, 1e-5)) + 60) / 60)); // come le barre di prima, in dB

const STATE_LABEL = { starting: "AVVIO", idle: "IN ATTESA", listening: "TI ASCOLTO", thinking: "CI PENSO", speaking: "PARLO", error: "ATTENZIONE" };

// =====================================================================================================
// Cursori: come si passa dal valore delle impostazioni a quello del cursore, e come si scrive
// =====================================================================================================
const SILENCE_MIN = 0.0005, SILENCE_MAX = 0.2;
const SLIDERS = {
  vadThreshold: {
    label: "Sensibilità", min: 5, max: 90, step: 5,
    toUi: (v) => Math.round((1 - v) * 100), fromUi: (x) => Math.round((1 - x / 100) * 100) / 100,
    fmt: (x) => { const v = 1 - x / 100; return v <= 0.3 ? "molto alta" : v <= 0.45 ? "alta" : v <= 0.6 ? "media" : v <= 0.75 ? "bassa" : "molto bassa"; },
    hint: (x) => "Soglia di voce " + dec(1 - x / 100, 2) + ": più è alta la sensibilità, più rumori la svegliano.",
  },
  endSilenceMs: { label: "Pausa che chiude la frase", min: 300, max: 2500, step: 100, fmt: (x) => dec(x / 1000, 1) + " s" },
  followUpSeconds: {
    label: "Ascolto dopo una risposta", min: 0, max: 30, step: 1, fmt: (x) => (x === 0 ? "serve «Iris»" : x + " s"),
    hint: () => "Per tanti secondi dopo una risposta puoi continuare senza dire «Iris».",
  },
  micGain: { label: "Volume del microfono", min: 20, max: 800, step: 10, toUi: (v) => Math.round(v * 100), fromUi: (x) => x / 100, fmt: (x) => x + "%" },
  speechRate: { label: "Velocità della voce", min: 50, max: 200, step: 10, toUi: (v) => Math.round(v * 100), fromUi: (x) => x / 100, fmt: (x) => x + "%" },
  overlaySize: { label: "Dimensione", min: 80, max: 480, step: 20, fmt: (x) => x + " px" },
  overlayOpacity: { label: "Opacità", min: 24, max: 100, step: 1, toUi: pctOf, fromUi: alphaOf, fmt: (x) => x + "%" },
  chatOpenDelayMs: { label: "Attesa dopo l'apertura della chat", min: 0, max: 2000, step: 10, fmt: (x) => x + " ms" },
  typingDelayMs: {
    label: "Pausa tra un carattere e l'altro", min: 0, max: 200, step: 2, fmt: (x) => x + " ms",
    hint: () => "Se la chat perde delle lettere, aumentala.",
  },
  silenceThreshold: {
    label: "Soglia", min: 0, max: 1000, step: 1,
    toUi: (v) => Math.round((1000 * Math.log(v / SILENCE_MIN)) / Math.log(SILENCE_MAX / SILENCE_MIN)),
    fromUi: (x) => Math.round(SILENCE_MIN * Math.pow(SILENCE_MAX / SILENCE_MIN, x / 1000) * 100000) / 100000,
    fmt: (x) => dec(SILENCE_MIN * Math.pow(SILENCE_MAX / SILENCE_MIN, x / 1000), 4),
  },
  ocrMaxMessages: {
    label: "Messaggi tenuti nel riquadro", min: 1, max: 20, step: 1, fmt: (x) => String(x),
    hint: () => "Ogni traduzione nuova entra in fondo e spinge su le altre fino a questo numero.",
  },
  ocrOverlayOpacity: { label: "Opacità", min: 24, max: 100, step: 1, toUi: pctOf, fromUi: alphaOf, fmt: (x) => x + "%" },
  miningOverlayOpacity: {
    label: "Opacità dello sfondo", min: 0, max: 100, step: 5, toUi: pctOf, fromUi: alphaOf, fmt: (x) => (x === 0 ? "0% · solo scritte" : x + "%"),
    hint: () => "A 0% restano solo le scritte, con un alone scuro per leggerle sul gioco.",
  },
  miningOverlayTextOpacity: { label: "Opacità delle scritte", min: 20, max: 100, step: 5, toUi: pctOf, fromUi: alphaOf, fmt: (x) => x + "%" },
  miningOverlaySeconds: { label: "Resta a schermo", min: 3, max: 120, step: 1, fmt: (x) => x + " s" },
};

const SEGMENTS = {
  miningOverlayBasic: [[false, "Complete"], [true, "Base · materiale e quantità"]],
};

function buildSliders() {
  for (const host of $$("[data-slider]")) {
    const key = host.dataset.slider, def = SLIDERS[key];
    if (!def) continue;
    const value = h("span", { class: "slider-value" });
    const input = h("input", { type: "range", min: def.min, max: def.max, step: def.step, "aria-label": def.label });
    const hint = h("small");
    host.className = "slider";
    host.append(h("div", { class: "slider-head" }, h("b", {}, def.label), value), input, hint);
    const show = () => {
      const x = Number(input.value);
      value.textContent = def.fmt(x);
      input.style.setProperty("--p", ((x - def.min) * 100) / (def.max - def.min) + "%");
      hint.textContent = def.hint ? def.hint(x) : "";
      hint.style.display = def.hint ? "" : "none";
    };
    input.addEventListener("input", show);
    input.addEventListener("pointerdown", () => (input.dragging = true));
    input.addEventListener("pointerup", () => (input.dragging = false));
    input.addEventListener("change", () => {
      input.dragging = false;
      const x = Number(input.value);
      post({ a: "set", k: key, v: def.fromUi ? def.fromUi(x) : x });
    });
    host.update = (v) => {
      if (input.dragging || document.activeElement === input) return show();
      input.value = def.toUi ? def.toUi(v) : v;
      show();
    };
  }
}

function buildSegments() {
  for (const host of $$("[data-seg]")) {
    const key = host.dataset.seg, options = SEGMENTS[key];
    if (!options) continue;
    host.replaceChildren(...options.map(([value, label]) => h("button", { type: "button", onclick: () => post({ a: "set", k: key, v: value }) }, label)));
    host.update = (v) => options.forEach(([value], i) => host.children[i].classList.toggle("on", value === v));
  }
}

// =====================================================================================================
// Pagine
// =====================================================================================================
function showPage(name) {
  if (!$(`.page[data-page="${name}"]`)) name = "home";
  S.page = name;
  $$(".page").forEach((p) => p.classList.toggle("active", p.dataset.page === name));
  $$(".nav-item").forEach((b) => b.classList.toggle("active", b.dataset.page === name));
  $("#main").scrollTop = 0;
  post({ a: "page", name });
  if (name === "voice") post({ a: "refreshDevices" });
  if (name === "home") { startOrb(); scrollConvo(true); }
  try { localStorage.setItem("iris.page", name); } catch (e) { /* niente: si riparte da Iris */ }
}

// =====================================================================================================
// Impostazioni -> controlli
// =====================================================================================================
function fillSelect(select, items, selected) {
  const key = JSON.stringify(items);
  if (select.dataset.items !== key) {
    select.replaceChildren(...items.map(([value, label]) => h("option", { value }, label)));
    select.dataset.items = key;
  }
  if (document.activeElement !== select) select.value = String(selected);
}

function renderSettings() {
  const s = S.settings;
  if (!s) return;
  for (const input of $$("input[type=checkbox][data-set]")) input.checked = !!s[input.dataset.set];
  for (const host of $$("[data-slider]")) if (host.update && s[host.dataset.slider] !== undefined) host.update(s[host.dataset.slider]);
  for (const host of $$("[data-seg]")) if (host.update) host.update(s[host.dataset.seg]);
  for (const select of $$("select[data-set]")) {
    if (select.dataset.list === "languages") fillSelect(select, S.lists.languages.map((l, i) => [i, l]), s[select.dataset.set]);
    else if (document.activeElement !== select) select.value = String(s[select.dataset.set]);
  }

  // Tasti
  renderKeys("talk", s.triggers);
  renderKeys("chat", s.chatTriggers);
  renderMiningKeys(s.miningKeys);
  renderExtraSettings(s);
  renderCaptureLabel("chatOpen", "#key-chatOpen", s.chatOpenKey);
  renderCaptureLabel("chatSend", "#key-chatSend", s.chatSendKey);
  $('[data-capture="chatOpen"]').disabled = !s.chatOpenEnabled && S.capture !== "chatOpen";

  // Voce
  fillSelect($("#mic-select"), [["", "Predefinito di Windows"], ...S.lists.mics.map((m) => [m.name, m.name + (m.missing ? " (non collegato)" : "")])], s.micName);
  fillSelect($("#output-select"), [["", "Predefinita di Windows"], ...S.lists.outputs.map((m) => [m.name, m.name + (m.missing ? " (non collegato)" : "")])], s.outputDeviceName);
  fillSelect($("#effect-select"), S.lists.effects.map((e) => [e.id, e.label]), s.voiceEffect);
  const voices = S.lists.voices.length ? S.lists.voices : ["(nessuna voce di Windows trovata)"];
  const wanted = (s.voiceName || "").toLowerCase();
  const chosen = voices.find((v) => v.toLowerCase().includes(wanted)) || voices[0];
  fillSelect($("#voice-select"), voices.map((v) => [v, v]), chosen);
  $$("#engine-seg button").forEach((b) => b.classList.toggle("on", b.dataset.engine === (s.voiceEngine === "windows" ? "windows" : "neurale")));

  // Traduzione chat
  const r = s.ocrRegion;
  $("#ocr-region").textContent = r.w > 0 && r.h > 0
    ? `${r.w} × ${r.h} pixel in (${r.x}, ${r.y})` + (Math.abs(r.angle) >= 0.05 ? ` · girata di ${r.angle > 0 ? "+" : ""}${dec(r.angle, 1)}°` : "")
    : "Nessuna zona scelta.";
  const filterInput = $("#filter-form input");
  if (document.activeElement !== filterInput) filterInput.value = s.chatFilterTag;
  $("#filter-label").textContent = s.chatFilterTag
    ? `Messaggi di [${s.chatFilterTag}], già tradotti, dal più vecchio al più nuovo.`
    : "Nessun filtro: scegline uno nella pagina Traduzione chat.";
  setChip("#screen-chip", !s.ocrEnabled ? ["", "Spenta"] : r.w > 0 ? ["ok", "Attiva"] : ["warn", "Manca la zona"]);
  setChip("#chat-chip", s.chatEnabled ? ["ok", "Dettatura attiva"] : ["", "Spenta"]);

  // Mining
  const m = s.miningRegion;
  $("#mining-region").textContent = m.w > 0 && m.h > 0 ? `${m.w} × ${m.h} pixel in (${m.x}, ${m.y})` : "Nessuna zona scelta.";
  setChip("#mining-chip", !s.miningEnabled ? ["", "Spento"] : m.w > 0 ? ["ok", "Pronto"] : ["warn", "Manca la zona"]);

  // Tu e memoria
  const nameInput = $("#name-form input");
  if (document.activeElement !== nameInput) nameInput.value = s.userName;
  $("#memory-count").textContent = s.memoryCount === 0 ? "Ancora niente in memoria." : s.memoryCount === 1 ? "1 cosa in memoria." : `${s.memoryCount} cose in memoria.`;
}

function setChip(sel, [kind, text]) {
  const chip = $(sel);
  if (!chip) return;
  chip.className = "chip" + (kind ? " " + kind : "");
  chip.textContent = text;
}

// --- Tasti (tastiera o joystick) -----------------------------------------------------------------------------
function keyChip(k, onRemove) {
  return h("span", { class: "key" + (k.shared ? " shared" : "") + (k.missing ? " missing" : ""), title: k.missing ? "Non collegato" : "" },
    k.label, k.shared ? h("em", {}, "anche Iris") : null,
    onRemove ? h("button", { type: "button", title: "Togli", onclick: onRemove }, "✕") : null);
}

function renderKeys(target, keys) {
  const host = $(`[data-keys="${target}"]`);
  if (!host) return;
  const capturing = S.capture === target;
  const children = keys.length
    ? keys.map((k, i) => keyChip(k, () => post({ a: "removeKey", target, index: i })))
    : [h("span", { class: "key-empty" }, target === "chat" ? "Nessun tasto: solo «Iris, scrivi in chat…»" : "Nessun tasto: si usa solo «Iris».")];
  const add = h("button", { class: "btn small" + (capturing ? " busy" : ""), type: "button", disabled: keys.length >= 6 && !capturing, onclick: () => post({ a: "captureKey", target }) },
    capturing ? "Annulla" : "+ Aggiungi un tasto");
  const hint = S.captureText.target === target && S.captureText.text ? h("div", { class: "capture-hint" }, S.captureText.text) : null;
  host.replaceChildren(...[...children, add, hint].filter(Boolean)); // (null diventerebbe il testo "null")
}

const MINING_KEYS = ["Leggi la signature", "Inserimento manuale (tastierino)", "Contorno della zona sì/no", "Mining acceso/spento"];
function renderMiningKeys(rows) {
  const host = $("#mining-keys");
  const cells = [];
  MINING_KEYS.forEach((name, i) => {
    const target = "mining:" + i, keys = rows[i] || [], capturing = S.capture === target;
    cells.push(h("div", { class: "label" }, name));
    cells.push(h("div", { class: "cell" },
      capturing ? h("span", { class: "key-empty" }, "premi il tasto…") : keys.length ? keyChip(keys[0]) : h("span", { class: "key-empty" }, "— nessuno —"),
      h("button", { class: "btn small" + (capturing ? " busy" : ""), type: "button", onclick: () => post({ a: "captureKey", target }) }, capturing ? "Annulla" : "Cambia"),
      keys.length ? h("button", { class: "btn small danger", type: "button", onclick: () => post({ a: "removeKey", target }) }, "Togli") : null));
  });
  host.replaceChildren(...cells);
  if (/^mining:[0-3]$/.test(S.captureText.target) && S.captureText.text) host.append(h("div", { class: "capture-hint", style: "grid-column: 1 / -1" }, S.captureText.text));
}

function renderCaptureLabel(target, sel, name) {
  const capturing = S.capture === target;
  $(sel).textContent = capturing ? "Premi il tasto da usare (Esc annulla)…" : name;
  const button = $(`[data-capture="${target}"]`);
  button.textContent = capturing ? "Annulla" : "Cambia";
  button.classList.toggle("busy", capturing);
}

// =====================================================================================================
// Stato, conversazione, sfera
// =====================================================================================================
function renderStatus() {
  const st = S.status;
  const label = STATE_LABEL[st.state] || "—";
  for (const el of [$("#status"), $(".orb-panel")]) el.dataset.state = st.state;
  $("#status-label").textContent = label;
  $("#status-text").textContent = st.text || "";
  $("#orb-state").textContent = label;
  $("#orb-detail").textContent = st.text || "";
  $("#orb-heard").textContent = st.heard ? `Ultima cosa sentita: «${st.heard}»` : "";
  $("#retry").classList.toggle("hidden", !st.canRetry);
  $("#gpu-info").textContent = st.gpu || "Caricamento dei modelli…";
}

let convoStick = true;
function scrollConvo(force) {
  const el = $("#convo");
  if (force || convoStick) el.scrollTop = el.scrollHeight;
}

function renderConversation(list) {
  const el = $("#convo");
  convoStick = el.scrollTop >= el.scrollHeight - el.clientHeight - 24;
  if (!list.length) {
    el.replaceChildren(h("div", { class: "convo-empty" },
      h("b", {}, "Canale aperto"),
      "Chiamala dicendo «Iris» e poi cosa ti serve: «Iris, carrello», «Iris, rotta per Crusader», «Iris, che minerale è?», «Iris, scrivi in chat: arrivo».",
      h("br"), "Oppure scrivile qui sotto."));
    return;
  }
  el.replaceChildren(...list.map((e) => {
    const small = e.kind === "tool" || e.kind === "info";
    return h("div", { class: `bubble ${e.kind}${e.pending ? " pending" : ""}`, title: small ? "" : "Doppio clic per copiare", ondblclick: () => post({ a: "copy", text: e.text }) },
      e.text, small && e.time ? h("time", {}, "· " + e.time) : null);
  }));
  scrollConvo(false);
}

// La sfera grande: anelli che girano, nucleo che respira con la voce.
const ORB_COLOR = { starting: [246, 183, 64], idle: [63, 216, 245], listening: [70, 227, 160], thinking: [246, 183, 64], speaking: [170, 242, 255], error: [255, 93, 108] };
const ORB_SPEED = { starting: 0.5, idle: 0.18, listening: 0.6, thinking: 1.8, speaking: 0.8, error: 0.1 };
const orb = { color: [63, 216, 245], level: 0, angle: 0, last: 0, running: false };

function startOrb() {
  if (orb.running || S.page !== "home" || document.hidden) return;
  orb.running = true;
  orb.last = 0;
  requestAnimationFrame(drawOrb);
}

function drawOrb(ts) {
  const canvas = $("#orb");
  if (S.page !== "home" || document.hidden) { orb.running = false; return; }
  const dt = orb.last ? Math.min(0.05, (ts - orb.last) / 1000) : 0.016;
  orb.last = ts;
  const ratio = window.devicePixelRatio || 1;
  const size = Math.round(canvas.clientWidth * ratio);
  if (size > 0 && canvas.width !== size) { canvas.width = size; canvas.height = size; }
  const ctx = canvas.getContext("2d");
  const W = canvas.width, c = W / 2;
  const state = S.meters.state || S.status.state || "idle";
  const target = ORB_COLOR[state] || ORB_COLOR.idle;
  orb.color = orb.color.map((v, i) => v + (target[i] - v) * Math.min(1, dt * 4));
  const live = state === "speaking" ? level01(S.meters.voice) : state === "listening" || S.meters.speaking ? level01(S.meters.mic) : 0;
  orb.level += (live - orb.level) * Math.min(1, dt * 10);
  orb.angle += dt * (ORB_SPEED[state] || 0.2);
  const [r, g, b] = orb.color.map(Math.round);
  const col = (a) => `rgba(${r},${g},${b},${a})`;
  const flicker = state === "error" ? 0.75 + 0.25 * Math.sin(ts / 90) : 1;

  ctx.clearRect(0, 0, W, W);
  // Alone
  const glowR = W * (0.26 + 0.08 * orb.level);
  let grad = ctx.createRadialGradient(c, c, W * 0.02, c, c, glowR * 1.7);
  grad.addColorStop(0, col(0.55 * flicker));
  grad.addColorStop(0.45, col(0.16 * flicker));
  grad.addColorStop(1, col(0));
  ctx.fillStyle = grad;
  ctx.fillRect(0, 0, W, W);
  // Nucleo
  const coreR = W * (0.12 + 0.05 * orb.level + 0.008 * Math.sin(ts / 600));
  grad = ctx.createRadialGradient(c - coreR * 0.25, c - coreR * 0.3, coreR * 0.05, c, c, coreR);
  grad.addColorStop(0, "rgba(255,255,255,0.95)");
  grad.addColorStop(0.35, col(0.95));
  grad.addColorStop(1, col(0.05));
  ctx.fillStyle = grad;
  ctx.beginPath(); ctx.arc(c, c, coreR, 0, Math.PI * 2); ctx.fill();

  ctx.lineCap = "butt";
  // Tacche esterne
  ctx.strokeStyle = col(0.28);
  ctx.lineWidth = Math.max(1, W * 0.004);
  for (let i = 0; i < 72; i++) {
    const a = (i / 72) * Math.PI * 2 - orb.angle * 0.15, long = i % 6 === 0;
    const r1 = W * 0.465, r2 = W * (long ? 0.44 : 0.452);
    ctx.beginPath(); ctx.moveTo(c + Math.cos(a) * r1, c + Math.sin(a) * r1); ctx.lineTo(c + Math.cos(a) * r2, c + Math.sin(a) * r2); ctx.stroke();
  }
  // Anello tratteggiato
  ctx.strokeStyle = col(0.35);
  ctx.lineWidth = Math.max(1, W * 0.005);
  for (let i = 0; i < 16; i++) {
    const a = (i / 16) * Math.PI * 2 + orb.angle * 0.5;
    ctx.beginPath(); ctx.arc(c, c, W * 0.36, a, a + (Math.PI * 2) / 16 * 0.55); ctx.stroke();
  }
  // Tre archi che girano al contrario
  ctx.strokeStyle = col(0.85 * flicker);
  ctx.lineWidth = Math.max(2, W * 0.011);
  ctx.shadowColor = col(0.9);
  ctx.shadowBlur = W * 0.03;
  for (let i = 0; i < 3; i++) {
    const a = (i / 3) * Math.PI * 2 - orb.angle;
    ctx.beginPath(); ctx.arc(c, c, W * 0.41, a, a + Math.PI * 0.32); ctx.stroke();
  }
  // Il livello della voce (tua o sua) come arco luminoso
  if (orb.level > 0.02) {
    ctx.lineWidth = Math.max(2, W * 0.016);
    ctx.strokeStyle = col(0.95);
    ctx.beginPath(); ctx.arc(c, c, W * 0.3, -Math.PI / 2 - orb.level * Math.PI, -Math.PI / 2 + orb.level * Math.PI); ctx.stroke();
  }
  // Pensa: un arco veloce
  if (state === "thinking" || state === "starting") {
    ctx.lineWidth = Math.max(1.5, W * 0.007);
    const a = orb.angle * 2.4;
    ctx.beginPath(); ctx.arc(c, c, W * 0.3, a, a + Math.PI * 0.5); ctx.stroke();
  }
  ctx.shadowBlur = 0;
  requestAnimationFrame(drawOrb);
}

// =====================================================================================================
// Barre del microfono
// =====================================================================================================
function renderMeters() {
  const m = S.meters;
  if (S.page === "voice") {
    const meter = $("#mic-meter");
    meter.querySelector(".fill").style.width = level01(m.mic) * 100 + "%";
    meter.classList.toggle("live", !!m.speaking);
    const diag = $("#mic-diag");
    diag.textContent = `Voce ${Math.round(m.prob * 100)}%  ·  volume ${dec(m.mic, 4)}  ·  rumore di fondo ${dec(m.noise, 4)}` + (m.speaking ? "  ·  TI SENTO" : "");
    diag.classList.toggle("live", !!m.speaking);
  }
  if (S.page === "chat" && S.settings) {
    const meter = $("#threshold-meter");
    const threshold = S.settings.silenceThreshold;
    meter.querySelector(".fill").style.width = level01(m.mic) * 100 + "%";
    meter.querySelector(".mark").style.left = level01(threshold) * 100 + "%";
    meter.classList.toggle("live", m.mic >= threshold);
  }
  if (S.page === "home") startOrb();
}

// =====================================================================================================
// Chat, mining, gioco, aggiornamenti
// =====================================================================================================
function renderChatHistory(list) {
  const el = $("#chat-history");
  if (!list.length) { el.replaceChildren(h("div", { class: "key-empty" }, "Ancora nessuna frase. Tieni premuto il tasto della chat e parla.")); return; }
  el.replaceChildren(...list.map((e) => h("div", { class: "item", title: "Doppio clic per copiare la traduzione", ondblclick: () => post({ a: "copy", text: e.written || e.heard }) },
    h("time", {}, e.time),
    h("div", {},
      h("div", { class: "heard" }, `«${e.heard}»`),
      h("div", { class: "written" + (e.written ? "" : " none") }, e.written ? "→ " + e.written : "(niente scritto)"),
      e.note ? h("div", { class: "item-note" }, e.note) : null))));
}

function renderLog(el, lines, empty) {
  el.dataset.empty = empty;
  el.replaceChildren(...lines.map((l) => (typeof l === "string" ? h("div", { class: "line" }, l) : l)));
}

let tableDirty = false, tableSaving = false;
function renderMiningData(d) {
  const area = $("#materials");
  if (!tableDirty) { area.value = d.table; $("#table-state").textContent = ""; }
  tableSaving = false;
  renderLog($("#unknowns"), d.unknowns.map((u) => h("div", { class: "line warn", title: "Clic: usalo per Identifica", style: "cursor:pointer", onclick: () => { $("#identify [name=value]").value = u.value; $("#identify [name=name]").focus(); } },
    `${u.label.padStart(10)}   letta ${u.count} ${u.count === 1 ? "volta " : "volte"}   ${u.time}`)), "Nessuna signature sconosciuta.");
  const valueField = $("#identify [name=value]");
  if (!valueField.value && d.unknowns.length) valueField.value = d.unknowns[0].value;
}

function renderGame(g) {
  S.game = g;
  const box = $("#game-status");
  box.textContent = g.text || "—";
  box.className = "callout " + (g.elevationProblem ? "bad" : g.running ? "ok" : "");
  setChip("#game-chip", g.elevationProblem ? ["bad", "Permessi"] : g.running ? ["ok", "Online"] : ["", "Offline"]);
  $("#led-game").classList.toggle("on", !!g.running);
  $("#dry-run").checked = !!g.dryRun;
  $("#profile-error").textContent = g.profileError ? `Profilo non valido: ${g.profileError} (resta in uso quello di prima)` : "";
  const lines = [...g.actions];
  if (g.unrecognized.length) {
    lines.push(h("div", { class: "sep" }, "Frasi non capite dal profilo (passate al modello)"));
    lines.push(...g.unrecognized.map((u) => h("div", { class: "line warn" }, u)));
  }
  renderLog($("#game-log"), lines, "Ancora nessun comando di gioco.");
  renderHomeChips();
}

function renderHomeChips() {
  const chips = [];
  if (S.game) chips.push(h("span", { class: "chip" + (S.game.running ? " ok" : "") }, S.game.running ? "Star Citizen online" : "Star Citizen offline"));
  if (S.settings) chips.push(h("span", { class: "chip" + (S.settings.wakeWordEnabled ? " ok" : "") }, S.settings.wakeWordEnabled ? "Ascolto «Iris»" : "Solo tasto"));
  if (S.update && S.update.phase === "ready") chips.push(h("span", { class: "chip warn" }, "Aggiornamento pronto"));
  $("#home-chips").replaceChildren(...chips);
}

function renderUpdate(u) {
  S.update = u;
  const newer = u.latest && u.latest !== u.current;
  $("#update-current").textContent = "v" + u.current;
  const state = $("#update-state");
  const texts = {
    idle: ["", "Il primo controllo parte pochi secondi dopo l'avvio."],
    checking: ["", "Controllo su GitHub…"],
    uptodate: ["ok", `Hai l'ultima versione${u.checkedAt ? " (controllato alle " + u.checkedAt + ")" : ""}.`],
    downloading: ["warn", `Scarico la versione ${u.latest}… ${u.progress}%`],
    ready: ["warn", `Versione ${u.latest} pronta: si installa da sola al prossimo avvio, o subito con «Riavvia e aggiorna».`],
    error: ["bad", `Controllo non riuscito: ${u.error}`],
    disabled: ["", "Aggiornamenti automatici spenti: usa «Controlla ora»."],
  };
  const [kind, text] = texts[u.phase] || ["", ""];
  state.className = "update-state " + kind;
  state.textContent = text + (u.phase === "ready" && u.error ? ` (ultimo tentativo: ${u.error})` : "");
  const downloading = u.phase === "downloading";
  for (const bar of [$("#update-progress"), $("#update-card-progress")]) {
    bar.classList.toggle("hidden", !downloading);
    bar.firstElementChild.style.width = u.progress + "%";
  }
  $("#update-install").classList.toggle("hidden", u.phase !== "ready");
  const notes = $("#update-notes");
  notes.textContent = u.notes || "";
  notes.classList.toggle("hidden", !(newer && u.notes));
  // Nella barra laterale: solo quando c'è qualcosa di nuovo.
  const card = $("#update-card");
  card.classList.toggle("hidden", !(u.phase === "ready" || downloading));
  $("#update-card-version").textContent = "v" + u.latest;
  $("#update-card-text").textContent = downloading ? `Download in corso… ${u.progress}%` : "Pronto da installare.";
  $("#update-card-install").classList.toggle("hidden", u.phase !== "ready");
  $("#led-update").classList.toggle("on", u.phase === "ready");
  renderHomeChips();
}

// =====================================================================================================
// Avvisi e conferme
// =====================================================================================================
function toast(text, ok = true) {
  if (!text) return;
  const el = h("div", { class: "toast" + (ok ? "" : " bad") }, text);
  $("#toasts").append(el);
  setTimeout(() => { el.classList.add("out"); setTimeout(() => el.remove(), 320); }, ok ? 3200 : 6000);
}

function ask(title, text, buttons) {
  return new Promise((resolve) => {
    const modal = $("#modal");
    $("#modal-title").textContent = title;
    $("#modal-text").textContent = text;
    const close = (value) => { modal.classList.add("hidden"); document.removeEventListener("keydown", onKey); resolve(value); };
    const onKey = (e) => { if (e.key === "Escape") close(null); };
    $("#modal-buttons").replaceChildren(...buttons.map(([label, value, kind]) => h("button", { class: "btn " + (kind || ""), type: "button", onclick: () => close(value) }, label)));
    modal.classList.remove("hidden");
    document.addEventListener("keydown", onKey);
    modal.querySelector(".btn.primary, .btn.danger, .btn")?.focus();
  });
}

// =====================================================================================================
// Messaggi dal programma
// =====================================================================================================
const handlers = {
  lists(v) {
    S.lists = v;
    $("#version").textContent = "v" + v.version;
    renderSettings();
  },
  settings(v) { S.settings = v; renderSettings(); renderHomeChips(); },
  status(v) { S.status = v; renderStatus(); },
  conversation: renderConversation,
  meters(v) { S.meters = v; renderMeters(); },
  chatHistory: renderChatHistory,
  chatNotice(v) {
    const el = $("#chat-notice");
    el.textContent = v.text || "Nessun messaggio ancora.";
    el.className = "callout " + (v.kind === "dim" ? "" : v.kind);
  },
  filtered(lines) {
    renderLog($("#filtered"), lines, "Nessun messaggio filtrato ancora.");
    const el = $("#filtered"); el.scrollTop = el.scrollHeight;
    $("#count-filtered").textContent = lines.length ? String(lines.length) : "";
  },
  mining(v) {
    const box = $("#mining-last");
    box.className = "reading " + v.kind;
    box.querySelector(".reading-value").textContent = v.last;
    renderLog($("#mining-history"), v.history.map((r) => h("div", { class: "line " + r.kind }, r.text)), "Le ultime letture compaiono qui.");
    const ocr = $("#mining-ocr");
    ocr.textContent = v.ocr || "";
    ocr.className = "note " + (v.ocrKind || "");
  },
  miningData: renderMiningData,
  game: renderGame,
  gameTest(v) { const el = $("#game-test-result"); el.textContent = v.text; el.className = "note " + (v.ok ? "ok" : "warn"); },
  calibration(text) { $("#calibration").textContent = text; },
  update: renderUpdate,
  ocrInfo(v) { const el = $("#ocr-info"); el.textContent = v.text; el.className = "note " + v.kind; },
  capture(v) {
    S.capture = v.active ? v.target : null;
    S.captureText = { target: v.target, text: v.active ? v.text : v.text };
    renderSettings();
    if (!v.active && v.text) setTimeout(() => { if (S.captureText.target === v.target && !S.capture) { S.captureText = { target: "", text: "" }; renderSettings(); } }, 4000);
  },
  toast(v) {
    toast(v.text, v.ok);
    if (!v.ok && tableSaving) tableSaving = false;
  },
};

Object.assign(handlers, axisHandlers); // la pagina dell'Axis Visualizer (axis.js)
Object.assign(handlers, logHandlers);  // diario di bordo e raffineria (log.js)
Object.assign(handlers, commandHandlers); // editor dei comandi di gioco (commands.js)

if (webview) {
  webview.addEventListener("message", (e) => {
    const msg = e.data;
    const handler = msg && handlers[msg.t];
    if (handler) handler(msg.v);
  });
}

// =====================================================================================================
// Controlli -> programma
// =====================================================================================================
function wire() {
  $$(".nav-item").forEach((b) => b.addEventListener("click", () => showPage(b.dataset.page)));

  // Interruttori e menu legati a un'impostazione
  for (const input of $$("input[type=checkbox][data-set]")) input.addEventListener("change", () => post({ a: "set", k: input.dataset.set, v: input.checked }));
  for (const select of $$("select[data-set]")) select.addEventListener("change", () => post({ a: "set", k: select.dataset.set, v: select.hasAttribute("data-num") ? Number(select.value) : select.value }));

  // Pulsanti semplici: data-action (+ data-which)
  for (const b of $$("[data-action]")) b.addEventListener("click", () => post({ a: b.dataset.action, which: b.dataset.which }));
  for (const b of $$("[data-capture]")) b.addEventListener("click", () => post({ a: "captureKey", target: b.dataset.capture }));

  // Iris
  $("#composer").addEventListener("submit", (e) => {
    e.preventDefault();
    const input = $("#composer-input"), text = input.value.trim();
    if (!text) return;
    post({ a: "sendText", text });
    input.value = "";
    convoStick = true;
  });
  $("#stop").addEventListener("click", () => post({ a: "stop" }));
  $("#new-convo").addEventListener("click", () => post({ a: "newConversation" }));
  $("#retry").addEventListener("click", () => post({ a: "retry" }));
  $("#convo").addEventListener("scroll", () => { const el = $("#convo"); convoStick = el.scrollTop >= el.scrollHeight - el.clientHeight - 24; });

  // Voce
  $("#mic-select").addEventListener("change", (e) => post({ a: "setMic", name: e.target.value }));
  $("#output-select").addEventListener("change", (e) => post({ a: "setOutput", name: e.target.value }));
  $("#voice-select").addEventListener("change", (e) => post({ a: "setVoice", name: e.target.value }));
  $("#effect-select").addEventListener("change", (e) => post({ a: "setEffect", id: e.target.value }));
  $$("#engine-seg button").forEach((b) => b.addEventListener("click", () => post({ a: "setEngine", engine: b.dataset.engine })));

  // Moduli con una casella e un pulsante
  const form = (sel, send) => $(sel).addEventListener("submit", (e) => {
    e.preventDefault();
    const input = $(sel + " input"), text = input.value.trim();
    if (send(text, input) !== false) input.value = "";
  });
  form("#game-test", (text) => text && post({ a: "gameTest", text }));
  form("#translate-test", (text) => text && post({ a: "translateTest", text }));
  form("#mining-test", (text) => { if (text) post({ a: "miningTest", text }); return false; });
  form("#filter-form", (text) => { post({ a: "filterSave", tag: text }); return false; });
  form("#name-form", (text) => { post({ a: "set", k: "userName", v: text }); toast("Salvato."); return false; });
  $("#dry-run").addEventListener("change", (e) => post({ a: "dryRun", on: e.target.checked }));

  // Mappa stellare
  const zones = ["Casella di ricerca (un punto)", "Zona dei risultati", "Pulsante Imposta rotta", "Pulsante Annulla (rotta attiva)"];
  $("#zones").replaceChildren(...zones.map((name, zone) => h("div", { class: "zone" }, h("span", {}, name),
    h("button", { class: "btn small", type: "button", onclick: () => post({ a: "calibrate", zone }) }, "Calibra"),
    h("button", { class: "btn small", type: "button", onclick: () => post({ a: "readZone", zone }) }, "Leggi"))));

  // Mining
  let countdown = null;
  $("#mining-scan").addEventListener("click", (e) => {
    const b = e.currentTarget;
    if (countdown) { clearInterval(countdown); countdown = null; b.textContent = "Leggi tra 3 secondi"; b.classList.remove("busy"); return; }
    let left = 3;
    b.classList.add("busy");
    b.textContent = `Lettura tra ${left}…`;
    countdown = setInterval(() => {
      left -= 1;
      if (left > 0) { b.textContent = `Lettura tra ${left}…`; return; }
      clearInterval(countdown); countdown = null;
      b.textContent = "Leggi tra 3 secondi"; b.classList.remove("busy");
      post({ a: "miningScan" });
    }, 1000);
  });
  $("#materials").addEventListener("input", () => { tableDirty = true; $("#table-state").textContent = "modifiche non salvate"; });
  $("#table-save").addEventListener("click", () => { tableDirty = false; tableSaving = true; post({ a: "tableSave", text: $("#materials").value }); });
  $("#table-reset").addEventListener("click", async () => {
    if (await ask("Tabella predefinita", "Rimettere la tabella dei materiali predefinita? Quella attuale si perde.", [["Annulla", null], ["Rimetti", true, "danger"]])) {
      tableDirty = false; post({ a: "tableReset" });
    }
  });
  $("#table-import").addEventListener("click", async () => {
    const replace = await ask("Importa i materiali", "Come uso la tabella del file?", [["Annulla", null], ["Unisci", false], ["Sostituisci tutto", true, "primary"]]);
    if (replace !== null) { tableDirty = false; post({ a: "tableImport", replace }); }
  });
  $("#identify").addEventListener("submit", (e) => {
    e.preventDefault();
    const f = e.currentTarget.elements; // (form.name sarebbe il nome del modulo, non la casella)
    post({ a: "identify", value: f.namedItem("value").value.replace(/\D/g, ""), name: f.namedItem("name").value.trim(), qty: f.namedItem("qty").value.replace(/\D/g, "") });
    for (const n of ["value", "name", "qty"]) f.namedItem(n).value = "";
  });
  $("#unknown-remove").addEventListener("click", () => {
    const value = $("#identify [name=value]").value.replace(/\D/g, "");
    if (!value) return toast("Scrivi nel campo Valore la signature da togliere.", false);
    post({ a: "removeUnknown", value });
    $("#identify [name=value]").value = "";
  });
  $("#unknown-clear").addEventListener("click", async () => {
    if (await ask("Signature sconosciute", "Svuotare l'elenco delle signature sconosciute?", [["Annulla", null], ["Svuota", true, "danger"]])) post({ a: "clearUnknowns" });
  });
  $("#filtered-clear").addEventListener("click", async () => {
    if (await ask("Messaggi filtrati", "Svuotare l'elenco dei messaggi filtrati?", [["Annulla", null], ["Svuota", true, "danger"]])) post({ a: "clearFiltered" });
  });

  // Aggiornamenti
  for (const b of [$("#update-install"), $("#update-card-install")]) {
    b.addEventListener("click", async () => {
      if (await ask("Aggiornamento", `Iris si chiude e riparte con la versione ${S.update ? S.update.latest : "nuova"} (i modelli si ricaricano).`, [["Più tardi", null], ["Riavvia e aggiorna", true, "primary"]])) {
        post({ a: "installUpdate" });
      }
    });
  }

  axisWire();
  logWire();
  commandsWire();

  document.addEventListener("visibilitychange", startOrb);
  // Niente trascinamento di immagini e testo fuori dalle caselle.
  document.addEventListener("dragstart", (e) => e.preventDefault());
}

buildSliders();
buildSegments();
wire();
let firstPage = "home";
try { firstPage = localStorage.getItem("iris.page") || "home"; } catch (e) { /* niente */ }
showPage(firstPage);
renderConversation([]);
post({ a: "ready" });
