#!/usr/bin/env node
// competitive-parity.mjs — the single source of truth for the "how far behind
// pdf.js / MuPDF.js are we" numbers in README.md.
//
//   node scripts/competitive-parity.mjs          # markdown, paste into README
//   node scripts/competitive-parity.mjs --json   # raw numbers
//
// A score is a judgement, so the rule is: every score is anchored to a *named*
// missing capability (the `gap` field), not to a feeling. When a milestone
// lands, change the score and delete the gap line in the same commit, then paste
// the regenerated table into README.md. Do not hand-edit the table in README.
//
// Weights are relative importance for a document *viewer* (what a host app
// embeds), not for a PDF library's API surface. They sum to 100, so a weighted
// score reads directly as a percentage.

const ENGINES = ['ours', 'pdfjs', 'mupdf'];

const AREAS = [
  {
    id: 'parse', name: 'Parsing, xref, object streams, damaged files', weight: 8,
    scores: { ours: 92, pdfjs: 100, mupdf: 100 },
    gap: 'linearised-file first-page hint tables (parses correctly, just later)',
  },
  {
    id: 'crypt', name: 'Encryption (Standard handler R2–R6)', weight: 5,
    scores: { ours: 92, pdfjs: 85, mupdf: 100 },
    gap: 'public-key (/Adobe.PubSec) encryption; no mid-parse onPassword callback',
  },
  {
    id: 'codecs', name: 'Image codecs (Flate/DCT/CCITT/JBIG2/JPX)', weight: 8,
    scores: { ours: 85, pdfjs: 100, mupdf: 100 },
    gap: 'JPEG 2000 decodes only one encoder; OpenJPEG codestreams render noise',
  },
  {
    id: 'colour', name: 'Colour management (ICC, spot, DeviceN)', weight: 7,
    scores: { ours: 70, pdfjs: 85, mupdf: 100 },
    gap: 'ICC transforms wired into images only; fills still use the calibrated CMYK table (no lcms2)',
  },
  {
    id: 'fonts', name: 'Fonts and encodings (TT, CFF/CID, CMaps, Type3)', weight: 10,
    scores: { ours: 80, pdfjs: 90, mupdf: 95 },
    gap: 'no Type1 PFB, no vertical (-V) writing, no shaped fallback warning',
  },
  {
    id: 'content', name: 'Content stream: paths, clips, blend, transparency', weight: 10,
    scores: { ours: 90, pdfjs: 95, mupdf: 100 },
    gap: 'gs-level /SMask ignored; non-isolated groups composited isolated',
  },
  {
    id: 'shading', name: 'Shadings (types 1–7)', weight: 5,
    scores: { ours: 40, pdfjs: 100, mupdf: 100 },
    gap: 'types 1/6/7 unimplemented; 4/5 render but disagree with MuPDF (87/255)',
  },
  {
    id: 'patterns', name: 'Tiling patterns, images, masks', weight: 5,
    scores: { ours: 90, pdfjs: 95, mupdf: 100 },
    gap: 'rotated checker tile edge quality 7.65/255 (ink already matches)',
  },
  {
    id: 'annots', name: 'Annotations (render, links, markup)', weight: 6,
    scores: { ours: 75, pdfjs: 95, mupdf: 95 },
    gap: 'no note-text panel, no annotation authoring/export, highlights use alpha not multiply',
  },
  {
    id: 'forms', name: 'Forms (AcroForm read, fill, save)', weight: 6,
    scores: { ours: 45, pdfjs: 90, mupdf: 100 },
    gap: 'setField writes /V but does not regenerate a producer\'s /AP; no input overlay; no save',
  },
  {
    id: 'text', name: 'Text extraction and structure', weight: 6,
    scores: { ours: 78, pdfjs: 95, mupdf: 100 },
    gap: 'runs only (no exposed line/block structure), no /ToUnicode fallback policy, no full-document index',
  },
  {
    id: 'viewer', name: 'Viewer UX (toolbar, rail, search, print)', weight: 8,
    scores: { ours: 72, pdfjs: 100, mupdf: 50 },
    gap: 'no outline panel, page rotation, zoom presets, pinch gestures or full keyboard map',
  },
  {
    id: 'a11y', name: 'Accessibility (tagged PDF, screen readers)', weight: 5,
    scores: { ours: 20, pdfjs: 90, mupdf: 35 },
    gap: 'text layer only: no structure tree, no roles/aria, no read-order or alt text',
  },
  {
    id: 'arch', name: 'Architecture (worker, ranged loading, cache)', weight: 6,
    scores: { ours: 60, pdfjs: 100, mupdf: 80 },
    gap: 'the worker landed (M9.1); still whole file resident in wasm memory, and no display list or page cache',
  },
  {
    id: 'eco', name: 'Ecosystem (release, docs, integrations, testing)', weight: 5,
    scores: { ours: 30, pdfjs: 100, mupdf: 90 },
    gap: 'npm 3 versions behind, one maintainer, no docs site, no framework examples, no fuzzing or CI matrix',
  },
];

// What closes the gap. `to` is the score the area reaches when the item lands,
// so the report can price each item in percentage points of the whole.
const ITEMS = [
  {
    title: 'Ranged loading + display list (M9.2/M9.3)',
    areas: { arch: 100 }, effort: '2–3 weeks',
    why: 'whole-file residency, no eviction; the worker that unblocks the work has landed',
  },
  {
    title: 'Shadings 1/6/7 and a verified 4/5',
    areas: { shading: 95 }, effort: '3–5 days',
    why: 'gradient-heavy marketing PDFs currently lose their background',
  },
  {
    title: 'Forms: input overlay, /AP regeneration, save/flatten',
    areas: { forms: 85 }, effort: '1–2 weeks',
    why: 'a filled field under a real producer\'s /AP still looks empty',
  },
  {
    title: 'Accessibility: tagged structure tree, roles, read order',
    areas: { a11y: 65 }, effort: '2–3 weeks',
    why: 'the text layer is selectable but not navigable by a screen reader',
  },
  {
    title: 'JPEG 2000: vendor openjpeg\'s j2k decoder',
    areas: { codecs: 100 }, effort: '3–5 days',
    why: 'the only output that is garbage rather than merely off',
  },
  {
    title: 'ICC for fills via lcms2',
    areas: { colour: 90 }, effort: '3–4 days',
    why: 'print-derived CMYK art is visibly off next to MuPDF',
  },
  {
    title: 'Viewer UX: outline panel, rotate, zoom presets, gestures',
    areas: { viewer: 92 }, effort: '1 week',
    why: 'the chrome is the first thing a reviewer compares against pdf.js',
  },
  {
    title: 'Ecosystem: publish 0.3.0, docs site, framework examples, CI matrix',
    areas: { eco: 65 }, effort: '1 week + ongoing',
    why: 'registry is stale, and there is no fuzzing or disclosure pipeline',
  },
  {
    title: 'Type1 PFB, vertical CMaps, no-shaped-fallback warning',
    areas: { fonts: 92 }, effort: '4–6 days',
    why: 'legacy TeX output and Japanese vertical text fall back to a substitute face',
  },
  {
    title: 'Annotations: note panel, multiply highlights, authoring',
    areas: { annots: 90 }, effort: '1 week',
    why: 'review workflows stop at "markup renders"',
  },
  {
    title: 'Text structure: lines/blocks, /ToUnicode policy, document index',
    areas: { text: 92 }, effort: '3–4 days',
    why: 'every pdf.js consumer expects line/block grouping',
  },
  {
    title: 'Encryption: public-key handler, onPassword retry',
    areas: { crypt: 97 }, effort: '2–3 days',
    why: 'rare, but a hard fail when a host meets a /Adobe.PubSec file',
  },
];

const totalWeight = AREAS.reduce((n, a) => n + a.weight, 0);
if (totalWeight !== 100) throw new Error(`weights sum to ${totalWeight}, expected 100`);

const weighted = (engine, areas = AREAS) =>
  areas.reduce((n, a) => n + (a.scores[engine] * a.weight) / 100, 0);

const score = {};
for (const e of ENGINES) score[e] = weighted(e);

const overall = {
  vsPdfjs: (score.ours / score.pdfjs) * 100,
  vsMupdf: (score.ours / score.mupdf) * 100,
};

// Simulate every item landing: best score per area wins.
const after = AREAS.map((a) => {
  const target = ITEMS.reduce((best, it) => Math.max(best, it.areas[a.id] ?? 0), 0);
  return { ...a, scores: { ...a.scores, ours: Math.max(a.scores.ours, target) } };
});
const projected = weighted('ours', after);
const projectedPct = {
  vsPdfjs: (projected / score.pdfjs) * 100,
  vsMupdf: (projected / score.mupdf) * 100,
};

const items = ITEMS.map((it) => {
  const points = Object.entries(it.areas).reduce((n, [area, to]) => {
    const a = AREAS.find((x) => x.id === area);
    if (!a) throw new Error(`unknown area ${area}`);
    if (to < a.scores.ours) throw new Error(`${it.title}: ${area} would regress`);
    return n + ((to - a.scores.ours) * a.weight) / 100;
  }, 0);
  return { ...it, points };
});

if (process.argv.includes('--json')) {
  console.log(JSON.stringify({ score, overall, projected, projectedPct, areas: AREAS, items }, null, 2));
  process.exit(0);
}

const pct = (n) => `${n.toFixed(0)}%`;
const line = (cells) => `| ${cells.join(' | ')} |`;

console.log('### Capability parity\n');
console.log(line(['Area', 'Weight', 'This engine', 'pdf.js', 'MuPDF.js']));
console.log(line(['---', '---:', '---:', '---:', '---:']));
for (const a of AREAS) {
  console.log(line([a.name, `${a.weight}%`, pct(a.scores.ours), pct(a.scores.pdfjs), pct(a.scores.mupdf)]));
}
console.log(line(['**Weighted total**', '**100%**',
  `**${pct(score.ours)}**`, `**${pct(score.pdfjs)}**`, `**${pct(score.mupdf)}**`]));
console.log(`\nParity: **${pct(overall.vsPdfjs)} of pdf.js**, **${pct(overall.vsMupdf)} of MuPDF.js**`);
console.log(`(weighted score ${score.ours.toFixed(1)} of 100; pdf.js ${score.pdfjs.toFixed(1)}, MuPDF.js ${score.mupdf.toFixed(1)})`);

console.log('\n### What closing them would buy\n');
console.log(line(['Remaining item', '+points', 'Result', 'Effort', 'Why it matters']));
console.log(line(['---', '---:', '---:', '---', '---']));
for (const it of [...items].sort((a, b) => b.points - a.points)) {
  const to = Math.max(...Object.entries(it.areas).map(([id, v]) => v));
  console.log(line([it.title, `+${it.points.toFixed(1)}`, pct(to), it.effort, it.why]));
}
const gained = projected - score.ours;
console.log(line(['**All of the above**', `**+${gained.toFixed(1)}**`,
  `**${pct(projected)}**`, '', `**${pct(projectedPct.vsPdfjs)} of pdf.js, ${pct(projectedPct.vsMupdf)} of MuPDF.js**`]));
