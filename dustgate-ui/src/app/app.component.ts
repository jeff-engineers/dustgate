import { Component } from '@angular/core';
import { RouterOutlet } from '@angular/router';

@Component({
  selector: 'app-root',
  standalone: true,
  imports: [RouterOutlet],
  styles: [`
    :host {
      display: block;
      height: 100%;
      /* 1440, up from 960 on 2026-08-16. This is the ceiling for the BUILD canvas
         and nothing else: every other screen sets a 360–460px cap of its own, so
         they stay phone-shaped and centred however wide this gets. The canvas is
         the one screen whose usefulness scales with the window — a shop grows
         down-and-right and the boards now sit at its top-right corner. */
      max-width: 1440px;
      margin: 0 auto;
      /* On wide viewports the app column no longer reaches the screen edge,
         so give it its own borders to stay visually separated from the bg. */
      border-left: 1px solid var(--border);
      border-right: 1px solid var(--border);
    }
  `],
  template: `<router-outlet />`
})
export class AppComponent {}
