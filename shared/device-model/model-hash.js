'use strict';
// A fingerprint of the model's RUNTIME files (not its tests or conformance runners): what the UI bundle was built from.
//
// The UI imports this directory at BUILD time, so a constant here (COLLECTOR_RUNNING_W, a threshold, a text) is copied into the
// bundle and goes stale the moment the model changes and nobody rebuilds. Found 2026-10-06: the brain was changed to 25 W and the
// Live screen, still on the old bundle, called the same fan "Not starting". gen-build-info.js bakes this hash into the bundle and
// tools/check-ui-fresh.js compares it to the current one, so a stale bundle is a failing check, not a bench surprise.
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

function modelHash() {
  const dir = __dirname;
  const files = fs.readdirSync(dir)
    .filter((f) => /\.(js|ts)$/.test(f) && !/\.test\.|conformance|fixtures|model-hash/.test(f))
    .sort();
  const h = crypto.createHash('sha1');
  for (const f of files) { h.update(f); h.update(fs.readFileSync(path.join(dir, f))); }
  return h.digest('hex').slice(0, 12);
}
module.exports = { modelHash };
