import { Component, OnInit, AfterViewChecked, ViewChild, ElementRef } from '@angular/core';
import { Router } from '@angular/router';
import { FormsModule } from '@angular/forms';
import { CommonModule } from '@angular/common';
import { ClaudeService, ChatMessage, TurnEvent, ToolCall } from '../services/claude.service';
import { ApiService } from '../services/api.service';
import { ManifoldVisualizerComponent } from '../visualizer/manifold-visualizer.component';

interface DisplayMessage {
  role: 'user' | 'assistant' | 'tool';
  text: string;
  toolName?: string;
  toolOk?: boolean;
  pending?: boolean;
}

@Component({
  selector: 'app-setup',
  standalone: true,
  imports: [CommonModule, FormsModule, ManifoldVisualizerComponent],
  styles: [`
    :host {
      display: flex;
      flex-direction: column;
      height: 100dvh;
      height: 100vh;
      overflow: hidden;
      background: var(--bg);
    }

    /* ── Header ──────────────────────────────────────────────── */
    .header {
      display: flex;
      align-items: center;
      gap: 12px;
      padding: 14px 16px 10px;
      flex-shrink: 0;
      border-bottom: 1px solid var(--border);
    }

    .back-btn {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 50%;
      width: 40px; height: 40px;
      display: flex; align-items: center; justify-content: center;
      color: var(--text);
      font-size: 18px;
      flex-shrink: 0;
    }
    .back-btn:active { opacity: 0.6; }

    .header h1 {
      font-size: 18px;
      font-weight: 700;
      flex: 1;
    }

    .reset-btn {
      background: none;
      border: none;
      color: var(--muted);
      font-size: 13px;
      padding: 6px 8px;
      border-radius: 8px;
      flex-shrink: 0;
    }
    .reset-btn:active { opacity: 0.6; }

    .confirm-reset {
      display: flex;
      align-items: center;
      gap: 8px;
      font-size: 13px;
      color: var(--muted);
      flex-shrink: 0;
    }
    .confirm-reset span { white-space: nowrap; }
    .confirm-yes {
      border: none;
      border-radius: 6px;
      padding: 4px 10px;
      font-size: 12px;
      font-weight: 600;
      background: var(--danger);
      color: #fff;
    }
    .confirm-no {
      border: none;
      border-radius: 6px;
      padding: 4px 10px;
      font-size: 12px;
      font-weight: 600;
      background: var(--surface);
      color: var(--text);
      border: 1px solid var(--border);
    }

    /* ── Visualizer strip ────────────────────────────────────── */
    .viz-section {
      flex-shrink: 0;
      padding: 10px 12px 0;
      border-bottom: 1px solid var(--border);
    }

    /* ── Messages ─────────────────────────────────────────────── */
    .messages {
      flex: 1;
      overflow-y: auto;
      padding: 16px;
      display: flex;
      flex-direction: column;
      gap: 10px;
    }

    .msg {
      max-width: 90%;
      padding: 12px 15px;
      border-radius: 16px;
      font-size: 15px;
      line-height: 1.5;
      white-space: pre-wrap;
      word-break: break-word;
    }

    .msg.user {
      align-self: flex-end;
      background: var(--accent);
      color: #111;
      border-bottom-right-radius: 4px;
    }

    .msg.assistant {
      align-self: flex-start;
      background: var(--surface);
      color: var(--text);
      border-bottom-left-radius: 4px;
    }

    .msg.pending {
      opacity: 0.5;
    }

    /* Tool call pill */
    .tool-msg {
      align-self: flex-start;
      display: flex;
      align-items: center;
      gap: 8px;
      padding: 6px 12px;
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 20px;
      font-size: 12px;
      color: var(--muted);
    }
    .tool-msg .tool-icon { font-size: 14px; }
    .tool-msg.ok   .tool-icon::before { content: '✓ '; color: var(--success); }
    .tool-msg.err  .tool-icon::before { content: '✗ '; color: var(--danger); }
    .tool-msg.spin .tool-icon { animation: spin 1s linear infinite; display: inline-block; }
    @keyframes spin { to { transform: rotate(360deg); } }

    /* Empty state */
    .empty {
      flex: 1;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      gap: 12px;
      color: var(--muted);
      text-align: center;
      padding: 24px;
    }
    .empty .wave { font-size: 48px; }
    .empty h2 { color: var(--text); font-size: 20px; }
    .empty p  { font-size: 14px; line-height: 1.6; }

    /* ── Input area ───────────────────────────────────────────── */
    .input-area {
      display: flex;
      align-items: flex-end;
      gap: 10px;
      padding: 12px 16px;
      padding-bottom: max(12px, env(safe-area-inset-bottom));
      border-top: 1px solid var(--border);
      flex-shrink: 0;
      background: var(--bg);
    }

    .input-wrap {
      flex: 1;
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 22px;
      display: flex;
      align-items: flex-end;
      padding: 4px 14px;
    }

    textarea {
      flex: 1;
      background: none;
      border: none;
      outline: none;
      color: var(--text);
      font-family: inherit;
      font-size: 15px;
      line-height: 1.4;
      resize: none;
      max-height: 120px;
      padding: 8px 0;
    }

    textarea::placeholder { color: var(--muted); }

    .send-btn {
      width: 44px; height: 44px;
      border-radius: 50%;
      background: var(--accent);
      color: #111;
      font-size: 18px;
      display: flex;
      align-items: center;
      justify-content: center;
      flex-shrink: 0;
    }
    .send-btn:disabled {
      background: var(--border);
      color: var(--muted);
    }
  `],
  template: `
    <!-- Header -->
    <div class="header">
      <button class="back-btn" (click)="goBack()" aria-label="Back">←</button>
      <h1>Setup Assistant</h1>

      <!-- Normal state: show reset button -->
      <button class="reset-btn"
              *ngIf="!confirmingReset"
              (click)="confirmingReset = true"
              aria-label="Start over">↺ Start over</button>

      <!-- Confirmation state -->
      <div class="confirm-reset" *ngIf="confirmingReset">
        <span>Reset?</span>
        <button class="confirm-yes" (click)="doReset()">Yes</button>
        <button class="confirm-no"  (click)="confirmingReset = false">No</button>
      </div>
    </div>

    <!-- Visualizer strip -->
    <div class="viz-section">
      <app-manifold-visualizer [homeOnRight]="api.deviceInfo?.homeOnRight ?? false"></app-manifold-visualizer>
    </div>

    <!-- Messages -->
    <div class="messages" #scrollEl>

      <!-- Empty / welcome state -->
      <div class="empty" *ngIf="display.length === 0">
        <span class="wave">👋</span>
        <h2>Hi! I'm your setup assistant.</h2>
        <p>I'll help you map your tools and Shelly outlets to the right blast gate positions.
           Just say "let's start" and I'll walk you through it step by step.</p>
      </div>

      <ng-container *ngFor="let m of display">

        <!-- User / assistant speech bubbles -->
        <div *ngIf="m.role !== 'tool'"
             class="msg"
             [class.user]="m.role === 'user'"
             [class.assistant]="m.role === 'assistant'"
             [class.pending]="m.pending">
          {{ m.text }}
        </div>

        <!-- Tool-call pills -->
        <div *ngIf="m.role === 'tool'"
             class="tool-msg"
             [class.ok]="m.toolOk === true"
             [class.err]="m.toolOk === false"
             [class.spin]="m.toolOk === undefined">
          <span class="tool-icon">⚙</span>
          <span>{{ m.text }}</span>
        </div>

      </ng-container>

    </div>

    <!-- Input -->
    <div class="input-area">
      <div class="input-wrap">
        <textarea
          [(ngModel)]="inputText"
          placeholder="Message…"
          rows="1"
          (input)="autoResize($event)"
          (keydown.enter)="onEnter($event)"
          [disabled]="busy"
          #textareaEl>
        </textarea>
      </div>
      <button class="send-btn"
              [disabled]="busy || !inputText.trim()"
              (click)="send()"
              aria-label="Send">
        {{ busy ? '…' : '↑' }}
      </button>
    </div>
  `
})
export class SetupComponent implements OnInit, AfterViewChecked {

  @ViewChild('scrollEl') scrollEl!: ElementRef<HTMLElement>;
  @ViewChild('textareaEl') textareaEl!: ElementRef<HTMLTextAreaElement>;

  display: DisplayMessage[] = [];
  inputText = '';
  busy = false;
  confirmingReset = false;

  /** Full Anthropic message history (kept in memory for this session). */
  private history: ChatMessage[] = [];
  private shouldScroll = false;

  constructor(private claude: ClaudeService, public api: ApiService, private router: Router) {}

  ngOnInit() {}

  ngAfterViewChecked() {
    if (this.shouldScroll) {
      const el = this.scrollEl?.nativeElement;
      if (el) el.scrollTop = el.scrollHeight;
      this.shouldScroll = false;
    }
  }

  // ── Actions ──────────────────────────────────────────────────────────────────

  goBack() { this.router.navigate(['/']); }

  async doReset() {
    this.confirmingReset = false;
    try {
      await this.api.resetSetup();
      await this.api.refreshInfo();  // sync deviceInfo.numStops → visualizer shows placeholder
    } catch { /* device may not respond — optimistic numStops=0 still collapses the viz */ }
    this.display   = [];
    this.history   = [];
    this.inputText = '';
    this.busy      = false;
  }

  onEnter(e: Event) {
    const ke = e as KeyboardEvent;
    if (!ke.shiftKey) {
      ke.preventDefault();
      this.send();
    }
  }

  autoResize(e: Event) {
    const el = e.target as HTMLTextAreaElement;
    el.style.height = 'auto';
    el.style.height = Math.min(el.scrollHeight, 120) + 'px';
  }

  async send() {
    const text = this.inputText.trim();
    if (!text || this.busy) return;

    this.inputText = '';
    if (this.textareaEl) {
      this.textareaEl.nativeElement.style.height = 'auto';
    }

    // Show user message
    this.display.push({ role: 'user', text });
    this.scrollDown();
    this.busy = true;

    // Placeholder for the first assistant text segment.
    this.display.push({ role: 'assistant', text: '…', pending: true });
    // pendingIdx tracks the slot for the CURRENT text segment.
    // When it equals display.length the slot is "lazy" — created on first text event.
    let pendingIdx = this.display.length - 1;
    let segmentText = ''; // text accumulated in the current segment

    try {
      await this.claude.sendMessage(this.history, text, (event: TurnEvent) => {
        switch (event.type) {

          case 'text':
            // Accumulate text into the current segment.
            segmentText += (segmentText ? '\n' : '') + event.text;
            if (pendingIdx < this.display.length) {
              // Update existing bubble (initial "…" or continuation).
              this.display[pendingIdx] = { role: 'assistant', text: segmentText };
            } else {
              // Post-tool text: create a new bubble (lazy slot becomes real).
              this.display.push({ role: 'assistant', text: segmentText });
              // pendingIdx was already set to display.length − 1 by tool_start.
            }
            this.scrollDown();
            break;

          case 'tool_start':
            // Freeze the current text segment (or drop the empty "…" bubble).
            if (!segmentText && pendingIdx < this.display.length &&
                this.display[pendingIdx]?.pending) {
              // No pre-tool text — remove the "…" placeholder.
              this.display.splice(pendingIdx, 1);
            }
            // Append the tool pill at the end — never shift existing bubbles.
            this.display.push(this.toolDisplay(event.tool!, false));
            // Next text event will create a NEW bubble after this tool pill.
            pendingIdx = this.display.length; // lazy slot
            segmentText = '';
            this.scrollDown();
            break;

          case 'tool_done': {
            // Find the in-progress pill for this tool and mark it complete.
            const idx = this.display.findIndex(
              m => m.role === 'tool' && m.toolName === event.tool!.name && m.toolOk === undefined
            );
            if (idx >= 0) this.display[idx] = this.toolDisplay(event.tool!, true);
            break;
          }

          case 'error':
            if (pendingIdx < this.display.length) {
              this.display[pendingIdx] = { role: 'assistant', text: `⚠️ ${event.text}` };
            } else {
              this.display.push({ role: 'assistant', text: `⚠️ ${event.text}` });
            }
            this.scrollDown();
            break;
        }
      });
    } finally {
      // Clean up any trailing "…" placeholder that never received text.
      if (!segmentText && pendingIdx < this.display.length &&
          this.display[pendingIdx]?.pending) {
        this.display.splice(pendingIdx, 1);
      }
      this.busy = false;
      this.scrollDown();
    }
  }

  // ── Helpers ──────────────────────────────────────────────────────────────────

  private toolDisplay(tool: ToolCall, done: boolean): DisplayMessage {
    const label = this.toolLabel(tool.name, tool.input);
    return {
      role:     'tool',
      text:     label,
      toolName: tool.name,
      toolOk:   done ? (tool.error === undefined) : undefined
    };
  }

  private toolLabel(name: string, input: Record<string, unknown>): string {
    switch (name) {
      case 'get_status':         return 'Reading system status';
      case 'home':               return 'Homing actuator';
      case 'move_to_stop':       return `Moving to stop ${input['stop']}`;
      case 'jog':                return `Jogging ${input['mm']} mm`;
      case 'ping_outlet':        return `Pinging ${input['ip']}`;
      case 'configure_outlet':   return `Configuring "${input['name']}" at ${input['ip']}`;
      case 'save_config':        return 'Saving configuration';
      case 'delete_outlet':      return `Removing slot ${input['slot']}`;
      case 'save_stop':          return `Saving stop ${input['index']} position`;
      case 'set_home_side':      return `Home side: ${input['home_on_right'] ? 'right' : 'left'}`;
      case 'set_motor_direction':return `Motor direction: ${input['invert'] ? 'inverted' : 'normal'}`;
      case 'set_num_gates':      return `Gates: ${input['num_gates']}`;
      default:                   return name;
    }
  }

  private scrollDown() {
    this.shouldScroll = true;
  }
}
