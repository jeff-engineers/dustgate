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
  '2.5in': 82.9, // measured gate-to-gate on the reference build (span 84.9mm at 2 gates, 1mm offset/side)
  '4in':   127,  // Rockler 10" manifold ÷ 2 gates = 5" = 127mm center-to-center (unconfirmed on hardware)
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
