// Iris — Diario di bordo (Game.log), timer della raffineria, avvio con Windows e barra rapida.
// Si carica prima di app.js e usa i suoi aiuti ($, h, post, toast, ask, setChip, keyChip, S) solo
// dentro le funzioni.
//   dal programma: {"t":"gameLog"}, {"t":"refinery"}
//   al programma:  {"a":"gameLogEnable","on":true,"accepted":true}, {"a":"refineryAdd",...}, {"a":"autostart","on":true}...
"use strict";

// L'avviso, uguale ovunque (pagina, conferma, README).
const GAMELOG_TOS =
  "La lettura del file Game.log è vietata dai Termini di Servizio di Star Citizen. " +
  "Queste funzioni sono spente in partenza e vanno attivate consapevolmente, a tuo rischio: " +
  "lo sviluppatore non si assume alcuna responsabilità per eventuali conseguenze causate dall'uso improprio di queste funzioni.";

const LOG = { data: null, filter: "all" };
const LOG_FILTERS = [["all", "Tutti"], ["missions", "Missioni"], ["zones", "Zone"], ["health", "Salute"], ["party", "Gruppo"], ["other", "Altro"]];

function logCategory(kind) {
  if (kind === "contract" || kind === "objective") return "missions";
  if (kind === "zone" || kind === "jurisdiction" || kind === "location") return "zones";
  if (kind === "health" || kind === "combat") return "health";
  if (kind === "party") return "party";
  return "other";
}

// Ora locale da secondi UTC: "21:40", o "12/10 21:40" se non è oggi.
function clockOf(epoch, withDay) {
  if (!epoch) return "";
  const d = new Date(epoch * 1000), now = new Date();
  const hm = String(d.getHours()).padStart(2, "0") + ":" + String(d.getMinutes()).padStart(2, "0");
  if (d.toDateString() === now.toDateString()) return hm;
  const day = String(d.getDate()).padStart(2, "0") + "/" + String(d.getMonth() + 1).padStart(2, "0");
  return withDay === false ? day : day + " " + hm;
}

function kv(label, value, kind) {
  return h("div", { class: "kv" }, h("span", {}, label), h("b", { class: kind || null }, value || "—"));
}

function zoneValue(v, yes, no) {
  if (v === 1) return [yes, "ok"];
  if (v === 0) return [no, "warn"];
  return ["—", "dim"];
}

function renderGameLog(v) {
  LOG.data = v;
  const on = v.enabled;
  $("#log-enable").checked = on;
  $("#led-log").classList.toggle("on", on && v.found);
  setChip("#log-chip", !on ? ["", "Spento"] : !v.found ? ["warn", "Game.log non trovato"] : v.catchingUp ? ["warn", "Rileggo la sessione…"] : ["ok", "Attivo"]);
  $("#log-state").textContent = !on
    ? "Spento: il Game.log non viene letto."
    : !v.found
      ? "Attivo, ma il Game.log non c'è ancora: avvia il gioco (lo cerco ogni pochi secondi)."
      : `Attivo · ${v.lines.toLocaleString("it-IT")} righe lette, ${v.eventCount} eventi riconosciuti.`;
  $("#log-path").textContent = v.path
    ? v.path + (v.custom ? "  ·  scelto a mano" : "  ·  trovato da sola")
    : "Lo cerco da sola nelle cartelle di Star Citizen (LIVE, PTU, EPTU…).";
  $("#log-auto").disabled = !v.custom;
  $$(".log-body").forEach((p) => p.classList.toggle("locked", !on));

  // Missioni
  const contracts = $("#log-contracts");
  contracts.replaceChildren(...(v.contracts.length
    ? v.contracts.map((c) => h("div", { class: "mission" },
      h("span", { class: "mission-dot" }),
      h("div", { class: "mission-text" }, h("b", {}, c.name), h("small", {}, c.since ? "accettato " + clockOf(c.since) : "")),
      h("button", { class: "icon-x", type: "button", title: "Togli dall'elenco (se l'hai già chiuso nel gioco)", onclick: () => post({ a: "gameLogForget", id: c.id }) }, "✕")))
    : [h("div", { class: "empty" }, on ? "Nessun contratto in corso." : "Attiva la lettura per vedere i contratti.")]));
  const objectives = $("#log-objectives");
  objectives.replaceChildren(...(v.objectives.length
    ? v.objectives.map((o) => h("div", { class: "objective" }, h("span", {}, "›"), o.text))
    : [h("div", { class: "empty" }, "Nessun obiettivo in corso.")]));
  $("#log-session").replaceChildren(
    h("span", { class: "stat" }, h("b", {}, String(v.session.accepted)), "accettati"),
    h("span", { class: "stat ok" }, h("b", {}, String(v.session.completed)), "completati"),
    h("span", { class: "stat bad" }, h("b", {}, String(v.session.failed)), "falliti"));

  // Stato
  const z = v.zone;
  const [arm, armK] = zoneValue(z.armistice, "Sì · combattimento proibito", "No · cautela");
  const [mon, monK] = zoneValue(z.monitored, "Sì", "No · niente comunicazioni");
  const [pri, priK] = zoneValue(z.privateProperty, "Sì", "No");
  $("#log-status").replaceChildren(
    kv("Giocatore", v.handle),
    kv("Giurisdizione", v.jurisdiction),
    kv("Ultima località", v.location),
    kv("Zona di armistizio", arm, armK),
    kv("Spazio monitorato", mon, monK),
    kv("Proprietà privata", pri, z.privateProperty === 1 ? "warn" : priK === "warn" ? "dim" : priK),
    kv("Ultima nave pilotata", v.lastShip));
  const recentHealth = v.health.filter((m) => !m.epoch || v.now - m.epoch < 30 * 60);
  $("#log-health").replaceChildren(...(recentHealth.length
    ? recentHealth.slice().reverse().map((m) => h("div", { class: "health-item" }, h("span", {}, m.text), h("time", {}, clockOf(m.epoch))))
    : [h("div", { class: "empty" }, "Niente da segnalare nell'ultima mezz'ora.")]));

  renderLogFeed();
}

function renderLogFeed() {
  const v = LOG.data;
  if (!v) return;
  const host = $("#log-feed");
  const items = v.events.slice().reverse().filter((e) => LOG.filter === "all" || logCategory(e.kind) === LOG.filter);
  host.replaceChildren(...items.map((e) => h("div", { class: "feed-item " + e.level },
    h("time", {}, clockOf(e.epoch)), h("span", { class: "feed-cat" }, LOG_FILTERS.find(([id]) => id === logCategory(e.kind))[1]), h("span", { class: "feed-text" }, e.text))));
  host.dataset.empty = v.enabled ? "Ancora nessun evento." : "Diario spento.";
  $$("#log-filter button").forEach((b) => b.classList.toggle("on", b.dataset.filter === LOG.filter));
}

// --- Raffineria -----------------------------------------------------------------------------------------
const REF = { jobs: [], offset: 0 };

function formatLeft(s) {
  if (s <= 0) return "pronto";
  const d = Math.floor(s / 86400), hh = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60), sec = Math.floor(s % 60);
  if (d > 0) return `${d} g ${hh} h`;
  if (hh > 0) return `${hh} h ${String(m).padStart(2, "0")} min`;
  if (m > 0) return `${m} min ${String(sec).padStart(2, "0")} s`;
  return `${sec} s`;
}

function renderRefinery(v) {
  REF.jobs = v.jobs || [];
  REF.offset = v.now - Date.now() / 1000; // l'orologio del programma (stesso PC: di solito 0)
  drawRefinery();
}

function drawRefinery() {
  const host = $("#refinery-list");
  if (!host) return;
  const now = Date.now() / 1000 + REF.offset;
  const done = REF.jobs.filter((j) => now >= j.end).length;
  $("#refinery-clear").disabled = done === 0;
  $("#refinery-meta").textContent = REF.jobs.length ? `${REF.jobs.length} in lavorazione${done ? ", " + done + " pronti" : ""}` : "";
  host.replaceChildren(...(REF.jobs.length
    ? REF.jobs.map((j) => {
      const ready = now >= j.end, pct = Math.min(1, Math.max(0, (now - j.start) / Math.max(1, j.end - j.start)));
      return h("div", { class: "ref-item" + (ready ? " done" : "") },
        h("div", { class: "ref-head" },
          h("b", {}, j.place || "Ordine"),
          j.note ? h("span", { class: "ref-note" }, j.note) : null,
          h("span", { class: "ref-time" }, ready ? "PRONTO" : formatLeft(j.end - now)),
          h("button", { class: "icon-x", type: "button", title: "Togli", onclick: () => post({ a: "refineryRemove", id: j.id }) }, "✕")),
        h("div", { class: "ref-bar" }, h("i", { style: `width:${(pct * 100).toFixed(1)}%` })),
        h("small", { class: "ref-end" }, ready ? "pronto dalle " + clockOf(j.end) : "pronto alle " + clockOf(j.end)));
    })
    : [h("div", { class: "empty" }, "Nessun ordine in lavorazione. Aggiungilo qui sotto, dalla barra rapida o a voce.")]));
}

// --- Impostazioni: avvio con Windows, tasti della barra rapida ------------------------------------------
const BAR_KEYS = ["Mostra / nascondi la barra", "Nascondi tutto (e rimetti)"];

function renderExtraSettings(s) {
  const auto = $("#autostart");
  if (!auto) return;
  auto.checked = !!s.autostart;
  $("#autostart-note").textContent = s.autostartElsewhere
    ? "Attivo per un'altra copia di Iris (exe spostato): spegni e riaccendi per usare questa."
    : "Parte nascosta nell'area di notifica, pronta per quando giochi.";
  $("#autostart-note").classList.toggle("warn-text", !!s.autostartElsewhere);

  const host = $("#bar-keys");
  const rows = s.barKeys || [[], []];
  const cells = [];
  BAR_KEYS.forEach((name, i) => {
    const target = "mining:" + (i + 4), keys = rows[i] || [], capturing = S.capture === target;
    cells.push(h("div", { class: "label" }, name));
    cells.push(h("div", { class: "cell" },
      capturing ? h("span", { class: "key-empty" }, "premi il tasto…") : keys.length ? keyChip(keys[0]) : h("span", { class: "key-empty" }, "— nessuno —"),
      h("button", { class: "btn small" + (capturing ? " busy" : ""), type: "button", disabled: !s.quickBarEnabled && !capturing, onclick: () => post({ a: "captureKey", target }) }, capturing ? "Annulla" : "Cambia"),
      keys.length ? h("button", { class: "btn small danger", type: "button", onclick: () => post({ a: "removeKey", target }) }, "Togli") : null));
  });
  host.replaceChildren(...cells);
  if ((S.captureText.target === "mining:4" || S.captureText.target === "mining:5") && S.captureText.text) {
    host.append(h("div", { class: "capture-hint", style: "grid-column: 1 / -1" }, S.captureText.text));
  }
  $("#bar-show").disabled = !s.quickBarEnabled;
}

const logHandlers = {
  gameLog: renderGameLog,
  refinery: renderRefinery,
  miningLog: renderMiningLog,
  miningLogAsk() { showPage("mining"); openMiningLogDialog(); },
  miningLogResult(v) {
    $("#mlog-save").classList.remove("busy");
    if (v.ok) $("#mlog-modal").classList.add("hidden");
    else showMlogError(v.error || "Non sono riuscita a salvare il file.");
  },
};

function logWire() {
  $("#log-tos-text").textContent = GAMELOG_TOS;
  $("#log-enable").addEventListener("change", async (e) => {
    const want = e.target.checked;
    e.target.checked = !want; // lo stato vero lo decide il programma
    if (!want) return post({ a: "gameLogEnable", on: false });
    const ok = await ask("Lettura del Game.log", GAMELOG_TOS + " Vuoi attivarla lo stesso?", [["Annulla", null], ["Attiva a mio rischio", true, "danger"]]);
    if (ok) post({ a: "gameLogEnable", on: true, accepted: true });
  });
  $("#log-choose").addEventListener("click", () => post({ a: "gameLogPath" }));
  $("#log-auto").addEventListener("click", () => post({ a: "gameLogPathAuto" }));
  $("#log-clear").addEventListener("click", () => post({ a: "gameLogClear" }));
  $("#log-filter").replaceChildren(...LOG_FILTERS.map(([id, label]) =>
    h("button", { type: "button", "data-filter": id, onclick: () => { LOG.filter = id; renderLogFeed(); } }, label)));
  $$(".log-bar-show, #bar-show").forEach((b) => b.addEventListener("click", () => post({ a: "quickBarShow" })));

  $("#refinery-form").addEventListener("submit", (e) => {
    e.preventDefault();
    const f = e.currentTarget.elements;
    const duration = f.namedItem("duration").value.trim();
    if (!duration) return toast("Scrivi quanto manca: per esempio 3:20, 3h20 o 200 (minuti).", false);
    post({ a: "refineryAdd", place: f.namedItem("place").value.trim(), note: f.namedItem("note").value.trim(), duration });
    for (const n of ["duration", "note"]) f.namedItem(n).value = "";
  });
  $("#refinery-clear").addEventListener("click", () => post({ a: "refineryClearDone" }));
  setInterval(() => { if (S.page === "mining" && REF.jobs.length && !document.hidden) drawRefinery(); }, 1000);

  miningLogWire();

  $("#autostart").addEventListener("change", (e) => {
    const on = e.target.checked;
    e.target.checked = !on;
    post({ a: "autostart", on });
  });
}

// --- Registro delle scansioni del mining --------------------------------------------------------------------
const MLOG = { data: null, offset: 0 };

function durationText(seconds) {
  const m = Math.max(0, Math.round(seconds / 60));
  return m < 60 ? `${m} min` : `${Math.floor(m / 60)} h ${String(m % 60).padStart(2, "0")} min`;
}

function mlogResult(e) {
  if (e.outcome === "certo") return [`${e.label} × ${e.quantity}`, "ok"];
  if (e.outcome === "probabile") return [`${e.label} × ${e.quantity} (probabile)`, "warn"];
  if (e.outcome === "ambiguo") return [e.alternatives.join(" o "), "warn"];
  return ["sconosciuta", "bad"];
}

function renderMiningLog(v) {
  MLOG.data = v;
  MLOG.offset = v.now - Date.now() / 1000;
  const on = v.active;
  $("#mlog-switch").checked = on;
  $("#mlog-live").classList.toggle("hidden", !on);
  $("#mlog-last").classList.toggle("hidden", on || !v.lastFile);
  $("#mlog-last-name").textContent = v.lastFileName;
  $("#mlog-open").disabled = !v.lastFileExists;
  $("#mlog-folder").disabled = !v.lastFileExists;
  if (on) {
    $("#mlog-totals").replaceChildren(...(v.summary.materials.length
      ? v.summary.materials.map((m) => h("span", { class: "mlog-total" }, h("b", {}, m.label), `${m.rocks} ${m.rocks === 1 ? "roccia" : "rocce"} · ${m.quantity}`))
      : [h("span", { class: "key-empty" }, "Ancora nessun materiale certo.")]));
    $("#mlog-feed").replaceChildren(...v.entries.map((e) => {
      const [text, kind] = mlogResult(e);
      return h("div", { class: "feed-item " + kind },
        h("time", {}, e.time), h("span", { class: "feed-cat" }, String(e.value)),
        h("span", { class: "feed-text" }, text, e.reads > 1 ? h("small", { class: "mlog-reads" }, ` · letta ${e.reads} volte`) : null));
    }));
    $("#mlog-feed").dataset.empty = "Leggi una signature (tasto, voce o lettura continua): comparirà qui.";
  }
  drawMlogStats();
}

function drawMlogStats() {
  const v = MLOG.data;
  if (!v || !v.active) return;
  const now = Date.now() / 1000 + MLOG.offset;
  const s = v.summary;
  $("#mlog-state").textContent = `In registrazione dalle ${clockOf(v.start)}${v.operator ? " · " + v.operator : ""}.`;
  $("#mlog-stats").replaceChildren(
    h("span", { class: "stat" }, h("b", {}, String(s.finds)), "rilevamenti"),
    h("span", { class: "stat ok" }, h("b", {}, String(s.materials.length)), "materiali"),
    h("span", { class: "stat" + (s.toCheck ? " warn" : "") }, h("b", {}, String(s.toCheck)), "da verificare"),
    h("span", { class: "stat" }, h("b", {}, durationText(now - v.start)), "durata"));
}

function showMlogError(text) {
  const el = $("#mlog-error");
  el.textContent = text;
  el.classList.remove("hidden");
}

async function openMiningLogDialog() {
  const v = MLOG.data;
  if (!v || !v.active) return;
  if (v.summary.finds === 0) {
    if (await ask("Chiudi il registro", "Nel registro non c'è ancora nessuna lettura: lo chiudo senza creare il file?",
      [["Continua a registrare", null], ["Chiudi senza file", true, "danger"]])) post({ a: "miningLogCancel" });
    return;
  }
  const now = Date.now() / 1000 + MLOG.offset;
  $("#mlog-sum").textContent = `${v.summary.finds} rilevamenti in ${durationText(now - v.start)}` +
    (v.summary.materials.length ? `: ${v.summary.materials.slice(0, 3).map((m) => m.label).join(", ")}${v.summary.materials.length > 3 ? "…" : ""}.` : ".") +
    ` Il file va sul desktop${v.desktop ? " (" + v.desktop + ")" : ""}.`;
  const places = [...new Set([v.suggestedPlace, ...v.places].filter(Boolean))];
  $("#mlog-places").replaceChildren(...places.map((p) => h("option", { value: p })));
  const place = $("#mlog-place");
  place.value = v.suggestedPlace || "";
  $("#mlog-quality").value = "";
  $("#mlog-csv").checked = !!(S.settings && S.settings.miningLogCsv);
  $("#mlog-error").classList.add("hidden");
  $("#mlog-save").classList.remove("busy");
  $("#mlog-modal").classList.remove("hidden");
  setTimeout(() => (place.value ? $("#mlog-quality") : place).focus(), 30);
}

function saveMiningLog() {
  const place = $("#mlog-place").value.trim();
  const qualityText = $("#mlog-quality").value.trim().replace(",", ".");
  const quality = Number(qualityText);
  if (!place) return showMlogError("Scrivi in quale luogo ti trovavi.");
  if (!qualityText || !Number.isFinite(quality) || quality < 0 || quality > 1000) return showMlogError("La qualità media è un numero da 0 a 1000.");
  $("#mlog-error").classList.add("hidden");
  $("#mlog-save").classList.add("busy");
  post({ a: "miningLogFinish", place, quality: Math.round(quality), csv: $("#mlog-csv").checked });
}

function miningLogWire() {
  $("#mlog-switch").addEventListener("change", (e) => {
    const want = e.target.checked;
    e.target.checked = !want; // lo stato vero lo decide il programma
    if (want) post({ a: "miningLogStart" });
    else openMiningLogDialog();
  });
  $("#mlog-close").addEventListener("click", openMiningLogDialog);
  $("#mlog-cancel").addEventListener("click", async () => {
    const n = MLOG.data ? MLOG.data.summary.finds : 0;
    if (await ask("Annulla il registro", `Buttare via il registro${n ? " con " + n + " rilevamenti" : ""}? Nessun file verrà creato.`,
      [["Tienilo", null], ["Butta via", true, "danger"]])) post({ a: "miningLogCancel" });
  });
  $("#mlog-resume").addEventListener("click", () => $("#mlog-modal").classList.add("hidden"));
  $("#mlog-save").addEventListener("click", saveMiningLog);
  for (const id of ["#mlog-place", "#mlog-quality"]) {
    $(id).addEventListener("keydown", (e) => { if (e.key === "Enter") { e.preventDefault(); saveMiningLog(); } });
  }
  $("#mlog-quality").addEventListener("input", (e) => { e.target.value = e.target.value.replace(/[^\d]/g, ""); });
  $("#mlog-open").addEventListener("click", () => post({ a: "miningLogOpen" }));
  $("#mlog-folder").addEventListener("click", () => post({ a: "miningLogFolder" }));
  document.addEventListener("keydown", (e) => { if (e.key === "Escape" && !$("#mlog-modal").classList.contains("hidden")) $("#mlog-modal").classList.add("hidden"); });
  setInterval(() => { if (S.page === "mining" && !document.hidden) drawMlogStats(); }, 1000);
}
