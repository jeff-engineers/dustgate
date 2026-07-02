import { Component, OnInit, AfterViewChecked, ViewChild, ElementRef } from '@angular/core';
import { Router } from '@angular/router';
import { FormsModule } from '@angular/forms';
import { CommonModule } from '@angular/common';
import { ClaudeService, ChatMessage, TurnEvent, ToolCall } from '../services/claude.service';

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
  imports: [CommonModule, FormsModule],
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

  /** Full Anthropic message history (kept in memory for this session). */
  private history: ChatMessage[] = [];
  private shouldScroll = false;

  constructor(private claude: ClaudeService, private router: Router) {}

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

    // Placeholder for assistant response
    const pendingIdx = this.display.length;
    this.display.push({ role: 'assistant', text: '…', pending: true });
    this.scrollDown();

    let assistantText = '';

    try {
      await this.claude.sendMessage(this.history, text, (event: TurnEvent) => {
        switch (event.type) {
          case 'text':
            assistantText += (assistantText ? '\n' : '') + event.text;
            this.display[pendingIdx] = { role: 'assistant', text: assistantText };
            this.scrollDown();
            break;

          case 'tool_start':
            this.display.splice(pendingIdx, 0, this.toolDisplay(event.tool!, false));
            this.scrollDown();
            break;

          case 'tool_done': {
            // Find and update the matching tool pill
            const idx = this.display.findIndex(
              m => m.role === 'tool' && m.toolName === event.tool!.name && m.toolOk === undefined
            );
            if (idx >= 0) {
              this.display[idx] = this.toolDisplay(event.tool!, true);
            }
            break;
          }

          case 'error':
            this.display[pendingIdx] = { role: 'assistant', text: `⚠️ ${event.text}` };
            this.scrollDown();
            break;
        }
      });
    } finally {
      // Remove pending placeholder if no text came back
      if (!assistantText) {
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
      case 'get_status':    return 'Reading system status';
      case 'home':          return 'Homing actuator';
      case 'move_to_stop':  return `Moving to stop ${input['stop']}`;
      case 'jog':           return `Jogging ${input['mm']} mm`;
      case 'ping_outlet':   return `Pinging ${input['ip']}`;
      case 'configure_outlet': return `Configuring "${input['name']}" at ${input['ip']}`;
      case 'save_config':   return 'Saving configuration';
      case 'delete_outlet': return `Removing slot ${input['slot']}`;
      default:              return name;
    }
  }

  private scrollDown() {
    this.shouldScroll = true;
  }
}
