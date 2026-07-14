# Dashboard

React, TypeScript, and Vite frontend for the ESP32 Water Level Monitor.

The dashboard reads the shared REST/WebSocket contract exposed by the local USB bridge today and by the ESP32 Wi-Fi firmware in a future release.

Run from the repository root with `npm start`, or run only this package:

```bash
npm ci
npm run dev
```

By default it connects to port 8787 on the same hostname used to load the page. The API base URL can also be changed in the dashboard connection panel.

Checks:

```bash
npm run lint
npm run build
```
