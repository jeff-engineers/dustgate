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
      max-width: 960px;
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
