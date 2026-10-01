// Frametop gaze first: a filter in Steam's UI (SharedJSContext, through Steam's CEF debugger on
// 127.0.0.1:8080) that keeps the Frame controllers' gamepad input away from Steam's UI while
// gaze mode has the dashboard (docs/gaze-first.md). Steam reads the controllers as its own
// virtual gamepad (Steam Input), so no SteamVR binding can do this.
//
// Steam's gamepad input source (webpack module 17900, class E) turns each controller message
// into this.OnButtonDown / OnButtonUp / OnAnalogPad calls, which live on its base class. The
// filter defines wrappers on E's prototype, so both live instances go through them. They look
// up window.__frametopGaze at every call, so evaluating this file again updates the logic in
// place; a wrapper from an older version is replaced. It returns the state as JSON.
//
// The dashboard's mode (laser or gamepad) is SteamVR's, not the UI's: the UI only mirrors it
// (VRFocus, module 84114: J.Instance.ShowGamepadFocusMode, from SteamVR's
// system_panel_interaction_mode).
//
// window.__frametopGaze:
//   mode    "off" (pass everything), "log" (pass, and log), "block" (drop all but `allow`),
//           "auto" (block while the dashboard is in laser mode, pass in gamepad mode)
//   until   with "block" or "auto": blocking stops at this time (Date.now(), ms) unless it's
//           moved on. The input relay (input/gazefirst.py) sets it 15 s ahead every 10 s or so,
//           so if the relay dies, the controllers come back to Steam's UI by themselves.
//   allow   buttons that always pass: the Steam button's guide and quick menu, and Steam's
//           own dummy input
//   log     the last 256 events: [ms, "down"|"up"|"analog", button, controller, passed]
//   modes   the dashboard's mode changes: [ms, "gamepad"|"laser"] (sampled every 50 ms)
// A release passes if its press did, so nothing stays held when blocking starts.
// Buttons (Steam's enum): 1 OK 2 CANCEL 3 SECONDARY 4 OPTIONS 5/6 bumpers 7/8 triggers 9-12 dpad
// 13 SELECT 14 START 15/16 stick clicks 17/18 stick touch 23-26 rear buttons (24 left grip,
// 26 right grip) 27 STEAM_GUIDE 28 STEAM_QUICK_MENU 29 DUMMY_INPUT.
(() => {
    const VERSION = 2;
    if (!window.__ftreq) {
        const name = Object.keys(window).find(k => k.startsWith("webpackChunk"));
        window[name].push([[Symbol("frametop")], {}, r => { window.__ftreq = r; }]);
    }
    const E = window.__ftreq(17900).E;
    const proto = E.prototype;
    if (!("HandleControllerInputMessages" in proto)) throw new Error("Steam's gamepad source moved (module 17900)");
    const base = Object.getPrototypeOf(proto);
    const G = window.__frametopGaze || (window.__frametopGaze = {mode: "log", allow: [27, 28, 29], log: [], held: {}, dropped: 0});
    G.modes = G.modes || [];
    G.gamepadMode = () => {
        try { return !!window.__ftreq(84114).J.Instance?.ShowGamepadFocusMode; } catch (e) { return false; }
    };
    G.note = (ev, button, controller, passed) => {
        G.log.push([Math.round(performance.now()), ev, button, controller, passed]);
        if (G.log.length > 256) G.log.shift();
    };
    G.blocks = button => {
        if (G.allow.includes(button) || (G.until && Date.now() > G.until)) return false;
        return G.mode === "block" || (G.mode === "auto" && !G.gamepadMode());
    };
    if (proto.OnButtonDown && proto.OnButtonDown.__frametop !== VERSION) {
        delete proto.OnButtonDown;
        delete proto.OnButtonUp;
        delete proto.OnAnalogPad;
    }
    if (!Object.prototype.hasOwnProperty.call(proto, "OnButtonDown")) {
        proto.OnButtonDown = function (button, controller, ...rest) {
            let pass = true;
            try {
                const g = window.__frametopGaze;
                g.inst = this;
                pass = !g.blocks(button);
                if (g.mode !== "off") g.note("down", button, controller, pass);
                if (pass) g.held[controller + ":" + button] = true;
                else g.dropped++;
            } catch (e) { pass = true; }
            if (pass) return base.OnButtonDown.call(this, button, controller, ...rest);
        };
        proto.OnButtonUp = function (button, controller, ...rest) {
            let pass = true;
            try {
                const g = window.__frametopGaze, key = controller + ":" + button;
                pass = !!g.held[key] || !g.blocks(button);
                delete g.held[key];
                if (g.mode !== "off") g.note("up", button, controller, pass);
            } catch (e) { pass = true; }
            if (pass) return base.OnButtonUp.call(this, button, controller, ...rest);
        };
        proto.OnAnalogPad = function (button, x, y, controller, ...rest) {
            let pass = true;
            try {
                const g = window.__frametopGaze;
                pass = !g.blocks(button);
                if (g.mode !== "off" && (x || y)) g.note("analog", button, controller, pass);
            } catch (e) { pass = true; }
            if (pass) return base.OnAnalogPad.call(this, button, x, y, controller, ...rest);
        };
        for (const f of [proto.OnButtonDown, proto.OnButtonUp, proto.OnAnalogPad]) f.__frametop = VERSION;
        G.installed = Date.now();
    }
    if (!G.sampler) {
        let last = null;
        G.sampler = setInterval(() => {
            const m = G.gamepadMode() ? "gamepad" : "laser";
            if (m !== last) {
                last = m;
                G.modes.push([Math.round(performance.now()), m]);
                if (G.modes.length > 128) G.modes.shift();
            }
        }, 50);
    }
    return JSON.stringify({version: VERSION, mode: G.mode, gamepad: G.gamepadMode(), installed: G.installed,
                           dropped: G.dropped, modes: G.modes.slice(-10), log: G.log.slice(-10)});
})()
