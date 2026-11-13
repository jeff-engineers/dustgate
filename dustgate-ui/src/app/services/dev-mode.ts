/**
 * Developer mode (2026-11-01): a SHOP setting, kept in the layout as `devMode` (jeff: shop-wide, not per phone), so every
 * phone in the shop sees the same thing. It shows what a woodworker never needs to run a shop — servo numbers, raw
 * readings, communication losses, the PWM pulse range and the controls that can leave a gate somewhere it shouldn't be.
 *
 * Read once from the layout and kept here, so every screen that shows or hides something agrees without each one asking
 * the brain; Settings writes it through `set()`.
 */
import { Injectable } from '@angular/core';
import { BehaviorSubject } from 'rxjs';
import { ApiService, Topology } from './api.service';

/** The layout's setting; absent is off. */
export function devModeOf(t: unknown): boolean {
  return !!t && typeof t === 'object' && (t as { devMode?: unknown }).devMode === true;
}

@Injectable({ providedIn: 'root' })
export class DevModeService {
  readonly on$ = new BehaviorSubject<boolean>(false);
  private loaded = false;

  constructor(private api: ApiService) {}

  /** Read it from the layout, once per app load (a reload picks up another phone's change). */
  load(): void {
    if (this.loaded) return;
    this.loaded = true;
    void this.api.getTopology().then((t) => this.on$.next(devModeOf(t))).catch(() => { this.loaded = false; });
  }

  /** Save it into the layout as read NOW, so whatever else changed since is kept. Absent when off. */
  async set(on: boolean): Promise<void> {
    const doc = JSON.parse(JSON.stringify(await this.api.getTopology())) as Topology & { devMode?: boolean };
    if (on) doc.devMode = true; else delete doc.devMode;
    await this.api.putTopology(doc as Topology);
    this.on$.next(on);
  }
}
