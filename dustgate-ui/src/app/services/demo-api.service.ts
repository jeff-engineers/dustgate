import { Injectable } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import {
  ApiService,
  DeviceInfo,
  DiscoveredNode,
  DiscoveredOutlet,
  OutletNameResult,
  OutletReleaseResult,
  NodeLinkState,
  OutletConfigCmd,
  PingResult,
  SystemStatus,
} from './api.service';
import { HardwareProfileService } from './hardware-profile.service';
import * as model from '@device-model';
import { validateTopology, type Topology } from '@topology';
import { isShop, systemsOf, validateShop } from '@shop';
import { createTopologyDevice, setCollectorManual, setToolPower, statusView as topoStatus, toolThreshold, type TopologyDevice, type TopologyStatus } from '@topology-device';
import { DEMO_TOPOLOGY } from './demo-topology';

// ── Service ────────────────────────────────────────────────────────────────────

/**
 * Drop-in replacement for ApiService used in demo / Vercel mode.
 *
 * This is a THIN async wrapper over the canonical device model
 * (shared/device-model/device-model.js) — the SAME model that drives the Node
 * dev mock (tools/mock-api.js). All device behaviour lives in the model; this
 * class only owns timing (await delay between the begin/complete motion steps)
 * and maps the model's wire shape to the Angular API types.
 *
 * Provided via DI override in app.config.ts when running outside localhost.
 */
@Injectable()
export class DemoApiService extends ApiService {

  /**
   * Where the demo keeps what you changed, so a reload doesn't wipe it.
   *
   * The demo used to live entirely in memory, which made it useless for trying
   * anything that outlives one page: rename an outlet, reload, and you were back
   * to the seed shop with the rename gone — indistinguishable from a rename that
   * never saved. Worse under `ng serve`, where every source edit triggers a
   * live-reload and silently resets the shop mid-test.
   *
   * sessionStorage, not localStorage: a demo that remembered a shop you built
   * months ago in another tab would be its own kind of confusing. Closing the tab
   * still gives everyone the seeded shop.
   */
  private static readonly SAVE_KEY = 'dustgate_demo_state';

  /** The canonical device instance. Restored from sessionStorage when present. */
  private d: model.Device = model.createDevice();

  /** topology-native device (seeded with DEMO_TOPOLOGY, or with what you saved). */
  private td: TopologyDevice | null = createTopologyDevice(DEMO_TOPOLOGY);

  constructor(http: HttpClient, hardwareProfile: HardwareProfileService) {
    super(http, hardwareProfile);
    // super() triggers init() via the parent ctor; our override runs instead.
  }

  // ── Bootstrap (no HTTP, no WebSocket) ────────────────────────────────────────

  /** The two things a person actually changes: the layout, and the outlets they
   *  have named or paired. Everything else is derived or transient. */
  private persist(): void {
    try {
      sessionStorage.setItem(DemoApiService.SAVE_KEY, JSON.stringify({
        topology: this.td ? this.td.topology : null,
        discovered: (this.d as unknown as { _discovered?: unknown })._discovered ?? null,
      }));
    } catch { /* private browsing, or quota — the demo still works, just forgets */ }
  }

  private restore(): boolean {
    try {
      const raw = sessionStorage.getItem(DemoApiService.SAVE_KEY);
      if (!raw) return false;
      const saved = JSON.parse(raw) as { topology?: unknown; discovered?: unknown };
      if (!saved.topology) return false;
      this.td = createTopologyDevice(saved.topology as Topology);
      if (saved.discovered) {
        (this.d as unknown as { _discovered?: unknown })._discovered = saved.discovered;
      }
      return true;
    } catch {
      // A saved shape this build no longer understands is worse than none.
      try { sessionStorage.removeItem(DemoApiService.SAVE_KEY); } catch { /* ignore */ }
      return false;
    }
  }

  protected override async init(): Promise<void> {
    // Seed the showcase with a pre-configured dust collector so the collector
    // toggle works on a fresh page load without running setup. A start-over
    // (clearCal) still clears it, matching firmware — this is just initial demo
    // state, not a hardcoded-always-true override.
    this.d.dcConfigured = true;
    this.d.dcIp = '192.168.87.50';
    // The seeded shop's plugs are ON the simulated network. Without this the
    // scan invents its own plugs at random IPs and DEMO_TOPOLOGY's — the table
    // saw's, the bandsaw's — belong to no device anyone can reach, so the demo
    // showed every paired plug as not responding and refused every rename.
    // Restored state wins over the seed — but the seed still has to run first, so
    // a restore that only carries a layout still finds the demo network populated.
    model.adoptOutlets(this.d, DEMO_TOPOLOGY);
    const restored = this.restore();
    if (restored) model.adoptOutlets(this.d, this.td!.topology as unknown as Topology);

    this.deviceInfo = {
      apiKey:        'demo',
      numStops:      0,
      version:       '1.0-demo',
      motorInverted: false,
      // Taken from the model rather than typed here: this is the owner suffix the
      // demo's own nameOutlet() stamps on a plug, and the UI shows it beside the
      // name field as what WILL be written. Two literals would eventually disagree
      // and the preview would quietly start lying.
      owner:         model.infoView(this.d, 'demo', '1.0-demo').owner,
    } satisfies DeviceInfo;
    this.apiKey = 'demo';
    this.ready$.next(true);
    this.connected$.next(true);
    this.pushStatus();
  }

  protected override connectWebSocket(): void {
    // No real WebSocket in demo mode — pushStatus() drives status$ instead.
  }

  // ── Status helpers ────────────────────────────────────────────────────────────

  private buildStatus(): SystemStatus {
    return model.statusView(this.d) as unknown as SystemStatus;
  }

  private pushStatus(): void {
    this.status$.next(this.buildStatus());
  }

  // ── topology API (in-process, mirrors the mock's /api/* + real firmware) ──
  override async getTopology(): Promise<Topology> {
    if (!this.td) throw new Error('no topology configured');
    return this.td.topology;
  }

  override async putTopology(topology: Topology): Promise<{ ok: boolean }> {
    // Both shapes, like the mock and the firmware: a shop validates as a shop.
    const v = isShop(topology) ? validateShop(topology) : validateTopology(topology);
    if (!v.ok) throw new Error('invalid topology: ' + JSON.stringify(v.errors));
    this.td = createTopologyDevice(topology);
    // Whatever this shop is paired to is on the simulated network from here on,
    // the same as the mock does on PUT. See adoptOutlets().
    model.adoptOutlets(this.d, topology);
    this.persist();
    return { ok: true };
  }

  override async getStatus(): Promise<TopologyStatus> {
    if (!this.td) throw new Error('no topology configured');
    return topoStatus(this.td);
  }

  override async simTool(toolId: string, watts: number): Promise<TopologyStatus> {
    if (!this.td) throw new Error('no topology configured');
    setToolPower(this.td, toolId, watts);
    return topoStatus(this.td);
  }

  /** Manual switch. The model has one notion of "active" — a wattage — so this is
   *  the same lever with a synthetic reading, exactly as firmware does it.
   *
   *  The reading has to be BELIEVABLE, not merely large. It used to be 100000,
   *  a sentinel meaning "above any threshold anyone could set", which was
   *  invisible while nothing displayed watts. The canvas's plug row does, and it
   *  rendered a switched-on tool as "100.0 kW" — a number no shop tool draws.
   *  Three times the trip point clears every threshold test the same way and
   *  reads like a machine. Mirrors manualWattsFor() in
   *  firmware/control/TopologyRuntime.h — see the twin-pair table in CLAUDE.md. */
  override async setToolManual(toolId: string, on: boolean): Promise<unknown> {
    if (!this.td) throw new Error('no topology configured');
    // toolThreshold() is machineThreshold under its v1 name, and already falls
    // back to the default for a machine that has no plug configured.
    const trip = toolThreshold(this.td.topology, toolId);
    setToolPower(this.td, toolId, on ? Math.round(trip * 3) : 0);
    return { ok: true };
  }

  override async setCollectorManual(on: boolean, systemId?: string): Promise<unknown> {
    if (!this.td) throw new Error('no topology configured');
    const systems = systemsOf(this.td.topology as unknown as Parameters<typeof systemsOf>[0]);
    const id = systemId || (systems[0] && (systems[0].id as string));
    if (!id) throw new Error('no system');
    setCollectorManual(this.td, id, on);
    return { ok: true };
  }

  /** No servo to move in the demo — accept the nudge so the gate configurator is
   *  fully walkable, and remember the angle so a re-read reflects the last command. */
  override async jogServo(channel: number, angle: number, controllerId?: string): Promise<unknown> {
    // Keyed by board too — every controller numbers its channels 0-3, so channel
    // alone would have two boards' gate 1 overwriting each other.
    this.servoAngles.set(`${controllerId ?? ''}:${channel}`, angle);
    return { ok: true };
  }
  override async detachServo(_channel: number, _controllerId?: string): Promise<unknown> {
    return { ok: true };
  }
  private servoAngles = new Map<string, number>();

  private delay(ms: number): Promise<void> {
    return new Promise(r => setTimeout(r, ms));
  }

  /** Keep deviceInfo (read by Settings/visualizer) in step with the model. */
  private syncInfo(): void {
    if (!this.deviceInfo) return;
    this.deviceInfo.numStops      = this.d.numActiveStops;
    this.deviceInfo.motorInverted = this.d.motorInverted;
    this.deviceInfo.idleTimeoutSec = this.d.idleTimeoutSec;
  }

  // ── Read ──────────────────────────────────────────────────────────────────────

  override getMotionStatus(): Promise<SystemStatus> {
    return Promise.resolve(this.buildStatus());
  }

  // ── Motion (model owns state, we own the delay between begin/complete) ──────────

  override async home(): Promise<{ ok: boolean }> {
    const durMs = model.beginHome(this.d);
    this.pushStatus();
    await this.delay(durMs);
    model.completeHome(this.d);
    this.pushStatus();
    return { ok: true };
  }

  override async moveToStop(stop: number): Promise<{ ok: boolean }> {
    const durMs = model.beginMove(this.d, stop);
    this.pushStatus();
    await this.delay(durMs);
    model.completeMove(this.d, stop);
    this.pushStatus();
    return { ok: true };
  }

  override async jog(mm: number): Promise<{ ok: boolean }> {
    const durMs = model.beginJog(this.d, mm);
    this.pushStatus();
    await this.delay(durMs);
    model.completeJog(this.d);
    this.pushStatus();
    return { ok: true };
  }

  override estop(): Promise<{ ok: boolean }> {
    model.estop(this.d);
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override enable():  Promise<{ ok: boolean }> { return Promise.resolve(model.setEnabled(this.d, true)); }
  override disable(): Promise<{ ok: boolean }> { return Promise.resolve(model.setEnabled(this.d, false)); }

  // ── Dust collector ──────────────────────────────────────────────────────────────

  override setDustCollector(on: boolean): Promise<{ ok: boolean }> {
    model.switchDustCollector(this.d, on);
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override configureDustCollector(generation: number, ip: string): Promise<{ ok: boolean }> {
    model.configureDustCollector(this.d, { gen: generation, ip });
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override deleteDustCollector(): Promise<{ ok: boolean }> {
    model.deleteDustCollector(this.d);
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  // ── Calibration ───────────────────────────────────────────────────────────────

  override async saveStop(index: number): Promise<{ ok: boolean }> {
    // Client-side friendly pre-check (throws a helpful message) stays in the base
    // ApiService; the model then applies the device-level behaviour (an overlap
    // is silently skipped, matching firmware).
    this.checkStopConflict(index, this.d.positionMM);
    model.saveStop(this.d, index);
    this.syncInfo();
    this.pushStatus();
    return { ok: true };
  }

  override clearCal(): Promise<{ ok: boolean }> {
    model.clearCal(this.d);
    this.syncInfo();
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  // ── Outlets ───────────────────────────────────────────────────────────────────

  override async configureOutlet(cmd: OutletConfigCmd): Promise<{ ok: boolean }> {
    model.configureOutlet(this.d, {
      slot:      cmd.slot,
      name:      cmd.name,
      stop:      cmd.stop,
      ip:        cmd.ip,
      host:      cmd.host,
      gen:       cmd.generation,
      threshold: cmd.threshold_w,
    });
    this.pushStatus();
    return { ok: true };
  }

  override async pingOutlet(ip: string): Promise<PingResult> {
    await this.delay(400);
    const r = model.pingOutlet(this.d, ip);
    return { reachable: r.reachable, powerW: r.powerW, generation: r.gen, name: r.name };
  }

  override async discoverOutlets(): Promise<DiscoveredOutlet[]> {
    await this.delay(600);
    return model.discoverOutlets(this.d).map(x => ({
      ip:         x.ip,
      hostname:   x.hostname,
      name:       x.name,
      reachable:  x.reachable,
      powerW:     x.powerW,
      generation: x.gen,
      claim:      x.claim,
      holder:     x.holder ?? undefined,
      takeable:   x.takeable,
      claimReason: x.claimReason,
    }));
  }

  override async renameOutlet(ip: string, label: string, takeover = false): Promise<OutletNameResult> {
    await this.delay(350);   // a write to an outlet over the LAN is not instant
    const r = model.nameOutlet(this.d, ip, label, takeover);
    // The name lives on the OUTLET, not in the layout, so it is saved here — a
    // rename followed by a reload with no layout save must still stick.
    if (r.ok) this.persist();
    return r;
  }

  override async takeoverOutlet(ip: string): Promise<{ ok: boolean; error?: string }> {
    await this.delay(400);
    const r = model.takeoverOutlet(this.d, ip);
    if (r.ok) this.persist();
    return r;
  }

  override async releaseOutlet(ip: string): Promise<OutletReleaseResult> {
    await this.delay(350);
    const r = model.releaseOutlet(this.d, ip);
    this.persist();
    return r;
  }

  // ── Secondary boards ───────────────────────────────────────────────────
  // Four fake nodes so the boards surface is explorable in the demo. Two are
  // already in DEMO_TOPOLOGY's controllers[], one is not, and one belongs to
  // another primary — so "add", "already added" and "claimed by someone else"
  // are all visible without any hardware. Matches NETWORK_BOARDS in
  // tools/mock-api.js, which is the same fixture on the other runner.
  //
  // node-4 is here because the seed shop grew: node-2 used to be the unpaired
  // one, and the real layout that replaced the hand-built seed has it paired
  // (and driving nothing, which is its own thing worth being able to see). A
  // fourth board keeps every state on the screen.
  private readonly demoNodes: DiscoveredNode[] = [
    { host: 'dustgate-node-1', ip: '192.168.87.61', board: 'qtpy_s3', servos: 4 },
    { host: 'dustgate-node-2', ip: '192.168.87.62', board: 'devkitc', servos: 4 },
    { host: 'dustgate-node-3', ip: '192.168.87.63', board: 'xiao_c5', servos: 4,
      claimedBy: 'dustgate-garage', takeable: true },
    { host: 'dustgate-node-4', ip: '192.168.87.64', board: 'xiao_c5', servos: 4 },
  ];

  override async discoverNodes(): Promise<DiscoveredNode[]> {
    await this.delay(600);   // an mDNS sweep is not instant; don't pretend it is
    return this.demoNodes.map(n => ({ ...n }));
  }

  /** Which boards this fake device has PAIRED. Its own state, exactly as on the
   *  hardware (control/NodeRegistry.h, NVS) — and the reason Add works at all.
   *
   *  getNodes() used to derive the pairing list from the topology's controllers[],
   *  which made Add a no-op: pairNode wrote nothing, getNodes therefore didn't
   *  report the new board, and syncLayoutControllers only ever adds controllers
   *  for boards getNodes reported. The layout could only gain a board it already
   *  had. Nothing surfaced — the button just did nothing.
   *
   *  Seeded lazily from the topology so a demo shop that already names secondaries
   *  still shows them as paired. */
  private paired: Set<string> | null = null;
  private pairedHosts(): Set<string> {
    if (!this.paired) {
      const controllers = (this.td?.topology as { controllers?: Array<{ role: string; link?: { host?: string } }> })?.controllers ?? [];
      this.paired = new Set(
        controllers.filter(c => c.role === 'secondary').map(c => c.link?.host ?? '').filter(Boolean),
      );
    }
    return this.paired;
  }

  override async pairNode(host: string, name?: string): Promise<unknown> {
    this.pairedHosts().add(host);
    if (name) this.pairedNames.set(host, name);
    return { ok: true };
  }
  override async unpairNode(host: string): Promise<unknown> {
    this.pairedHosts().delete(host);
    this.pairedNames.delete(host);
    return { ok: true };
  }

  /** The friendly name a paired board carries. On the device this lives in the
   *  node registry (NVS), NOT in the layout — so it survives a layout wipe — and
   *  the mock keeps its own copy for the same reason. The demo had neither, so
   *  every board on /boards read as its hostname while the canvas, which reads
   *  controllers[], called the same board "Back wall". */
  private readonly pairedNames = new Map<string, string>();
  private nameForHost(host: string): string {
    const seeded = (this.td?.topology as { controllers?: Array<{ name?: string; link?: { host?: string } }> })
      ?.controllers?.find(c => c.link?.host === host)?.name;
    return this.pairedNames.get(host) ?? seeded ?? '';
  }

  override async getNodes(): Promise<NodeLinkState[]> {
    // node-2 is simulated as UNREACHABLE — the interesting case, and the one that's
    // hard to stage on a bench with two working boards.
    return [...this.pairedHosts()].map(host => {
      const known = this.demoNodes.find(n => n.host === host);
      const online = host !== 'dustgate-node-2';
      return {
        id: host,                          // the node's host IS its controllerId
        host,
        online,
        lastSeen: online ? Date.now() : 0,
        name: this.nameForHost(host),
        board: known?.board ?? 'unknown',
        fw: online ? '1.0.0-demo' : '',
        caps: { servos: known?.servos ?? 0, linear: 0 },
      };
    });
  }

  override saveOutletConfig(): Promise<{ ok: boolean }> {
    return Promise.resolve({ ok: true }); // state is already in-memory
  }

  override deleteOutlet(slot: number): Promise<{ ok: boolean }> {
    model.deleteOutlet(this.d, slot);
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  // ── Config ────────────────────────────────────────────────────────────────────

  override setHomedLeft(homedLeft: boolean): Promise<{ ok: boolean }> {
    model.setHomedLeft(this.d, homedLeft);
    this.syncInfo();
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override setMotorDirection(invert: boolean): Promise<{ ok: boolean }> {
    model.setMotorInverted(this.d, invert);
    this.syncInfo();
    return Promise.resolve({ ok: true });
  }

  override setNumGates(n: number): Promise<{ ok: boolean }> {
    model.setNumGates(this.d, n);
    this.syncInfo();
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override async calibrate(modelId: string, gateCount: number): Promise<{ ok: boolean }> {
    const durMs = model.beginCalibrate(this.d, modelId, gateCount);
    this.pushStatus();
    await this.delay(durMs);
    model.completeCalibrate(this.d);
    this.syncInfo();
    this.pushStatus();
    return { ok: true };
  }

  override setPortRole(index: number, role: string): Promise<{ ok: boolean }> {
    model.setPortRole(this.d, index, role as model.PortRole);
    this.pushStatus();
    return Promise.resolve({ ok: true });
  }

  override setIdleTimeout(seconds: number): Promise<{ ok: boolean }> {
    model.setIdleTimeout(this.d, seconds);
    this.syncInfo();
    return Promise.resolve({ ok: true });
  }

  override resetSetup(): Promise<{ ok: boolean }> {
    return this.clearCal();
  }

  override forgetWifi(): Promise<{ ok: boolean }> {
    return Promise.resolve({ ok: true }); // no real WiFi to forget
  }

  override async refreshInfo(): Promise<void> {
    // Already in sync — nothing to fetch
  }
}
