#!/usr/bin/env node
'use strict';
// Fails when the built UI bundle (dustgate-ui/dist) was made from an older shared/device-model than the one on disk. The model's
// constants are compiled into the bundle, so a stale bundle quietly disagrees with the brain (see shared/device-model/model-hash.js).
// Skips (exit 0) when there is no bundle: nothing is being served, so nothing is stale.
const fs = require('fs');
const path = require('path');
const { modelHash } = require('../shared/device-model/model-hash.js');

const dist = path.join(__dirname, '..', 'dustgate-ui', 'dist', 'dustgate-ui', 'browser');
if (!fs.existsSync(dist)) { console.log('[ui-fresh] no built bundle - nothing to check'); process.exit(0); }
const js = fs.readdirSync(dist).filter((f) => /^main.*\.js$/.test(f));
const want = modelHash();
const found = js.some((f) => fs.readFileSync(path.join(dist, f), 'utf8').includes(want));
if (found) { console.log('[ui-fresh] the built app matches the model (' + want + ')'); process.exit(0); }
console.error('[ui-fresh] STALE: the built app in dustgate-ui/dist was not built from the current shared/device-model (' + want + ').\n' +
              '           Rebuild it:  cd dustgate-ui && npm run build   (a constant changed in the model is otherwise still the old one in the app)');
process.exit(1);
