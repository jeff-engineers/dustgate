# Archive

Documents that are **no longer current**: superseded RFCs and specs, plans that have been built, audits that have been
worked through, and the old wiring notes. Kept because the reasoning is worth more than the file: knowing why something
was done, or not done, is what stops it being re-proposed.

**Nothing here is a source of truth.** The current sources are the code, [`CLAUDE.md`](../../CLAUDE.md),
[`TODO/`](../../TODO/TODO.md), [`docs/BOM.md`](../BOM.md), [`docs/carrier-wiring.html`](../carrier-wiring.html) and the two
canonical mockups ([`canvas.html`](../mockups/canvas.html), [`decisions.html`](../mockups/decisions.html)). A fact found only
here is one to re-check before relying on it. Each file opens with a banner saying what replaced it. Archived mockups have
their own folder, [`docs/mockups/archived/`](../mockups/archived/README.md).

Archived 2026-10-18:

| File | What it was |
|---|---|
| [`REQUIREMENTS.md`](REQUIREMENTS.md) | The original requirements, from the single-slider product. |
| [`WIRING.md`](WIRING.md) | The pin-level wiring notes, 2026-08 to 2026-10. |
| [`architecture-rfc.md`](architecture-rfc.md) | The v2 architecture RFC (2026-07/08). |
| [`boards-on-canvas-plan.md`](boards-on-canvas-plan.md) | A plan, DONE 2026-08-16. |
| [`brain-options.md`](brain-options.md) | A decision document; the brain now runs on a Pi (`native/`). |
| [`cleanup-audit.md`](cleanup-audit.md) | An audit, worked through 2026-10-05/06. |
| [`dual-endstop-calibration.md`](dual-endstop-calibration.md) | The phase-1 slider calibration spec. |
| [`duct-routing-rewrite-plan.md`](duct-routing-rewrite-plan.md) | A plan, implemented 2026-08-09. |
| [`node-vs-brain.md`](node-vs-brain.md) | A snapshot (2026-10-04) of who decides what. |
| [`nodes-dial-the-brain-plan.md`](nodes-dial-the-brain-plan.md) | A plan, built 2026-10-04. |
| [`optional-nodes-plan.md`](optional-nodes-plan.md) | A plan, built 2026-10-08. |
| [`outlet-discovery-provisioning.md`](outlet-discovery-provisioning.md) | The smart-plug discovery and provisioning design. |
| [`review-2026-09-08.md`](review-2026-09-08.md) | A code review; its open findings moved to `TODO/TODO.md` ("Small fixes carried from the 2026-09-08 code review"). |
| [`shop-schema-rfc.md`](shop-schema-rfc.md) | The N-systems shop schema RFC. |
| [`tool-sensing-rfc.md`](tool-sensing-rfc.md) | The tool-sensing RFC: decided, and the reasoning behind much of the sensing code. |
| [`topology-schema.md`](topology-schema.md) | The July draft of the layout schema. It called itself the single source of truth; it is not. |
| [`ui-design.md`](ui-design.md) | The July interface design. |
| [`provision.html`](provision.html) | A Web Serial WiFi-provisioning page from the first commit; replaced by `dev.sh provision`. |
