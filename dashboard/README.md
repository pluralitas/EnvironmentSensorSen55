# Bedroom Air Quality Dashboard

Static GitHub Pages dashboard for public ThingSpeak channels 3591940 and 3591941. It reads ThingSpeak directly from the browser. The ESP32 does not connect to GitHub.

## Deploy
1. Create a public GitHub repository, e.g. `Bedroom-Air-Quality-Dashboard`.
2. Upload `index.html` and `app.js` to the repository root.
3. Under **Settings > Pages**, select **Deploy from a branch**, branch `main`, folder `/ (root)`.
4. Open the Pages URL displayed by GitHub.

## Features
- Preset or custom time range, with custom inputs interpreted in Singapore time (UTC+8).
- Toggle 12 metrics; graph and CSV use the same selection.
- One-minute data joined from the two ThingSpeak channels by UTC reporting minute.
- Browser-side CSV export. No keys required for public channels.

## Caveats
- ThingSpeak can limit large API responses; 5-day chunks reduce request sizes but may still need pagination for unusually high-frequency data. Verify row counts for long exports.
- The current graph displays UTC timestamps; CSV timestamps are formatted in Singapore time.
- Long-range plots can be slow with many data points. Production enhancements should add downsampling for plotting while preserving full-resolution CSV.
- The channel join assumes both uploads occur in the same UTC minute. Uploads crossing a minute boundary can misalign; a shared timestamp or snapshot ID would be more reliable.
- This repository must not include `config_settings.h` containing Wi-Fi credentials or ThingSpeak write keys.
