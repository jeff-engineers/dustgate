/** The wattage above which a tool's plug counts as "running" when a layout names none. ONE definition: the tool sheet and the
 *  canvas's outlet sheet each had their own `50`, so a change in one left the other writing the old default. (The firmware's own
 *  fallback for a document that omits `thresholdW` is smaller — kDefaultThresholdW in control/TopologyController.h — and is a
 *  different number on purpose: this is what the UI WRITES, that is what the device assumes when nothing was written.) */
export const DEFAULT_THRESHOLD = 50;
