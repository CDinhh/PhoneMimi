# Tip videos

`tips.html` draws the three first-connect tip clips; every frame is a pure function of time.
`render.mjs` captures it frame by frame with puppeteer-core (system Chromium) and encodes
`tip1-3.mp4` (720x800, 30 fps, H.264) with ffmpeg-static:

```
npm ci                    # tools are already in node_modules on the desktop PC
node render.mjs 1 video   # also 2, 3; copy tipN.mp4 to resources/tips/
```

Edit `executablePath` in render.mjs to point at a Chromium-based browser.
