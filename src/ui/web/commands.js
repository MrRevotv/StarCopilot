// Iris — editor dei comandi di gioco (pagina Star Citizen): elenco, ricerca, aggiunta rapida e la
// finestra per creare o cambiare un comando (frasi, tipo, tasti presi premendoli, conferma a voce).
// Si carica prima di app.js e usa i suoi aiuti ($, h, post, toast, ask, keyChip...) solo dentro le funzioni.
//   dal programma: {"t":"gameCommands"}, {"t":"gameCmdResult"}, {"t":"gameKeyCapture"}
//   al programma:  {"a":"gameCmdSave",...}, {"a":"gameCmdDelete","index":3}, {"a":"gameKeyCapture"}
"use strict";

const CMD = { data: { commands: [], keys: [], error: "" }, edit: null, capturing: -1, saving: false };

const CMD_TYPES = [
  ["press", "Premi un tasto", "Un tocco sul tasto: carrello, luci, scudi, porte…"],
  ["hold", "Tieni premuto", "Tiene giù il tasto per il tempo che scegli: atterraggio automatico, funzioni da tenere premute."],
  ["sequence", "Sequenza", "Più tasti uno dopo l'altro, con una pausa: come «prepara la nave»."],
];
const CMD_BADGE = { press: "TASTO", hold: "TIENI", sequence: "SEQUENZA", set_route: "ROTTA", cancel_route: "ROTTA", open_map: "MAPPA", close_map: "MAPPA", quantum_travel: "QUANTUM" };

// Idee pronte: le frasi ci sono, il tasto lo scegli tu (quello che hai in gioco).
const CMD_PRESETS = [
  { label: "Porte", intent: "press", phrases: ["apri le porte", "chiudi le porte", "porte"], say: "Porte." },
  { label: "Blocca le porte", intent: "press", phrases: ["blocca le porte", "sblocca le porte"], say: "Porte." },
  { label: "Modalità mining", intent: "press", phrases: ["modalità mining", "laser minerario", "mining mode"], say: "Mining." },
  { label: "Modalità salvage", intent: "press", phrases: ["modalità salvage", "modalità recupero"], say: "Salvage." },
  { label: "Modalità quantum", intent: "press", phrases: ["modalità quantum", "prepara il quantum"], say: "Quantum." },
  { label: "Atterraggio automatico", intent: "hold", phrases: ["atterraggio automatico", "atterra da sola", "autoland"], say: "Atterro.", ms: 1500 },
  { label: "Visuale esterna", intent: "press", phrases: ["visuale esterna", "terza persona", "cambia visuale"] },
  { label: "mobiGlas", intent: "press", phrases: ["apri il mobiglas", "mobiglas"] },
  { label: "Screenshot", intent: "press", phrases: ["fai uno screenshot", "screenshot", "foto"] },
];

const norm = (t) => String(t || "").toLowerCase().normalize("NFD").replace(/[̀-ͯ]/g, "").replace(/\s+/g, " ").trim();
const keyLabel = (name) => (name || "").replace(/_/g, " ");

function cmdTaken(phrase) {
  const n = norm(phrase);
  return CMD.data.commands.some((c) => c.phrases.some((p) => norm(p) === n));
}

// --- Elenco ---------------------------------------------------------------------------------------------
function renderCommands(v) {
  CMD.data = v;
  const total = v.commands.length;
  $("#cmd-count").textContent = total ? `${total} comandi` : "";
  // Aggiunta rapida: solo le idee che non hai già.
  const presets = CMD_PRESETS.filter((p) => !p.phrases.some(cmdTaken));
  $("#cmd-presets").replaceChildren(...(presets.length
    ? presets.map((p) => h("button", { type: "button", class: "preset", title: "Frasi: " + p.phrases.join(", "), onclick: () => openEditor(null, p) }, "+ " + p.label))
    : [h("span", { class: "key-empty" }, "Le idee pronte le hai già tutte.")]));
  drawCommandList();
}

function drawCommandList() {
  const q = norm($("#cmd-search").value);
  const items = CMD.data.commands.filter((c) => !q
    || c.phrases.some((p) => norm(p).includes(q))
    || c.steps.some((s) => norm(s.combo).includes(q) || norm(keyLabel(s.name)).includes(q))
    || norm(c.say).includes(q));
  const host = $("#cmd-list");
  host.replaceChildren(...(items.length ? items.map(commandRow) : [h("div", { class: "empty" }, q ? "Nessun comando con queste parole." : "Nessun comando.")]));
}

function stepChips(c) {
  if (!c.steps.length) return [h("span", { class: "key-empty" }, c.special ? "logica di Iris" : "— nessun tasto —")];
  const out = [];
  c.steps.forEach((s, i) => {
    if (i) out.push(h("span", { class: "step-arrow" }, "›"));
    out.push(h("span", { class: "key" + (s.combo ? "" : " missing"), title: s.name ? "nel profilo: " + keyLabel(s.name) : "" }, s.combo || keyLabel(s.name) || "?"));
  });
  if (c.intent === "hold") out.push(h("span", { class: "step-ms" }, (c.ms / 1000).toLocaleString("it-IT") + " s"));
  return out;
}

function commandRow(c) {
  const more = c.phrases.length - 1;
  return h("div", { class: "cmd-item", tabindex: "0", role: "button", "aria-label": "Modifica " + (c.phrases[0] || c.intent),
    onclick: (e) => { if (!e.target.closest("button")) openEditor(c); },
    onkeydown: (e) => { if (e.key === "Enter" && e.target === e.currentTarget) openEditor(c); } },
    h("span", { class: "cmd-badge " + (c.special ? "special" : c.intent) }, CMD_BADGE[c.intent] || "IRIS"),
    h("div", { class: "cmd-main" },
      h("b", {}, c.phrases[0] || c.intent),
      h("small", {}, more > 0 ? c.phrases.slice(1, 3).join(" · ") + (more > 2 ? ` · +${more - 2}` : "") : c.say ? "risponde «" + c.say + "»" : "")),
    h("div", { class: "cmd-keys" }, ...stepChips(c)),
    h("div", { class: "cmd-actions" },
      h("button", { class: "btn small", type: "button", onclick: () => openEditor(c) }, "Modifica"),
      c.special
        ? lockIcon()
        : h("button", { class: "icon-x", type: "button", title: "Togli il comando", onclick: () => deleteCommand(c) }, "✕")));
}

function lockIcon() {
  const el = h("span", { class: "cmd-lock", title: "Comando speciale di Iris: si cambiano le frasi, non si toglie" });
  el.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="5" y="11" width="14" height="9"/><path d="M8 11V8a4 4 0 018 0v3"/></svg>';
  return el;
}

async function deleteCommand(c) {
  if (await ask("Togli il comando", `Togliere «${c.phrases[0] || c.intent}»? Il tasto resta nel profilo.`, [["Annulla", null], ["Togli", true, "danger"]])) {
    post({ a: "gameCmdDelete", index: c.index });
  }
}

// --- Finestra di modifica -----------------------------------------------------------------------------------
function openEditor(command, preset) {
  const c = command || null;
  CMD.edit = {
    index: c ? c.index : -1,
    intent: c ? c.intent : (preset && preset.intent) || "press",
    special: c ? c.special : false,
    phrases: c ? c.phrases.slice() : preset ? preset.phrases.slice() : [],
    steps: c ? c.steps.map((s) => ({ name: s.name, combo: s.combo })) : [{ name: "", combo: "" }],
    ms: c ? c.ms : (preset && preset.ms) || 800,
    say: c ? c.say : (preset && preset.say) || "",
  };
  if (!CMD.edit.steps.length) CMD.edit.steps.push({ name: "", combo: "" });
  $("#cmd-title").textContent = c ? "Modifica comando" : preset ? "Nuovo comando · " + preset.label : "Nuovo comando";
  $("#cmd-say").value = CMD.edit.say;
  $("#cmd-phrase-input").value = "";
  $("#cmd-error").classList.add("hidden");
  CMD.holdSlider.update(CMD.edit.ms);
  drawEditor();
  $("#cmd-modal").classList.remove("hidden");
  setTimeout(() => (c || preset ? $("#cmd-save") : $("#cmd-phrase-input")).focus(), 30);
}

function closeEditor() {
  if (CMD.capturing >= 0) post({ a: "gameKeyCapture" }); // annulla la cattura in corso
  CMD.edit = null;
  $("#cmd-modal").classList.add("hidden");
}

function drawEditor() {
  const e = CMD.edit;
  if (!e) return;
  $("#cmd-type-field").classList.toggle("hidden", e.special);
  $("#cmd-special").classList.toggle("hidden", !e.special);
  $("#cmd-special").textContent = e.intent === "set_route"
    ? "La rotta è un comando speciale di Iris: puoi cambiarne le frasi. Scrivi {destination} dove dici il luogo, es. «rotta per {destination}»."
    : "È un comando speciale di Iris: puoi cambiarne le frasi e la conferma a voce.";
  $("#cmd-type").replaceChildren(...CMD_TYPES.map(([id, label]) =>
    h("button", { type: "button", class: e.intent === id ? "on" : null, onclick: () => {
      e.intent = id;
      if (id !== "sequence") e.steps = e.steps.slice(0, 1);
      drawEditor();
    } }, label)));
  const type = CMD_TYPES.find(([id]) => id === e.intent);
  $("#cmd-type-hint").textContent = type ? type[2] : "";

  // Frasi
  const box = $("#cmd-phrases"), input = $("#cmd-phrase-input");
  box.replaceChildren(...e.phrases.map((p, i) => h("span", { class: "phrase" + (cmdTakenElsewhere(p) ? " taken" : ""), title: cmdTakenElsewhere(p) ? "C'è già in un altro comando" : "" }, p,
    h("button", { type: "button", title: "Togli", onclick: () => { e.phrases.splice(i, 1); drawEditor(); } }, "✕"))), input);
  input.placeholder = e.phrases.length ? "un altro modo di dirlo…  (Invio)" : (e.intent === "set_route" ? "rotta per {destination}  (Invio)" : "es. apri le porte  (Invio)");

  // Tasti
  const keysVisible = !e.special;
  $("#cmd-keys-field").classList.toggle("hidden", !keysVisible);
  $("#cmd-hold-field").classList.toggle("hidden", e.special || e.intent !== "hold");
  $("#cmd-keys-label").textContent = e.intent === "sequence" ? "Tasti, in ordine" : "Tasto";
  $("#cmd-add-step").classList.toggle("hidden", e.intent !== "sequence");
  if (keysVisible) {
    const options = [["", "dal profilo…"], ...CMD.data.keys.slice().sort((a, b) => keyLabel(a.name).localeCompare(keyLabel(b.name)))
      .map((k) => [k.name, keyLabel(k.name) + " · " + k.combo])];
    $("#cmd-steps").replaceChildren(...e.steps.map((s, i) => {
      const capturing = CMD.capturing === i;
      const combo = h("input", { type: "text", class: "step-combo", maxlength: "40", spellcheck: "false", placeholder: "es. LAlt+N", "aria-label": "Tasto" });
      combo.value = s.combo;
      combo.addEventListener("input", () => { s.combo = combo.value.trim(); });
      const pick = h("select", { class: "step-pick", "aria-label": "Tasto già nel profilo" }, ...options.map(([v, t]) => h("option", { value: v }, t)));
      pick.value = CMD.data.keys.some((k) => k.name === s.name) ? s.name : "";
      pick.addEventListener("change", () => {
        const k = CMD.data.keys.find((x) => x.name === pick.value);
        s.name = k ? k.name : "";
        s.combo = k ? k.combo : s.combo;
        drawEditor();
      });
      const seq = e.intent === "sequence";
      return h("div", { class: "step" + (capturing ? " capturing" : "") },
        seq ? h("span", { class: "step-n" }, String(i + 1)) : null,
        combo,
        h("button", { class: "btn small" + (capturing ? " busy" : ""), type: "button", disabled: CMD.capturing >= 0 && !capturing,
          onclick: () => { CMD.capturing = capturing ? -1 : i; post({ a: "gameKeyCapture" }); drawEditor(); } },
          capturing ? "Premi il tasto… (Esc)" : "Premi il tasto"),
        pick,
        seq ? h("button", { class: "icon-x up", type: "button", title: "Su", disabled: i === 0, onclick: () => { [e.steps[i - 1], e.steps[i]] = [e.steps[i], e.steps[i - 1]]; drawEditor(); } }, "↑") : null,
        seq ? h("button", { class: "icon-x up", type: "button", title: "Giù", disabled: i === e.steps.length - 1, onclick: () => { [e.steps[i + 1], e.steps[i]] = [e.steps[i], e.steps[i + 1]]; drawEditor(); } }, "↓") : null,
        seq && e.steps.length > 1 ? h("button", { class: "icon-x", type: "button", title: "Togli il passo", onclick: () => { e.steps.splice(i, 1); drawEditor(); } }, "✕") : null,
        s.name && CMD.data.keys.some((k) => k.name === s.name)
          ? h("small", { class: "step-note" }, `Tasto «${keyLabel(s.name)}» del profilo: se lo cambi, vale per tutti i comandi che lo usano.`)
          : null);
    }));
  }
}

function cmdTakenElsewhere(phrase) {
  const n = norm(phrase), e = CMD.edit;
  return CMD.data.commands.some((c) => c.index !== (e ? e.index : -2) && c.phrases.some((p) => norm(p) === n));
}

function addPhraseFromInput() {
  const input = $("#cmd-phrase-input"), e = CMD.edit;
  const text = input.value.trim().replace(/^iris[,\s]+/i, "");
  if (!e || !text) return false;
  if (!e.phrases.some((p) => norm(p) === norm(text))) e.phrases.push(text);
  input.value = "";
  drawEditor();
  input.focus();
  return true;
}

function saveEditor() {
  const e = CMD.edit;
  if (!e || CMD.saving) return;
  addPhraseFromInput();
  e.say = $("#cmd-say").value.trim();
  CMD.saving = true;
  $("#cmd-save").classList.add("busy");
  post({ a: "gameCmdSave", index: e.index, intent: e.intent, phrases: e.phrases, steps: e.steps, ms: e.ms, say: e.say });
}

const commandHandlers = {
  gameCommands: renderCommands,
  gameCmdResult(v) {
    CMD.saving = false;
    $("#cmd-save").classList.remove("busy");
    if (v.action === "gameCmdSave") {
      if (v.ok) closeEditor();
      else {
        const el = $("#cmd-error");
        el.textContent = v.error;
        el.classList.remove("hidden");
      }
    } else if (!v.ok && v.error) {
      toast(v.error, false);
    }
  },
  gameKeyCapture(v) {
    if (v.active) return;
    const e = CMD.edit, i = CMD.capturing;
    CMD.capturing = -1;
    if (e && i >= 0 && e.steps[i] && v.combo) {
      e.steps[i].combo = v.combo;
      $("#cmd-error").classList.add("hidden");
    } else if (v.timeout) {
      toast("Nessun tasto premuto: riprova.", false);
    }
    drawEditor();
  },
};

function commandsWire() {
  $("#cmd-new").addEventListener("click", () => openEditor(null));
  $("#cmd-search").addEventListener("input", drawCommandList);
  $("#cmd-cancel").addEventListener("click", closeEditor);
  $("#cmd-save").addEventListener("click", saveEditor);
  $("#cmd-add-step").addEventListener("click", () => { if (CMD.edit) { CMD.edit.steps.push({ name: "", combo: "" }); drawEditor(); } });
  $("#cmd-phrase-input").addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") { ev.preventDefault(); addPhraseFromInput(); }
    if (ev.key === "Backspace" && !ev.target.value && CMD.edit && CMD.edit.phrases.length) { CMD.edit.phrases.pop(); drawEditor(); }
  });
  $("#cmd-phrases").addEventListener("click", (ev) => { if (ev.target.id === "cmd-phrases") $("#cmd-phrase-input").focus(); });
  CMD.holdSlider = axSlider({ label: "Quanto tenerlo premuto", min: 100, max: 5000, step: 100, fmt: (x) => (x / 1000).toLocaleString("it-IT") + " s" }, 800,
    (v) => { if (CMD.edit) CMD.edit.ms = v; });
  $("#cmd-hold-field").append(CMD.holdSlider);
  // Esc chiude la finestra (ma non mentre stai scegliendo un tasto: lì Esc annulla la scelta).
  document.addEventListener("keydown", (ev) => {
    if (ev.key === "Escape" && CMD.edit && CMD.capturing < 0 && !$("#cmd-modal").classList.contains("hidden")) closeEditor();
  });
  $("#cmd-modal").addEventListener("mousedown", (ev) => { if (ev.target.id === "cmd-modal" && CMD.capturing < 0) closeEditor(); });
}
