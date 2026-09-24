# demo/ — the browser demo

Live at **<https://wipe-demo.stoatworks-labs.com>**. Not served from this
README: `.assetsignore` keeps this file and `tools/` out of the upload.

    index.html       the shell
    plugin.js        the parameters, the plugin's two shaders, the render
    waveform.js      the CPU half, ported: Controls.cpp, Waveform.cpp, ModPhase
    area-worker.js   the lattice Area solves, off the main thread
    vendor/          the shared kit, copied in by sync.sh — DO NOT EDIT
    tools/           check_shaders.py, run by tools/verify.sh
    _headers         CSP and caching, honoured by the Cloudflare assets runtime

## What this page is, exactly

A **port**, not a recording and not the plugin.

The shaders are the plugin's, copied across unedited: `VERTEX` and `WIPE` in
`plugin.js` are `kVertexShader` and `kWipeShader` from `source/Shaders.cpp`.
`tools/check_shaders.py` compares them character for character and
`../tools/verify.sh` runs it.

The CPU half is a port — `waveform.js` and the Flip-Flop and cache logic in
`plugin.js` — and **nothing checks it but a reader.**

Wipe is a **mixer**, and the kit hands a demo one input. So A (the layer below)
is the kit's clip, relabelled `Clip A`, and B (this layer) is a second copy of
the kit's clip generator, picked by the transport's `Clip B`. In Resolume the
fader is the layer's opacity fader; here it is the `Opacity` slider.

Everything else is not the plugin: no Resolume, no layer stack, no FFGL, no
padded textures (both MaxUVs are 1), and GLSL ES 3.00 in WebGL2 rather than
desktop GL 4.1 core. The page's own disclosure lists every difference.

## Working on it

```bash
python3 -m http.server 8951          # from this directory
python3 tools/check_shaders.py       # the copies still match the C++
../tools/verify.sh                   # everything, including the above
```

There is no build step. It is hand-written ES modules and what is committed is
what is served. A push to main deploys it (`.github/workflows/deploy.yml`);
by hand, `cf-run npx wrangler deploy` from the repo root. Verify by content:
`curl -s 'https://wipe-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`.

**After changing a shader in `source/Shaders.cpp`, copy it across here too** —
`check_shaders.py` names the shader and the first differing line. After
changing Controls.cpp, Waveform.cpp or Timing.cpp, change `waveform.js` to
match; nothing will tell you if you forget.
