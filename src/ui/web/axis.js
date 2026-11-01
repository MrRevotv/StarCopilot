// Iris — la pagina dell'Axis Visualizer: overlay, profili, moduli con l'anteprima dal vivo,
// aspetto e server per OBS. Si carica prima di app.js e usa i suoi aiuti ($, h, post, toast,
// ask, setChip) solo dentro le funzioni, quando ci sono già.
//   dal programma: {"t":"axis"} (tutto), {"t":"axisPreview"} (forma dei moduli), {"t":"axisValues"} (~30 al secondo)
//   al programma:  {"a":"axisOpen"}, {"a":"axisPane","index":0,"k":"Label","v":"..."}, ...
"use strict";

const AX = {
  state: null,      // l'ultimo {"t":"axis"}
  layouts: [],      // la forma di ogni modulo (null = modulo senza periferica)
  font: 11, bold: false, opacity: 1,
  values: [],       // i numeri dell'ultimo fotogramma, per modulo
  previews: [],     // canvas + stato locale (scia), per modulo
  listKey: "",      // cosa c'è disegnato nell'elenco dei moduli
  pending: false,   // l'elenco è cambiato mentre scrivevi: si ridisegna quando esci dalla casella
  lastFrame: 0,
  looping: false,
};

const AXIS_KIND_LABEL = { Assi: "Assi", Barre: "Barre", Pulsanti: "Pulsanti" };
const AXIS_KIND_HINT = {
  Assi: "Il puntino si muove con due assi (cloche, levetta, mouse); il terzo è la barra sotto.",
  Barre: "Una barra per asse: manetta, timone, levette, cursori.",
  Pulsanti: "Le caselle si accendono quando premi; anche cappelli e rotella del mouse.",
};

// =====================================================================================================
// Il disegno: lo stesso di overlay.html (la sorgente Browser di OBS) e di axis_panes.cpp (lo schermo)
// =====================================================================================================
const AxisPaint = (() => {
  const PAD = 10, CORNER = 7;
  const BAR_ROW = 26, ROW_H = 18, ROW_GAP = 6, VALUE_W = 44, NAME_W = 96;
  const CELL_GAP = 5, HAT_SIZE = 46, HAT_GAP = 10, TRAIL_SAMPLES = 26;
  const FONT_BASE = 11;
  const FONT = 'px Consolas, "Cascadia Mono", "Courier New", monospace';
  let font = FONT_BASE, bold = false;
  let barRow = BAR_ROW, rowH = ROW_H, valueW = VALUE_W, nameW = NAME_W;

  function applyFont(size, isBold) {
    font = size > 0 ? size : FONT_BASE;
    bold = !!isBold;
    const s = font / FONT_BASE;
    barRow = Math.round(BAR_ROW * s);
    rowH = Math.round(ROW_H * s);
    valueW = Math.round(VALUE_W * s);
    nameW = Math.round(NAME_W * s);
  }

  function rgba(hex, alpha) {
    const n = parseInt(String(hex || "#000000").slice(1), 16) || 0;
    return "rgba(" + ((n >> 16) & 255) + "," + ((n >> 8) & 255) + "," + (n & 255) + "," + alpha + ")";
  }
  function contrast(hex) {
    const n = parseInt(String(hex || "#000000").slice(1), 16) || 0;
    const luminance = (0.299 * ((n >> 16) & 255) + 0.587 * ((n >> 8) & 255) + 0.114 * (n & 255)) / 255;
    return luminance > 0.55 ? "#04181C" : "#FFFFFF";
  }
  function theme(p) {
    return {
      bg: rgba(p.bg, p.bgAlpha), dot: p.dot, dotSoft: rgba(p.dot, 0.2), bar: p.barColor, accent: p.accent,
      text: "#FFFFFF", muted: "rgba(255,255,255,0.6)", inverted: contrast(p.dot),
      frame: rgba(p.accent, 0.4), rule: rgba(p.accent, 0.2), track: "rgba(255,255,255,0.24)",
      cell: rgba(p.accent, 0.35), grid: "rgba(255,255,255,0.3)", barDash: rgba(p.barColor, 0.5),
    };
  }

  function line(ctx, x1, y1, x2, y2, color, dash) {
    ctx.save(); ctx.strokeStyle = color; ctx.lineWidth = 1;
    if (dash) ctx.setLineDash([4, 3]);
    ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x2, y2); ctx.stroke(); ctx.restore();
  }
  function strokeRect(ctx, x, y, w, hh, color) { ctx.save(); ctx.strokeStyle = color; ctx.lineWidth = 1; ctx.strokeRect(x, y, w, hh); ctx.restore(); }
  function fillRect(ctx, x, y, w, hh, color) { ctx.fillStyle = color; ctx.fillRect(x, y, w, hh); }
  function circle(ctx, x, y, r, fill, stroke) {
    ctx.beginPath(); ctx.arc(x, y, Math.max(0, r), 0, Math.PI * 2);
    if (fill) { ctx.fillStyle = fill; ctx.fill(); }
    if (stroke) { ctx.strokeStyle = stroke; ctx.lineWidth = 1; ctx.stroke(); }
  }
  function text(ctx, content, x, y, size, color, baseline) {
    ctx.font = (bold ? "bold " : "") + size + FONT; ctx.fillStyle = color; ctx.textBaseline = baseline || "top"; ctx.fillText(content, x, y);
  }
  function width(ctx, content, size) { ctx.font = (bold ? "bold " : "") + size + FONT; return ctx.measureText(content).width; }
  const textHeight = (size) => size * 1.36;
  function percent(value) { const n = Math.round(value * 100); return (n > 0 ? "+" : "") + n + "%"; }
  function ellipsis(ctx, content, size, max) {
    if (width(ctx, content, size) <= max) return content;
    let cut = content;
    while (cut.length > 1 && width(ctx, cut + "…", size) > max) cut = cut.slice(0, -1);
    return cut + "…";
  }

  function drawPane(pane, dt, opacity) {
    const p = pane.layout, ctx = pane.ctx, t = pane.theme;
    ctx.setTransform(pane.scale, 0, 0, pane.scale, 0, 0);
    ctx.clearRect(0, 0, p.w, p.h);
    ctx.globalAlpha = opacity;
    if (p.bgAlpha > 0) fillRect(ctx, 0, 0, p.w, p.h, t.bg);
    if (p.frame) drawFrame(ctx, t, p.w, p.h);
    let top = PAD;
    if (p.titleBand > 0) { drawTitle(ctx, p, t, PAD, top, p.w - 2 * PAD); top += p.titleBand; }
    const area = { x: PAD, y: top, w: p.w - 2 * PAD, h: p.h - top - PAD };
    if (area.w <= 0 || area.h <= 0) return;
    if (p.kind === "assi") drawAxes(ctx, pane, area, dt);
    else if (p.kind === "barre") drawBars(ctx, pane, area);
    else if (p.kind === "pulsanti") drawButtons(ctx, pane, area);
  }

  function drawFrame(ctx, t, w, hh) {
    strokeRect(ctx, 0.5, 0.5, Math.max(1, w - 1), Math.max(1, hh - 1), t.rule);
    const x0 = 0.5, y0 = 0.5, x1 = w - 0.5, y1 = hh - 0.5;
    line(ctx, x0, y0, x0 + CORNER, y0, t.frame); line(ctx, x0, y0, x0, y0 + CORNER, t.frame);
    line(ctx, x1, y0, x1 - CORNER, y0, t.frame); line(ctx, x1, y0, x1, y0 + CORNER, t.frame);
    line(ctx, x0, y1, x0 + CORNER, y1, t.frame); line(ctx, x0, y1, x0, y1 - CORNER, t.frame);
    line(ctx, x1, y1, x1 - CORNER, y1, t.frame); line(ctx, x1, y1, x1, y1 - CORNER, t.frame);
  }

  function drawTitle(ctx, p, t, left, top, w) {
    const tw = p.titleWidth || width(ctx, p.title, font);
    let x = left;
    if (p.align === "right") x = left + w - tw;
    else if (p.align === "center") x = left + (w - tw) / 2;
    x = Math.max(left, x);
    text(ctx, p.title, x, top, font, t.accent);
    const ruleY = Math.round(top + textHeight(font) / 2) + 0.5;
    if (x > left + 6) line(ctx, left, ruleY, x - 6, ruleY, t.rule);
    if (x + tw + 6 < left + w) line(ctx, x + tw + 6, ruleY, left + w, ruleY, t.rule);
  }

  function drawAxes(ctx, pane, area, dt) {
    const p = pane.layout, t = pane.theme;
    const x = pane.values[0] || 0, y = pane.values[1] || 0, z = pane.values[2] || 0;
    const plot = { x: area.x, y: area.y, w: area.w, h: area.w };
    const dotR = p.dotRadius || 0;
    const cx = plot.x + plot.w / 2, cy = plot.y + plot.h / 2;
    const reach = plot.w / 2 - dotR;
    strokeRect(ctx, plot.x + 0.5, plot.y + 0.5, plot.w - 1, plot.h - 1, t.rule);
    if (p.grid) {
      line(ctx, cx, plot.y + 4, cx, plot.y + plot.h - 4, t.grid, true);
      line(ctx, plot.x + 4, cy, plot.x + plot.w - 4, cy, t.grid, true);
    }
    for (let i = 1; i <= 3; i++) {
      const len = i === 2 ? 6 : 3, tx = plot.x + plot.w * i / 4, ty = plot.y + plot.h * i / 4;
      line(ctx, tx, plot.y, tx, plot.y + len, t.frame); line(ctx, tx, plot.y + plot.h, tx, plot.y + plot.h - len, t.frame);
      line(ctx, plot.x, ty, plot.x + len, ty, t.frame); line(ctx, plot.x + plot.w, ty, plot.x + plot.w - len, ty, t.frame);
    }
    const dotX = cx + x * reach, dotY = cy + y * reach;
    if (p.trailSeconds > 0.01) {
      pane.trailClock += dt;
      if (pane.trailClock >= p.trailSeconds / TRAIL_SAMPLES) {
        pane.trailClock = 0;
        pane.trail.unshift([x, y]);
        if (pane.trail.length > TRAIL_SAMPLES) pane.trail.length = TRAIL_SAMPLES;
      }
      for (let i = pane.trail.length - 1; i >= 0; i--) {
        const r = dotR * 0.9 * (1 - i / TRAIL_SAMPLES);
        if (r >= 0.7) circle(ctx, cx + pane.trail[i][0] * reach, cy + pane.trail[i][1] * reach, r, t.dotSoft);
      }
    }
    line(ctx, plot.x + 1, dotY, plot.x + plot.w - 1, dotY, t.track);
    line(ctx, dotX, plot.y + 1, dotX, plot.y + plot.h - 1, t.track);
    circle(ctx, dotX, dotY, dotR * 1.9, t.dotSoft);
    circle(ctx, dotX, dotY, dotR, t.dot);
    let next = plot.y + plot.h;
    if (p.hasBar) { drawAxisBar(ctx, t, p, area.x, next, area.w, z); next += barRow; }
    if (p.showValues) text(ctx, p.tags[0] + " " + percent(x) + "   " + p.tags[1] + " " + percent(y), area.x, next, font, t.muted);
  }

  function drawAxisBar(ctx, t, p, left, top, w, z) {
    const readout = percent(z), readoutW = width(ctx, readout, font);
    const gutter = p.barGutter || readoutW;
    const tag = ellipsis(ctx, p.barTag || "", font, gutter);
    const center = left + w / 2, half = Math.max(5, w / 2 - gutter - 8);
    const x0 = center - half, x1 = center + half, y = Math.round(top + barRow / 2) + 0.5;
    line(ctx, x0, y, x1, y, t.barDash, true);
    [x0, center, x1].forEach((x) => line(ctx, x, y - 4, x, y + 4, t.frame));
    const value = center + z * half, fillW = Math.abs(value - center);
    if (fillW > 0) fillRect(ctx, Math.min(center, value), y - 2, fillW, 4, t.bar);
    circle(ctx, value, y, 5, t.bar);
    text(ctx, tag, left, y, font, t.muted, "middle");
    text(ctx, readout, left + w - readoutW, y, font, t.muted, "middle");
  }

  function drawBars(ctx, pane, area) {
    const p = pane.layout, t = pane.theme;
    if (!p.rows || p.rows.length === 0) { text(ctx, "NESSUN ASSE", area.x, area.y, font, t.muted); return; }
    const nameWidth = Math.min(nameW, Math.max(40, area.w * 0.35));
    const trackLeft = area.x + nameWidth + 8;
    const trackWidth = Math.max(20, area.x + area.w - valueW - 6 - trackLeft);
    for (let i = 0; i < p.rows.length; i++) {
      const top = area.y + i * (rowH + ROW_GAP), value = pane.values[i] || 0;
      text(ctx, ellipsis(ctx, p.rows[i], font, nameWidth), area.x, top + rowH / 2, font, t.muted, "middle");
      const y = Math.round(top + rowH / 2) + 0.5, center = trackLeft + trackWidth / 2;
      line(ctx, trackLeft, y, trackLeft + trackWidth, y, t.track);
      line(ctx, center, y - 5, center, y + 5, t.frame);
      line(ctx, trackLeft, y - 3, trackLeft, y + 3, t.frame);
      line(ctx, trackLeft + trackWidth, y - 3, trackLeft + trackWidth, y + 3, t.frame);
      const end = center + value * (trackWidth / 2), fillW = Math.abs(end - center);
      if (fillW >= 1) fillRect(ctx, Math.min(center, end), y - 3, fillW, 6, t.dot);
      fillRect(ctx, end - 1.5, y - 6, 3, 12, t.dot);
      const readout = percent(value);
      text(ctx, readout, area.x + area.w - width(ctx, readout, font), top + rowH / 2, font, t.text, "middle");
    }
  }

  function drawButtons(ctx, pane, area) {
    const p = pane.layout, t = pane.theme, chips = p.chips || [];
    if (chips.length === 0 && !p.hats && !p.wheel) { text(ctx, "NESSUN PULSANTE SCELTO", area.x, area.y, font, t.muted); return; }
    chips.forEach((chip, i) => {
      const x = area.x + chip.x, y = area.y + chip.y, heat = pane.values[i] || 0;
      strokeRect(ctx, x + 0.5, y + 0.5, chip.w - 1, chip.h - 1, t.cell);
      text(ctx, chip.text, x + (chip.w - width(ctx, chip.text, font)) / 2, y + chip.h / 2, font, t.muted, "middle");
      if (heat <= 0.001) return;
      const alpha = ctx.globalAlpha;
      ctx.globalAlpha = alpha * Math.min(1, heat);
      fillRect(ctx, x, y, chip.w, chip.h, t.dot);
      text(ctx, chip.text, x + (chip.w - width(ctx, chip.text, font)) / 2, y + chip.h / 2, font, t.inverted, "middle");
      ctx.globalAlpha = alpha;
    });
    const rowsHeight = p.rowsHeight || 0;
    const top = area.y + rowsHeight + (rowsHeight > 0 ? CELL_GAP : 0);
    let left = area.x;
    for (let i = 0; i < (p.hats || 0); i++) { drawHat(ctx, t, left, top, pane.values[chips.length + i]); left += HAT_SIZE + HAT_GAP; }
    if (p.wheel) {
      const base = chips.length + (p.hats || 0);
      const cx = left + HAT_SIZE / 2, cy = top + HAT_SIZE / 2;
      strokeRect(ctx, cx - 7, cy - 11, 14, 22, t.cell);
      line(ctx, cx, cy - 4, cx, cy + 4, t.cell);
      drawArrow(ctx, t, cx, top + 1, true, pane.values[base] || 0);
      drawArrow(ctx, t, cx, top + HAT_SIZE - 1, false, pane.values[base + 1] || 0);
    }
  }

  function drawHat(ctx, t, left, top, angle) {
    const cx = left + HAT_SIZE / 2, cy = top + HAT_SIZE / 2, ring = HAT_SIZE / 2 - 6;
    circle(ctx, cx, cy, ring, null, t.rule);
    circle(ctx, cx, cy, 3, null, t.cell);
    for (let step = 0; step < 8; step++) {
      const degrees = step * 45, radians = (degrees - 90) * Math.PI / 180;
      const x = cx + Math.cos(radians) * ring, y = cy + Math.sin(radians) * ring;
      const active = angle >= 0 && Math.abs(((angle - degrees + 540) % 360) - 180) < 1;
      if (active) circle(ctx, x, y, 5, t.dot); else circle(ctx, x, y, 3, null, t.cell);
    }
  }

  function drawArrow(ctx, t, x, y, up, heat) {
    const d = up ? 1 : -1;
    ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x - 6, y + 8 * d); ctx.lineTo(x + 6, y + 8 * d); ctx.closePath();
    ctx.strokeStyle = t.cell; ctx.lineWidth = 1; ctx.stroke();
    if (heat <= 0.001) return;
    const alpha = ctx.globalAlpha;
    ctx.globalAlpha = alpha * Math.min(1, heat); ctx.fillStyle = t.dot; ctx.fill(); ctx.globalAlpha = alpha;
  }

  return { applyFont, theme, drawPane };
})();

// =====================================================================================================
// Controlli piccoli (stessi stili del resto dell'interfaccia)
// =====================================================================================================
function axSwitch(checked, onchange, title) {
  const input = h("input", { type: "checkbox", "aria-label": title || "" });
  input.checked = !!checked;
  input.addEventListener("change", () => onchange(input.checked));
  return h("label", { class: "switch", title: title || null }, input, h("i"));
}

function axToggleRow(label, hint, checked, onchange) {
  return h("div", { class: "row" }, h("div", { class: "row-text" }, h("b", {}, label), hint ? h("small", {}, hint) : null), axSwitch(checked, onchange, label));
}

function axSelect(options, value, onchange, label) {
  const select = h("select", { "aria-label": label || "" }, ...options.map(([v, text]) => h("option", { value: String(v) }, text)));
  select.value = String(value);
  if (select.selectedIndex < 0 && options.length) select.selectedIndex = 0;
  select.addEventListener("change", () => onchange(select.value));
  return select;
}

function axField(label, control, extra) {
  return h("label", { class: "field ax-field" + (extra ? " " + extra : "") }, h("span", { class: "field-label" }, label), control);
}

function axText(value, placeholder, onchange, maxlength = 40) {
  const input = h("input", { type: "text", maxlength, placeholder: placeholder || "", spellcheck: "false" });
  input.value = value || "";
  input.addEventListener("change", () => onchange(input.value.trim()));
  input.addEventListener("keydown", (e) => { if (e.key === "Enter") input.blur(); });
  return input;
}

function axColor(value, onchange, label) {
  const input = h("input", { type: "color", class: "ax-color", "aria-label": label || "" });
  input.value = /^#[0-9a-f]{6}$/i.test(value || "") ? value : "#2aa198";
  input.addEventListener("change", () => onchange(input.value.toUpperCase()));
  return input;
}

// Un cursore come quelli delle altre pagine: si scrive quando lo lasci.
function axSlider(def, value, onchange) {
  const out = h("span", { class: "slider-value" });
  const input = h("input", { type: "range", min: def.min, max: def.max, step: def.step || 1, "aria-label": def.label });
  const host = h("div", { class: "slider ax-slider" }, h("div", { class: "slider-head" }, h("b", {}, def.label), out), input);
  const show = () => {
    const x = Number(input.value);
    out.textContent = def.fmt ? def.fmt(x) : String(x);
    input.style.setProperty("--p", ((x - def.min) * 100) / (def.max - def.min) + "%");
  };
  input.addEventListener("input", show);
  input.addEventListener("change", () => onchange(def.fromUi ? def.fromUi(Number(input.value)) : Number(input.value)));
  host.update = (v) => {
    if (document.activeElement === input) return;
    input.value = def.toUi ? def.toUi(v) : v;
    show();
  };
  host.update(value);
  return host;
}

// =====================================================================================================
// La pagina
// =====================================================================================================
const AXIS_LOOK = [
  { key: "Opacity", label: "Opacità dell'overlay", min: 20, max: 100, step: 5, toUi: (v) => Math.round(v * 100), fromUi: (x) => x / 100, fmt: (x) => x + "%" },
  { key: "DotRadius", label: "Puntino", min: 3, max: 40, fmt: (x) => x + " px" },
  { key: "TextSize", label: "Scritte", min: 8, max: 24, fmt: (x) => x + " px" },
  { key: "Smoothing", label: "Morbidezza del movimento", min: 0, max: 100, step: 5, toUi: (v) => Math.round(v * 100), fromUi: (x) => x / 100, fmt: (x) => (x === 0 ? "nessuna" : x + "%") },
  { key: "MouseRange", label: "Corsa del mouse", min: 50, max: 2000, step: 50, fmt: (x) => x + " px" },
  { key: "Gap", label: "Spazio tra i moduli", min: 0, max: 40, fmt: (x) => x + " px" },
];

function axisDevice(id) {
  const s = AX.state;
  return (s && s.devices.find((d) => d.id === id)) || null;
}

function axisKeyName(vk) {
  const names = (AX.state && AX.state.keyNames) || {};
  return names[String(vk)] || "Tasto " + vk;
}

function axisBuildStatic() {
  const look = $("#ax-look");
  if (!look || look.dataset.built) return;
  look.dataset.built = "1";
  const set = (k, v) => post({ a: "axisSet", k, v });
  AX.lookSliders = AXIS_LOOK.map((def) => { const el = axSlider(def, 0, (v) => set(def.key, v)); el.dataset.key = def.key; return el; });
  AX.lookColors = [
    ["BackgroundColor", "Sfondo"], ["AccentColor", "Cornici e titoli"],
  ].map(([key, label]) => { const el = axColor("#000000", (v) => set(key, v), label); el.dataset.key = key; return axField(label, el, "inline"); });
  AX.lookSwitches = [
    ["ShowTitles", "Titoli", "Il nome sopra ogni modulo."],
    ["ShowFrames", "Cornici", "Il bordo con gli angoli marcati."],
    ["ShowGrid", "Griglia", "La croce tratteggiata nel riquadro degli assi."],
    ["BoldText", "Scritte in grassetto", ""],
  ].map(([key, label, hint]) => {
    const row = axToggleRow(label, hint, false, (v) => set(key, v));
    row.dataset.key = key;
    return row;
  });
  const columns = axSelect([[0, "Automatiche"], ...Array.from({ length: 10 }, (_, i) => [i + 1, String(i + 1)])], 0, (v) => set("Columns", Number(v)), "Colonne");
  columns.dataset.key = "Columns";
  AX.lookColumns = columns;
  look.replaceChildren(
    h("div", { class: "ax-colors" }, ...AX.lookColors),
    ...AX.lookSliders,
    axField("Moduli per riga (stessa finestra)", columns),
    ...AX.lookSwitches,
  );
}

function axisRenderStatic(v) {
  const s = v.settings;
  for (const el of AX.lookSliders) el.update(s[el.dataset.key]);
  for (const field of AX.lookColors) {
    const input = field.querySelector("input");
    if (document.activeElement !== input) input.value = String(s[input.dataset.key] || "#000000").toLowerCase();
  }
  for (const row of AX.lookSwitches) row.querySelector("input").checked = !!s[row.dataset.key];
  if (document.activeElement !== AX.lookColumns) AX.lookColumns.value = String(s.Columns || 0);

  // Overlay
  const st = v.status;
  const panes = (s.Profiles[s.ActiveProfile] || { Panes: [] }).Panes;
  const usable = panes.filter((p) => p.DeviceId).length;
  $("#led-axis").classList.toggle("on", !!st.open);
  setChip("#axis-chip", !st.open ? ["", "Overlay chiuso"] : st.obsOnly ? ["ok", "Solo OBS"] : !st.visible ? ["warn", "Nascosto"] : ["ok", "Overlay aperto"]);
  setChip("#axis-server-chip", v.server.running ? ["ok", "OBS · porta " + v.server.port] : s.ServerEnabled ? ["bad", "OBS · errore"] : ["", "OBS spento"]);
  const callout = $("#ax-state");
  callout.className = "callout " + (st.open ? "ok" : "");
  callout.textContent = !st.open
    ? (usable ? `Chiuso. ${usable} ${usable === 1 ? "modulo pronto" : "moduli pronti"} nel profilo «${(s.Profiles[s.ActiveProfile] || {}).Name || ""}».` : "Chiuso. Aggiungi un modulo qui sotto e scegli la periferica.")
    : st.obsOnly
      ? `Solo per OBS: ${st.windows} ${st.windows === 1 ? "finestra" : "finestre"}, niente sullo schermo.`
      : `${st.windows} ${st.windows === 1 ? "finestra" : "finestre"} sopra il gioco${st.visible ? "" : " (nascoste)"}${st.locked ? ", bloccate" : ""}${st.fromGame ? ", aperte perché è partito il gioco" : ""}.`;
  $("#ax-open").textContent = st.open && !st.obsOnly ? "Riapri" : "Apri sul gioco";
  $("#ax-obs").disabled = !v.server.running;
  $("#ax-close").disabled = !st.open;
  $("#ax-hide").disabled = !st.open || st.obsOnly;
  $("#ax-hide").textContent = st.open && !st.visible ? "Mostra" : "Nascondi";
  $("#ax-lock input").checked = !!s.Locked;
  $("#ax-auto input").checked = !!s.AutoOverlayWithGame;

  // Profili
  const select = $("#ax-profile");
  const items = s.Profiles.map((p, i) => [i, p.Name || "PROFILO " + (i + 1)]);
  const key = JSON.stringify(items);
  if (select.dataset.items !== key) {
    select.replaceChildren(...items.map(([value, label]) => h("option", { value }, label)));
    select.dataset.items = key;
  }
  if (document.activeElement !== select) select.value = String(s.ActiveProfile);
  const profile = s.Profiles[s.ActiveProfile] || { Name: "", Triggers: [] };
  const name = $("#ax-profile-name"), triggers = $("#ax-triggers input");
  if (document.activeElement !== name) name.value = profile.Name || "";
  if (document.activeElement !== triggers) triggers.value = (profile.Triggers || []).join(", ");
  $("#ax-profile-delete").disabled = s.Profiles.length <= 1;

  // OBS
  const server = $("#ax-server-state");
  server.className = "callout " + (v.server.running ? "ok" : v.server.error ? "bad" : "");
  server.textContent = v.server.running
    ? `In ascolto su ${v.server.index} (solo da questo computer).`
    : v.server.error ? "Non parte: " + v.server.error : "Spento: le pagine per OBS non rispondono.";
  $("#ax-server input").checked = !!s.ServerEnabled;
  const port = $("#ax-port input");
  if (document.activeElement !== port) port.value = String(s.ServerPort);
  const urls = $("#ax-urls");
  urls.replaceChildren(...(v.server.urls.length ? v.server.urls.map((u) => h("div", { class: "ax-url" },
    h("span", { class: "ax-url-name" }, u.name),
    h("code", { class: "selectable" }, u.url),
    h("button", { class: "btn small", type: "button", onclick: () => axisCopy(u.url) }, "Copia"))) : [h("div", { class: "key-empty" }, "Nessuna finestra: aggiungi un modulo con una periferica.")]));
  $("#ax-index").textContent = v.server.index;
  $("#ax-import").disabled = !v.axisVisualizerFound;
  $("#ax-import-note").textContent = v.axisVisualizerFound
    ? "Trovate le impostazioni di Axis Visualizer: profili, moduli, nomi, colori e posizioni si copiano in Iris (la porta per OBS resta quella di Iris)."
    : "Impostazioni di Axis Visualizer non trovate su questo computer.";
  const message = $("#ax-message");
  message.textContent = v.message || "";
}

function axisCopy(text) {
  const done = () => toast("Indirizzo copiato: incollalo in una sorgente Browser di OBS.");
  if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).then(done, () => toast(text, true));
  else toast(text, true);
}

// --- I moduli ----------------------------------------------------------------------------------------------
function axisPaneEditor(p, index, count) {
  const set = (k, v) => post({ a: "axisPane", index, k, v });
  const device = axisDevice(p.DeviceId);
  const devices = (AX.state.devices || []).map((d) => [d.id, d.name + (d.connected ? "" : " · non collegata")]);
  const kind = AXIS_KIND_LABEL[p.Kind] ? p.Kind : "Assi";
  const isKeyboard = p.DeviceId === "keyboard";
  const axisNames = device ? device.axisNames : [];
  const buttonNames = device ? device.buttonNames : [];
  const capturing = AX.state.capture === index;

  const head = h("div", { class: "ax-mod-head" },
    h("span", { class: "ax-idx" }, String(index + 1).padStart(2, "0")),
    h("div", { class: "seg ax-kind" }, ...Object.keys(AXIS_KIND_LABEL).map((k) =>
      h("button", { type: "button", class: k === kind ? "on" : null, onclick: () => k !== kind && set("Kind", k) }, AXIS_KIND_LABEL[k]))),
    h("span", { class: "ax-mod-dev" + (device && !device.connected ? " missing" : "") }, !p.DeviceId ? "SPENTO · nessuna periferica" : device ? device.name + (device.connected ? "" : " · non collegata") : p.DeviceId),
    h("div", { class: "ax-mod-tools" },
      h("button", { class: "btn small icon", type: "button", title: "Su", disabled: index === 0, onclick: () => post({ a: "axisPaneMove", index, dir: -1 }) }, "↑"),
      h("button", { class: "btn small icon", type: "button", title: "Giù", disabled: index === count - 1, onclick: () => post({ a: "axisPaneMove", index, dir: 1 }) }, "↓"),
      h("button", { class: "btn small icon danger", type: "button", title: "Togli il modulo", onclick: async () => {
        if (await ask("Togli il modulo", `Togliere il modulo ${index + 1} (${p.Label || AXIS_KIND_LABEL[kind]})?`, [["Annulla", null], ["Togli", true, "danger"]])) post({ a: "axisPaneRemove", index });
      } }, "✕")));

  const form = [];
  form.push(h("div", { class: "ax-line" },
    axField("Periferica", axSelect([["", "— nessuna (modulo spento) —"], ...devices, ...(p.DeviceId && !devices.some(([id]) => id === p.DeviceId) ? [[p.DeviceId, p.DeviceId]] : [])], p.DeviceId, (v) => set("DeviceId", v), "Periferica"), "grow"),
    axField("Nome", axText(p.Label, AXIS_KIND_LABEL[kind].toUpperCase(), (v) => set("Label", v), 24), "grow")));
  form.push(h("div", { class: "ax-line" },
    axField("Titolo", (() => {
      const seg = h("div", { class: "seg" }, ...[["Sinistra", "Sx"], ["Centro", "Centro"], ["Destra", "Dx"]].map(([v, label]) =>
        h("button", { type: "button", class: p.Align === v ? "on" : null, onclick: () => set("Align", v) }, label)));
      return seg;
    })()),
    axField("Finestra", axSelect([[0, "Sua"], ...Array.from({ length: 9 }, (_, i) => [i + 1, "Gruppo " + (i + 1)])], p.Group || 0, (v) => set("Group", Number(v)), "Finestra")),
    h("div", { class: "field ax-field" }, h("span", { class: "field-label" }, "Fisso"), axSwitch(p.Locked, (v) => set("Locked", v), "Non si sposta col tasto centrale"))));
  form.push(axSlider({ label: "Larghezza", min: 80, max: 600, step: 10, fmt: (x) => x + " px" }, p.Size || 200, (v) => set("Size", v)));

  if (kind === "Assi") {
    const axisOptions = [[-1, "nessuno"], ...axisNames.map((n, i) => [i, n])];
    const axisRow = (label, key, invKey) => h("div", { class: "ax-axis" },
      axField(label, axSelect(axisOptions, p[key], (v) => set(key, Number(v)), label), "grow"),
      h("label", { class: "ax-invert", title: "Inverti" }, (() => { const c = h("input", { type: "checkbox" }); c.checked = !!p[invKey]; c.addEventListener("change", () => set(invKey, c.checked)); return c; })(), "inverti"));
    form.push(h("div", { class: "ax-line three" }, axisRow("Orizzontale (X)", "AxisX", "InvertX"), axisRow("Verticale (Y)", "AxisY", "InvertY"), axisRow("Barra (Z)", "AxisZ", "InvertZ")));
    if (isKeyboard) {
      const k = AX.state.settings.Keyboard || {};
      form.push(h("p", { class: "note" }, `Tastiera: ${axisKeyName(k.Left)}/${axisKeyName(k.Right)} orizzontale, ${axisKeyName(k.Up)}/${axisKeyName(k.Down)} verticale, ${axisKeyName(k.BarPlus)}/${axisKeyName(k.BarMinus)} la barra.`));
    }
    form.push(h("div", { class: "ax-line" },
      h("div", { class: "grow" }, axSlider({ label: "Scia", min: 0, max: 200, step: 5, toUi: (v) => Math.round(v * 100), fromUi: (x) => x / 100, fmt: (x) => (x === 0 ? "nessuna" : dec(x / 100, 2) + " s") }, p.TrailSeconds, (v) => set("TrailSeconds", v))),
      axField("Puntino", axColor(p.DotColor, (v) => set("DotColor", v), "Colore del puntino"), "inline"),
      axField("Barra", axColor(p.BarColor, (v) => set("BarColor", v), "Colore della barra"), "inline"),
      h("div", { class: "field ax-field" }, h("span", { class: "field-label" }, "Valori"), axSwitch(p.ShowValues, (v) => set("ShowValues", v), "Mostra le percentuali"))));
  } else if (kind === "Barre") {
    const chosen = new Set(p.BarAxes || []);
    form.push(h("div", { class: "field ax-field" }, h("span", { class: "field-label" }, chosen.size ? `Assi (${chosen.size})` : "Assi · tutti"),
      h("div", { class: "ax-chips" }, ...(axisNames.length ? axisNames.map((n, i) =>
        h("button", { type: "button", class: "ax-pick" + (chosen.has(i) ? " on" : ""), onclick: () => post({ a: "axisToggleList", index, which: "axis", item: i }) }, (p.AxisLabels || {})[i] || n))
        : [h("span", { class: "key-empty" }, p.DeviceId ? "La periferica non ha assi (o non è collegata)." : "Scegli una periferica.")]))));
    form.push(h("div", { class: "ax-line" }, axField("Colore", axColor(p.DotColor, (v) => set("DotColor", v), "Colore delle barre"), "inline")));
  } else {
    const chosen = p.Buttons || [];
    let picks;
    if (isKeyboard) {
      picks = chosen.length ? chosen.map((vk) => h("button", { type: "button", class: "ax-pick on", title: "Togli", onclick: () => post({ a: "axisToggleList", index, which: "button", item: vk }) }, ((p.ButtonLabels || {})[vk] || axisKeyName(vk)) + "  ✕"))
        : [h("span", { class: "key-empty" }, "Nessun tasto: aggiungili premendoli.")];
    } else {
      const selected = new Set(chosen);
      picks = buttonNames.length ? buttonNames.map((n, i) =>
        h("button", { type: "button", class: "ax-pick" + (selected.has(i) ? " on" : ""), onclick: () => post({ a: "axisToggleList", index, which: "button", item: i }) }, (p.ButtonLabels || {})[i] || n))
        : [h("span", { class: "key-empty" }, p.DeviceId ? "La periferica non ha pulsanti (o non è collegata)." : "Scegli una periferica.")];
    }
    form.push(h("div", { class: "field ax-field" },
      h("span", { class: "field-label" }, isKeyboard ? `Tasti (${chosen.length})` : chosen.length ? `Pulsanti (${chosen.length})` : "Pulsanti · tutti"),
      h("div", { class: "ax-chips" }, ...picks),
      h("div", { class: "btn-row" },
        h("button", { class: "btn small" + (capturing ? " busy" : ""), type: "button", disabled: !p.DeviceId, onclick: () => post({ a: "axisCapture", index }) }, capturing ? "Annulla" : "+ Aggiungi premendo"),
        capturing ? h("span", { class: "note inline warn" }, AX.state.message || "Premi…") : null)));
    form.push(h("div", { class: "ax-line" },
      axField("Colonne", axSelect([[0, "Auto"], ...Array.from({ length: 16 }, (_, i) => [i + 1, String(i + 1)])], p.ButtonColumns || 0, (v) => set("ButtonColumns", Number(v)), "Colonne")),
      axField("Accesi", axColor(p.DotColor, (v) => set("DotColor", v), "Colore dei pulsanti premuti"), "inline"),
      device && device.hats > 0 ? h("div", { class: "field ax-field" }, h("span", { class: "field-label" }, "Cappelli"), axSwitch(p.ShowHat, (v) => set("ShowHat", v), "Mostra i cappelli")) : null));
  }

  // Nomi: quelli della periferica, o i tuoi.
  const labelItems = kind === "Assi" ? [p.AxisX, p.AxisY, p.AxisZ].filter((i) => i >= 0 && i < axisNames.length).map((i) => ["axis", i, axisNames[i]])
    : kind === "Barre" ? ((p.BarAxes || []).length ? p.BarAxes : axisNames.map((_, i) => i)).filter((i) => i < axisNames.length).map((i) => ["axis", i, axisNames[i]])
      : isKeyboard ? (p.Buttons || []).map((vk) => ["button", vk, axisKeyName(vk)])
        : ((p.Buttons || []).length ? p.Buttons : buttonNames.map((_, i) => i)).filter((i) => i < buttonNames.length).map((i) => ["button", i, buttonNames[i]]);
  if (labelItems.length) {
    const labels = (which) => (which === "axis" ? p.AxisLabels : p.ButtonLabels) || {};
    form.push(h("details", { class: "ax-names", open: AX.openNames && AX.openNames.has(index) ? true : null,
      ontoggle: (e) => { AX.openNames = AX.openNames || new Set(); if (e.target.open) AX.openNames.add(index); else AX.openNames.delete(index); } },
      h("summary", {}, "Nomi a schermo"),
      h("div", { class: "ax-names-grid" }, ...labelItems.slice(0, 48).map(([which, key, def]) =>
        h("label", {}, h("span", {}, def), axText(labels(which)[key], def, (text) => post({ a: "axisPaneLabel", index, which, key, text }), 16))))));
  }

  const canvas = h("canvas", { class: "ax-canvas" });
  const preview = h("div", { class: "ax-mod-preview" + (p.DeviceId ? "" : " off") },
    canvas, h("div", { class: "ax-preview-empty" }, p.DeviceId ? "" : "Nessuna periferica: il modulo non compare."));

  return h("div", { class: "ax-mod" + (p.DeviceId ? "" : " off") },
    head,
    h("p", { class: "note ax-kind-hint" }, AXIS_KIND_HINT[kind]),
    h("div", { class: "ax-mod-body" }, h("div", { class: "ax-mod-form" }, ...form), preview));
}

function axisRenderModules(force) {
  const v = AX.state, host = $("#ax-modules");
  if (!v || !host) return;
  const s = v.settings, profile = s.Profiles[s.ActiveProfile] || { Panes: [] };
  const key = JSON.stringify([s.ActiveProfile, profile.Panes, v.devices, v.keyNames, v.capture, v.capture >= 0 ? v.message : "", s.Keyboard]);
  if (!force && key === AX.listKey) return;
  // Mentre scrivi in una casella dell'elenco non si ridisegna: lo fa quando esci.
  const focus = document.activeElement;
  if (!force && focus && host.contains(focus) && (focus.tagName === "INPUT" || focus.tagName === "SELECT")) { AX.pending = true; return; }
  AX.pending = false;
  AX.listKey = key;
  const scroll = $("#main").scrollTop;
  host.replaceChildren(...(profile.Panes.length ? profile.Panes.map((p, i) => axisPaneEditor(p, i, profile.Panes.length))
    : [h("div", { class: "callout" }, "Nessun modulo in questo profilo: aggiungine uno con i pulsanti qui sopra (Assi per la cloche, Barre per la manetta, Pulsanti per i tasti).")]));
  $("#main").scrollTop = scroll;
  $$(".ax-add button").forEach((b) => (b.disabled = profile.Panes.length >= 10));
  axisAttachPreviews();
}

// --- Anteprima dal vivo --------------------------------------------------------------------------------------
function axisAttachPreviews() {
  const canvases = $$("#ax-modules .ax-canvas");
  AxisPaint.applyFont(AX.font, AX.bold);
  const ratio = window.devicePixelRatio || 1;
  AX.previews = canvases.map((canvas, i) => {
    const layout = AX.layouts[i];
    if (!layout) { canvas.width = canvas.height = 0; canvas.style.display = "none"; return null; }
    canvas.style.display = "";
    canvas.width = Math.round(layout.w * ratio);
    canvas.height = Math.round(layout.h * ratio);
    canvas.style.width = layout.w + "px";
    canvas.style.aspectRatio = layout.w + " / " + layout.h;
    const old = AX.previews[i];
    return { layout, ctx: canvas.getContext("2d"), theme: AxisPaint.theme(layout), values: [], trail: old ? old.trail : [], trailClock: 0, scale: ratio };
  });
  axisStartLoop();
}

function axisStartLoop() {
  if (AX.looping) return;
  AX.looping = true;
  AX.lastFrame = 0;
  const frame = (now) => {
    if (S.page !== "axis" || document.hidden) { AX.looping = false; return; }
    const dt = AX.lastFrame ? Math.min(0.25, (now - AX.lastFrame) / 1000) : 1 / 60;
    AX.lastFrame = now;
    AX.previews.forEach((pane, i) => {
      if (!pane) return;
      pane.values = AX.values[i] || pane.values;
      try { AxisPaint.drawPane(pane, dt, AX.opacity); } catch (e) { /* un modulo rotto non ferma gli altri */ }
    });
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
}

const axisHandlers = {
  axis(v) {
    AX.state = v;
    axisBuildStatic();
    axisRenderStatic(v);
    axisRenderModules(false);
  },
  axisPreview(v) {
    AX.layouts = v.layouts || [];
    AX.font = v.font || 11;
    AX.bold = !!v.bold;
    AX.opacity = v.opacity || 1;
    axisAttachPreviews();
  },
  axisValues(v) {
    AX.values = v || [];
    if (!AX.looping && S.page === "axis" && AX.previews.length) axisStartLoop();
  },
};

function axisWire() {
  axisBuildStatic();
  $("#ax-open").addEventListener("click", () => post({ a: "axisOpen", obsOnly: false }));
  $("#ax-obs").addEventListener("click", () => post({ a: "axisOpen", obsOnly: true }));
  $("#ax-close").addEventListener("click", () => post({ a: "axisClose" }));
  $("#ax-hide").addEventListener("click", () => post({ a: "axisToggleVisible" }));
  $("#ax-lock input").addEventListener("change", (e) => post({ a: "axisLock", on: e.target.checked }));
  $("#ax-auto input").addEventListener("change", (e) => post({ a: "axisSet", k: "AutoOverlayWithGame", v: e.target.checked }));
  $("#ax-server input").addEventListener("change", (e) => post({ a: "axisSet", k: "ServerEnabled", v: e.target.checked }));
  $("#ax-port").addEventListener("submit", (e) => {
    e.preventDefault();
    const port = parseInt($("#ax-port input").value, 10);
    if (!(port >= 1024 && port <= 65535)) return toast("Porta tra 1024 e 65535.", false);
    post({ a: "axisSet", k: "ServerPort", v: port });
  });
  $("#ax-profile").addEventListener("change", (e) => post({ a: "axisProfile", op: "select", index: Number(e.target.value) }));
  $("#ax-profile-name").addEventListener("change", (e) => { const name = e.target.value.trim(); if (name) post({ a: "axisProfile", op: "rename", name }); });
  $("#ax-profile-name").addEventListener("keydown", (e) => { if (e.key === "Enter") e.target.blur(); });
  $("#ax-profile-add").addEventListener("click", () => post({ a: "axisProfile", op: "add" }));
  $("#ax-profile-delete").addEventListener("click", async () => {
    const s = AX.state && AX.state.settings;
    const name = s ? (s.Profiles[s.ActiveProfile] || {}).Name : "";
    if (await ask("Elimina il profilo", `Eliminare il profilo «${name}» con i suoi moduli?`, [["Annulla", null], ["Elimina", true, "danger"]])) post({ a: "axisProfile", op: "delete" });
  });
  $("#ax-triggers").addEventListener("submit", (e) => { e.preventDefault(); post({ a: "axisTriggers", text: $("#ax-triggers input").value }); });
  $$(".ax-add button").forEach((b) => b.addEventListener("click", () => post({ a: "axisPaneAdd", kind: b.dataset.kind })));
  $("#ax-refresh").addEventListener("click", () => post({ a: "axisRefresh" }));
  $("#ax-import").addEventListener("click", async () => {
    if (await ask("Importa da Axis Visualizer", "Profili, moduli e aspetto di Iris vengono sostituiti da quelli di Axis Visualizer.", [["Annulla", null], ["Importa", true, "primary"]])) post({ a: "axisImport" });
  });
  // Uscito dalla casella: se nel frattempo l'elenco è cambiato, ora si ridisegna.
  $("#ax-modules").addEventListener("focusout", () => setTimeout(() => { if (AX.pending) axisRenderModules(true); }, 0));
}
