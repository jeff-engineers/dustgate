/**
 * How the build canvas is framed and moved: zoom, pan, pinch, and the edge
 * auto-scroll that a drag leans on. None of it knows what is being drawn — the
 * component supplies the ink extent and the scroll element, and gets back a
 * scale and a scroll position.
 *
 * It lives outside the component because it is the one part of that file with a
 * genuinely separate job: every method here would read the same if the canvas
 * drew a wiring diagram or a seating chart.
 */

/** What the viewport needs from whatever is drawing. */
export interface ViewportHost {
  /** The scrolling element the canvas sits in, once the view exists. */
  wrapEl(): HTMLDivElement | undefined;
  /** Bounding box of the drawing itself, in board units, with a little air.
   *  Falls back to the padded content box when nothing is drawn yet. */
  inkExtent(): { w: number; h: number };
  /** False while there is nothing worth fitting to. */
  hasDrawing(): boolean;
  /** Recompute the scroll extent — the viewBox depends on the scale. */
  recomputeExtent(): void;
  /** Run inside Angular's zone. ResizeObserver and native listeners are not
   *  patched by zone.js, so a scale change made from one never repaints. */
  runInZone(fn: () => void): void;
}

/** Spread between the first two touches — the scalar a pinch is measured in. */
const touchDist = (e: TouchEvent): number =>
  Math.hypot(e.touches[0].clientX - e.touches[1].clientX, e.touches[0].clientY - e.touches[1].clientY);

export class CanvasViewport {
  constructor(private readonly host: ViewportHost) {}

  // ── zoom ──────────────────────────────────────────────────────────────────────
  // The board is drawn at a fixed viewBox and SCALED by the width/height attributes,
  // so every existing coordinate stays in board units and getScreenCTM() — which all
  // the drag math already goes through — absorbs the zoom for free.
  zoom = 1;
  // 0.3, not 0.4: a full shop on a phone fits at about 0.36, and a floor above that
  // meant fit-on-open silently clamped and left the far end of the drawing off-screen
  // — the one thing it exists to prevent. It also has to stay below any reachable fit, or
  // pressing (−) at the fitted scale would clamp upward and zoom IN.
  readonly ZOOM_MIN = 0.3;
  readonly ZOOM_MAX = 2.5;
  /** Whether the one-time fit-on-open has run yet. */
  private didFit = false;
  /** Set once the user picks a scale themselves. It stops a later resize from
   *  overriding that choice, while a board still sitting at its fitted scale keeps
   *  re-fitting — so rotating a phone reframes the shop instead of cropping it. */
  private userZoomed = false;
  private ro?: ResizeObserver;
  /** Finger spread and zoom at the start of the current pinch, or null. */
  private pinch: { dist: number; zoom: number } | null = null;

  /** Wire up the observers and the listeners that must be non-passive. Call once
   *  the view exists; the wrap does not before that. */
  attach(): void {
    const wrapEl = this.host.wrapEl();
    if (wrapEl) {
      // runInZone: ResizeObserver callbacks are not patched by zone.js, so a re-fit
      // done straight from here updates `zoom` and never repaints — the board kept
      // its old scale on screen while the model said otherwise.
      this.ro = new ResizeObserver(() => this.host.runInZone(() => this.onWrapResize()));
      this.ro.observe(wrapEl);
    }
    // Bound by hand, not in the template: both must be non-passive to preventDefault.
    setTimeout(() => {
      const wrap = this.host.wrapEl(); if (!wrap) return;
      wrap.addEventListener('touchstart', this.pinchStart, { passive: false });
      wrap.addEventListener('touchmove', this.pinchMove, { passive: false });
      wrap.addEventListener('touchend', this.pinchEnd);
      wrap.addEventListener('touchcancel', this.pinchEnd);
      wrap.addEventListener('wheel', this.wheelH, { passive: false });
    });
  }

  /* A pinned board rail used to live here: pinShift() measured how far to translate
   * the strip of boards above the grid so it stayed against the top of the viewport,
   * and a coalesced scroll listener re-rendered the drawing as you scrolled. Both
   * went on 2026-08-16 with the rail itself — boards stand on the grid now and scroll
   * with everything else, so scrolling no longer changes what is drawn. */

  destroy(): void {
    this.stopGlide();
    this.endEdgeScroll();
    this.ro?.disconnect();
    window.removeEventListener('pointermove', this.panMove);
    window.removeEventListener('pointerup', this.panUp);
    window.removeEventListener('pointercancel', this.panUp);
    const wrap = this.host.wrapEl();
    wrap?.removeEventListener('touchstart', this.pinchStart);
    wrap?.removeEventListener('touchmove', this.pinchMove);
    wrap?.removeEventListener('touchend', this.pinchEnd);
    wrap?.removeEventListener('touchcancel', this.pinchEnd);
    wrap?.removeEventListener('wheel', this.wheelH);
  }

  /** Wide enough that the shop can be scrolled around comfortably rather than
   *  needing to be shrunk to fit. Below this, auto-fit earns its keep; at or above
   *  it, it only gets in the way. */
  private readonly DESKTOP_MIN_W = 900;
  /** On a desktop the canvas sits at 100% and stays there until the user says
   *  otherwise. Auto-fit exists for the phone, where a shop is several screens wide
   *  and you'd otherwise land in a corner of it with no idea what you're looking at.
   *  A desktop has room to scroll, so the same machinery only ever surprises you.
   *
   *  Measured on the WINDOW, not the wrap. The app column is capped (app.component
   *  `max-width`), so the wrap's width saturates a little under that cap however wide
   *  the screen is — testing it would make this threshold a question about the column
   *  rather than about the device, and it would need moving every time the cap does. */
  private desktop(): boolean {
    return window.innerWidth >= this.DESKTOP_MIN_W;
  }

  /** Sole resize path — the wrap resizes whenever the window does, so observing it
   *  covers both, and covers panel/layout changes the window never sees. */
  private onWrapResize(): void {
    this.host.recomputeExtent();
    // Never re-scale a desktop. The re-fit below keys off the DRAWING's size, not the
    // window's, so anything that grew the shop and also nudged the wrap — the plug
    // tray appearing, the guide bar wrapping to a second line — rescaled the whole
    // canvas underneath you. Adding a system and then a tool did exactly that: 89% to
    // 63% in two edits, with nothing about the window having changed.
    if (this.desktop()) return;
    if (!this.didFit) { this.maybeFit(); return; }
    // Still framed as we left it: keep it framed. Touched by hand: leave it alone.
    if (this.userZoomed) return;
    const z = this.fitZoom();
    if (Math.abs(z - this.zoom) > 0.005) { this.setZoom(z); this.scrollToOrigin(); }
  }

  zoomPct(): number { return Math.round(this.zoom * 100); }
  zoomBy(factor: number): void { this.userZoomed = true; this.setZoom(this.zoom * factor); }

  /** The scale at which the whole shop is on screen at once. Capped at 1: on a
   *  desktop the drawing usually fits already, and blowing a three-piece layout up to
   *  250% to "fit" it would be absurd. So this only ever scales DOWN. */
  private fitZoom(): number {
    const wrap = this.host.wrapEl();
    if (!wrap) return 1;
    // Fit what is actually DRAWN, not the padded layout box. The content box carries
    // PAD on every side — around 150 units of nothing on a 650-unit shop — and fitting
    // to that shrank the drawing by a fifth for no reason. On a phone that was the
    // difference between 47% and 56%, which is the difference between reading a
    // tool's name and not. The padding still exists in the scroll extent, so there
    // is room to drag past the edge; it just doesn't get a vote on the scale.
    const ink = this.host.inkExtent();
    if (ink.w <= 0 || ink.h <= 0) return 1;
    // A few px of air, so the outermost glyph isn't welded to the bezel.
    const M = 10;
    const z = Math.min((wrap.clientWidth - M) / ink.w, (wrap.clientHeight - M) / ink.h);
    return Math.max(this.ZOOM_MIN, Math.min(1, z));
  }

  /** True when we're already showing the whole board, within rounding. */
  atFit(): boolean { return Math.abs(this.zoom - this.fitZoom()) < 0.01; }

  /** Toggle rather than a plain "reset to 100%": once the board opens fitted, a
   *  one-way reset would strand you at 100% with no way back to the overview short of
   *  pinching it out by hand. */
  resetZoom(): void {
    const fit = this.fitZoom();
    const toFit = !this.atFit();
    this.userZoomed = !toFit;          // going back to fit re-arms auto-reframing
    this.setZoom(toFit ? fit : 1);
    if (toFit) this.scrollToOrigin();
  }

  /** Fit the whole shop on screen. Runs once when the board first opens — on a phone
   *  the layout is otherwise several screens wide and you land in a corner of it with
   *  no idea what you're looking at. A no-op on a desktop wide enough to hold it.
   *
   *  Fits as soon as there is both a layout and a measured viewport to fit it to —
   *  whichever of the two load paths supplies the missing half. */
  maybeFit(): void {
    if (this.didFit || !this.host.hasDrawing()) return;
    const wrap = this.host.wrapEl();
    if (!wrap || !wrap.clientWidth || !wrap.clientHeight) return;
    // A desktop opens at 100%, full stop. The zoomer's % button still fits the shop
    // on demand — that is a thing you ask for, not a thing that happens to you.
    if (this.desktop()) { this.didFit = true; return; }
    this.fitToViewport();
  }
  private fitToViewport(): void {
    const z = this.fitZoom();
    if (z >= 1) { this.didFit = true; return; }
    this.setZoom(z);
    this.scrollToOrigin();
    this.didFit = true;
    this.userZoomed = false;
  }
  private scrollToOrigin(): void {
    const wrap = this.host.wrapEl(); if (!wrap) return;
    requestAnimationFrame(() => { wrap.scrollLeft = 0; wrap.scrollTop = 0; });
  }

  /** Set the zoom, holding the board point under (cx,cy) still. Defaults to the
   *  centre of the viewport, which is what the +/− buttons want. */
  private setZoom(next: number, cx?: number, cy?: number): void {
    const wrap = this.host.wrapEl(); if (!wrap) return;
    const z = Math.min(this.ZOOM_MAX, Math.max(this.ZOOM_MIN, next));
    if (Math.abs(z - this.zoom) < 0.0005) return;
    const r = wrap.getBoundingClientRect();
    const ax = (cx ?? r.left + r.width / 2) - r.left;
    const ay = (cy ?? r.top + r.height / 2) - r.top;
    // The board point currently under the anchor. Scroll offsets are in scaled px,
    // so dividing by the OLD zoom is what puts this back in board units.
    const bx = (wrap.scrollLeft + ax) / this.zoom;
    const by = (wrap.scrollTop + ay) / this.zoom;
    this.zoom = z;
    this.host.recomputeExtent();
    // Scroll only after the new width/height have been laid out: assigning scrollLeft
    // against the OLD scrollWidth clamps to the old maximum and eats the correction,
    // so zooming out would creep toward the top-left instead of staying put.
    requestAnimationFrame(() => {
      wrap.scrollLeft = bx * z - ax;
      wrap.scrollTop = by * z - ay;
    });
  }

  // ── background pan ────────────────────────────────────────────────────────────
  // Replaces native scrolling, which we gave up when the canvas took touch-action:
  // none. Native scrolling could not coexist with dragging: the only way to stop a
  // touch from scrolling is touch-action, and that isn't honored on the SVG glyphs
  // that need to opt out. Doing it here costs us momentum (re-added below as glide)
  // and buys an unambiguous split on every browser.
  private panning: { x: number; y: number; sl: number; st: number; moved: boolean } | null = null;
  private vx = 0; private vy = 0; private glideRaf = 0;
  /** Pointer travel, in px, before a press becomes a pan rather than a tap. Below it
   *  the gesture stays a tap so tapping empty board still deselects. */
  private readonly PAN_SLOP = 4;

  onCanvasDown(e: PointerEvent): void {
    if (e.pointerType === 'mouse' && e.button !== 0) return;
    this.stopGlide();
    const wrap = this.host.wrapEl(); if (!wrap) return;
    this.panning = { x: e.clientX, y: e.clientY, sl: wrap.scrollLeft, st: wrap.scrollTop, moved: false };
    this.vx = 0; this.vy = 0;
    window.addEventListener('pointermove', this.panMove);
    window.addEventListener('pointerup', this.panUp);
    window.addEventListener('pointercancel', this.panUp);
  }
  private readonly panMove = (e: PointerEvent): void => {
    const p = this.panning, wrap = this.host.wrapEl();
    if (!p || !wrap || this.pinch) return;      // a second finger means pinch, not pan
    const dx = e.clientX - p.x, dy = e.clientY - p.y;
    if (!p.moved && Math.hypot(dx, dy) < this.PAN_SLOP) return;
    p.moved = true;
    const sl = p.sl - dx, st = p.st - dy;
    // Velocity from the FRAME delta, not from the gesture total: the glide should
    // continue at the speed the finger was moving when it lifted, which a running
    // average of the whole drag would badly underestimate on a flick.
    this.vx = sl - wrap.scrollLeft; this.vy = st - wrap.scrollTop;
    wrap.scrollLeft = sl; wrap.scrollTop = st;
  };
  private readonly panUp = (): void => {
    const moved = this.panning?.moved ?? false;
    this.panning = null;
    window.removeEventListener('pointermove', this.panMove);
    window.removeEventListener('pointerup', this.panUp);
    window.removeEventListener('pointercancel', this.panUp);
    if (moved && Math.hypot(this.vx, this.vy) > 2) this.startGlide();
  };
  /** Inertia. Native overflow scrolling gave this away for free; a pan that stops
   *  dead the instant you lift feels broken on a phone, so it's worth the 10 lines. */
  private startGlide(): void {
    const wrap = this.host.wrapEl(); if (!wrap) return;
    const step = (): void => {
      if (Math.abs(this.vx) < 0.4 && Math.abs(this.vy) < 0.4) { this.glideRaf = 0; return; }
      wrap.scrollLeft += this.vx; wrap.scrollTop += this.vy;
      this.vx *= 0.93; this.vy *= 0.93;
      this.glideRaf = requestAnimationFrame(step);
    };
    this.glideRaf = requestAnimationFrame(step);
  }
  private stopGlide(): void {
    if (this.glideRaf) cancelAnimationFrame(this.glideRaf);
    this.glideRaf = 0; this.vx = 0; this.vy = 0;
  }

  // ── edge auto-scroll while dragging ───────────────────────────────────────────
  // The other half of why dragging felt broken: a phone shows about three cells, so
  // the destination is usually off-screen and there was no way to reach it without
  // dropping the piece, scrolling, and picking it up again. Holding the drag against
  // an edge now scrolls the board under it.
  //
  // The stored event is REPLAYED into the drag's own move handler each frame. Every
  // one of them resolves position through getScreenCTM(), so the same clientX/Y maps
  // to a new board point once the board has scrolled — the piece keeps travelling
  // while the finger sits still, with no special-casing per drag type.
  private edge: { move: (e: PointerEvent) => void; ev: PointerEvent; raf: number } | null = null;
  private readonly EDGE_BAND = 52;   // px from the edge where auto-scroll kicks in
  private readonly EDGE_MAX = 16;    // px per frame at the very edge

  beginEdgeScroll(move: (e: PointerEvent) => void, ev: PointerEvent): void {
    this.endEdgeScroll();
    this.edge = { move, ev, raf: 0 };
    const step = (): void => {
      const ed = this.edge, wrap = this.host.wrapEl();
      if (!ed || !wrap) return;
      const r = wrap.getBoundingClientRect();
      const e = ed.ev;
      // Ramps from 0 at the band's inner edge to EDGE_MAX at the boundary, so a drag
      // that drifts near an edge creeps and one held hard against it moves properly.
      const ramp = (over: number): number => Math.min(1, over / this.EDGE_BAND) * this.EDGE_MAX;
      let dx = 0, dy = 0;
      if (e.clientX - r.left < this.EDGE_BAND) dx = -ramp(this.EDGE_BAND - (e.clientX - r.left));
      else if (r.right - e.clientX < this.EDGE_BAND) dx = ramp(this.EDGE_BAND - (r.right - e.clientX));
      if (e.clientY - r.top < this.EDGE_BAND) dy = -ramp(this.EDGE_BAND - (e.clientY - r.top));
      else if (r.bottom - e.clientY < this.EDGE_BAND) dy = ramp(this.EDGE_BAND - (r.bottom - e.clientY));
      if (dx || dy) {
        const sl = wrap.scrollLeft, st = wrap.scrollTop;
        wrap.scrollLeft += dx; wrap.scrollTop += dy;
        // Only replay when the board actually moved — at the extent's end it can't,
        // and replaying there would rerun the drag's placement checks for nothing.
        if (wrap.scrollLeft !== sl || wrap.scrollTop !== st) ed.move(e);
      }
      ed.raf = requestAnimationFrame(step);
    };
    this.edge.raf = requestAnimationFrame(step);
  }
  /** Keep the replayed pointer current. Called from every drag's move handler. */
  trackEdge(ev: PointerEvent): void { if (this.edge) this.edge.ev = ev; }
  endEdgeScroll(): void {
    if (this.edge?.raf) cancelAnimationFrame(this.edge.raf);
    this.edge = null;
  }

  private readonly pinchStart = (e: TouchEvent): void => {
    if (e.touches.length !== 2) return;
    e.preventDefault();
    // A pinch always starts as a one-finger press, so a pan is already under way by
    // the time the second finger lands. Drop it, or the board pans off under the
    // pinch as the midpoint drifts.
    this.panning = null; this.stopGlide();
    this.userZoomed = true;
    this.pinch = { dist: touchDist(e), zoom: this.zoom };
  };
  private readonly pinchMove = (e: TouchEvent): void => {
    if (!this.pinch || e.touches.length !== 2) return;
    e.preventDefault();
    const d = touchDist(e);
    if (d < 1 || this.pinch.dist < 1) return;
    const [a, b] = [e.touches[0], e.touches[1]];
    this.setZoom(this.pinch.zoom * (d / this.pinch.dist),
                 (a.clientX + b.clientX) / 2, (a.clientY + b.clientY) / 2);
  };
  private readonly pinchEnd = (e: TouchEvent): void => {
    if (e.touches.length < 2) this.pinch = null;
  };
  /** Trackpad pinch and Ctrl-wheel arrive as a wheel with ctrlKey — that's the only
   *  signal a pinch gesture gives on desktop. A plain wheel is left alone so the
   *  canvas still scrolls normally. */
  /**
   * The wheel, which on a desktop is the whole navigation story.
   *
   * ctrl (or ⌘) + wheel is the trackpad pinch — every browser reports that gesture
   * as a wheel event with ctrlKey set — so it zooms.
   *
   * Anything else pans, and it has to be done by hand for the same reason the pan
   * gesture is: the canvas took `touch-action: none` to arbitrate drags itself,
   * which gave up native scrolling with it. That left the desktop with press-and-
   * drag as the ONLY way to move around a shop taller than the screen, which is
   * not what a mouse or a trackpad expects. A trackpad sends deltaX itself; a mouse
   * has one wheel and the convention for its other axis is shift.
   */
  private readonly wheelH = (e: WheelEvent): void => {
    if (e.ctrlKey || e.metaKey) {
      e.preventDefault();
      this.userZoomed = true;
      this.setZoom(this.zoom * Math.exp(-e.deltaY / 240), e.clientX, e.clientY);
      return;
    }
    const wrap = this.host.wrapEl(); if (!wrap) return;
    // deltaMode 1 is lines and 2 is pages — Firefox uses lines for a real mouse.
    const unit = e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? wrap.clientHeight : 1;
    const sideways = e.shiftKey && !e.deltaX;
    const dx = (sideways ? e.deltaY : e.deltaX) * unit;
    const dy = (sideways ? 0 : e.deltaY) * unit;
    const canX = wrap.scrollWidth - wrap.clientWidth > 1;
    const canY = wrap.scrollHeight - wrap.clientHeight > 1;
    if (!(dx && canX) && !(dy && canY)) return;    // nowhere to go: let the page have it
    e.preventDefault();
    this.stopGlide();                              // a wheel overrides a pan still coasting
    wrap.scrollLeft += dx;
    wrap.scrollTop += dy;
  };

  /** Bring a band of board rows into view — what adding a system needs, so the thing
   *  you just made is on screen instead of somewhere below the fold. Scrolls the
   *  minimum distance that gets there, and only downward-or-up as needed, so it never
   *  yanks the view when the target is already visible. */
  revealBoard(y0: number, y1: number): void {
    const wrap = this.host.wrapEl(); if (!wrap) return;
    // Deferred, because the caller has just made the drawing taller and this has to
    // run after Angular has rendered that — a scroll set against the OLD scrollHeight
    // clamps to the old maximum and silently does nothing, which is the exact bug
    // this method exists to fix.
    setTimeout(() => {
      const top = y0 * this.zoom, bottom = y1 * this.zoom;
      const view = wrap.clientHeight;
      const margin = 24;
      if (bottom > wrap.scrollTop + view) wrap.scrollTop = Math.max(0, bottom - view + margin);
      if (top < wrap.scrollTop) wrap.scrollTop = Math.max(0, top - margin);
    });
  }
}
