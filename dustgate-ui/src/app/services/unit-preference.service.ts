import { Injectable } from '@angular/core';
import { BehaviorSubject } from 'rxjs';

export type UnitSystem = 'mm' | 'in';

export interface JogStep {
  label: string;   // display label, e.g. "25 mm" or "1\""
  mm: number;      // amount in millimetres sent to firmware
}

const MM_STEPS: JogStep[] = [
  { label: '1 mm',  mm: 1 },
  { label: '5 mm',  mm: 5 },
  { label: '10 mm', mm: 10 },
  { label: '25 mm', mm: 25 },
  { label: '50 mm', mm: 50 },
];

const IN_STEPS: JogStep[] = [
  { label: '1/16"', mm: 1.5875  },
  { label: '1/4"',  mm: 6.35    },
  { label: '1"',    mm: 25.4    },
  { label: '2"',    mm: 50.8    },
];

@Injectable({ providedIn: 'root' })
export class UnitPreferenceService {

  private _unit$ = new BehaviorSubject<UnitSystem>('mm');
  readonly unit$ = this._unit$.asObservable();

  get unit(): UnitSystem { return this._unit$.value; }

  toggle() {
    this._unit$.next(this.unit === 'mm' ? 'in' : 'mm');
  }

  set(u: UnitSystem) {
    this._unit$.next(u);
  }

  get jogSteps(): JogStep[] {
    return this.unit === 'mm' ? MM_STEPS : IN_STEPS;
  }

  /**
   * Format a millimetre value for display in the current unit system.
   * Examples: 25.4 → "25.4 mm" or "1.00\""
   */
  format(mm: number): string {
    if (this.unit === 'mm') {
      return `${mm.toFixed(1)} mm`;
    }
    const inches = mm / 25.4;
    return `${inches.toFixed(2)}"`;
  }

  /**
   * Format just the numeric part, no unit suffix.
   */
  formatValue(mm: number): string {
    if (this.unit === 'mm') return mm.toFixed(1);
    return (mm / 25.4).toFixed(2);
  }

  get unitLabel(): string {
    return this.unit === 'mm' ? 'mm' : '"';
  }
}
