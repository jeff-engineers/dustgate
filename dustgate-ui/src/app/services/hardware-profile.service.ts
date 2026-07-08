import { Injectable } from '@angular/core';

export type PortSize = '2.5in' | '4in';

const STORAGE_KEY = 'dustgate.portSize';

/**
 * Distance between two adjacent fixed gate openings on the manifold (not the
 * sliding gate's own width, which is narrower). Used only as a starting guess
 * for the visualizer's jog animation before any real stop has been saved —
 * jogging to the actual position always takes precedence once you have one.
 */
const GATE_SPACING_MM: Record<PortSize, number> = {
  '2.5in': 89,
  '4in':   89, // placeholder — no 4" hardware to measure yet; update once available
};

@Injectable({ providedIn: 'root' })
export class HardwareProfileService {

  portSize: PortSize = (localStorage.getItem(STORAGE_KEY) as PortSize | null) ?? '2.5in';

  set(size: PortSize) {
    this.portSize = size;
    localStorage.setItem(STORAGE_KEY, size);
  }

  get expectedGateSpacingMm(): number {
    return GATE_SPACING_MM[this.portSize];
  }
}
