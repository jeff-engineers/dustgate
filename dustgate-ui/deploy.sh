#!/usr/bin/env bash
# Build the Angular app and copy compressed assets to ESP32 LittleFS data folder.
# After running this, upload to the device with:  pio run --target uploadfs
set -e

DATA_DIR="../linear_actuator/data"

echo "Building Angular app..."
npm run build

BROWSER_DIR="dist/dustgate-ui/browser"
if [ ! -d "$BROWSER_DIR" ]; then
  BROWSER_DIR="dist/dustgate-ui"
fi

echo "Copying to $DATA_DIR..."
rm -rf "$DATA_DIR"
mkdir -p "$DATA_DIR"
cp -r "$BROWSER_DIR"/* "$DATA_DIR/"

echo "Compressing JS and CSS for ESP32 serving..."
for f in "$DATA_DIR"/*.js "$DATA_DIR"/*.css; do
  if [ -f "$f" ]; then
    gzip -9 "$f"            # creates $f.gz and removes $f
    echo "  compressed: $(basename "$f")"
  fi
done
# Leave index.html uncompressed (it's tiny and ESPAsyncWebServer serves it directly)

echo ""
echo "Done. Files in $DATA_DIR:"
ls -lh "$DATA_DIR"
echo ""
echo "Next step: cd .. && pio run --target uploadfs"
