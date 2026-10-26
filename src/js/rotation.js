// rotation.js — the geometry of turning a rendered page by 90° steps.
//
// Rotation is a *view* transform, not a re-render: the bitmap already exists
// (the engine applies the page's own /Rotate before we ever see it), so turning
// the view is one CSS transform on the layer that holds the canvas, text and
// links together. Those layers are positioned in unrotated page space, so
// rotating their common parent keeps every span, anchor and highlight on top of
// the glyph it belongs to.
//
// The page element itself stays unrotated — only its children turn — because the
// selection overlay is painted in screen coordinates relative to it, and a
// multiple of 90° maps an axis-aligned box to another axis-aligned box. Turning
// the outer box would put that paint in rotated coordinates and every bar would
// land in the wrong place.

/** Fold any angle onto 0/90/180/270. */
export function normaliseRotation(deg) {
  const n = Number(deg);
  if (!Number.isFinite(n)) return 0;
  return ((Math.round(n / 90) * 90) % 360 + 360) % 360;
}

/**
 * Boxes for one page at `scale`, turned by `rotation` degrees clockwise.
 *
 * @param {{width:number, height:number}} size page size in points (already
 *   reflecting the page's own /Rotate, which the engine bakes into the bitmap)
 * @returns {{rotation:number, width:number, height:number,
 *            innerWidth:number, innerHeight:number, transform:string}}
 *   `width`/`height` are the outer (on-screen) CSS pixel box; `innerWidth`/
 *   `innerHeight` are the unrotated box the canvas and its layers keep.
 */
export function turnedPageBox(size, scale, rotation) {
  const rot = normaliseRotation(rotation);
  const w = Math.max(0, (size?.width || 612) * scale);
  const h = Math.max(0, (size?.height || 792) * scale);
  const swap = rot === 90 || rot === 270;
  let transform = 'none';
  if (rot === 90) transform = `translate(${h}px, 0) rotate(90deg)`;
  else if (rot === 180) transform = `translate(${w}px, ${h}px) rotate(180deg)`;
  else if (rot === 270) transform = `translate(0px, ${w}px) rotate(270deg)`;
  return {
    rotation: rot,
    width: swap ? h : w,
    height: swap ? w : h,
    innerWidth: w,
    innerHeight: h,
    transform,
  };
}

/** Next rotation for one step clockwise (dir = 1) or counter-clockwise (-1). */
export function stepRotation(rotation, dir) {
  return normaliseRotation(normaliseRotation(rotation) + (dir < 0 ? -90 : 90));
}
