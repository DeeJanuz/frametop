// frametop-float: the KWin side of floating windows (see docs/floating-windows.md). ft-floatd
// loads it into the desktop's KWin over D-Bus (org.kde.kwin.Scripting) and talks to it:
//   - events go to ft-floatd as JSON strings (org.frametop.Float.Event), for the windows it
//     cares about: floating windows (the ones on a spare output, WL-<screens> and up), their
//     popups and dialogs, new windows, and requests to float or dock one;
//   - commands come back through a long poll: the script calls NextCommand, ft-floatd
//     answers when it has one (or after a while with nothing), and the script calls again.
// KWin scripts can call D-Bus but can't serve it, hence the poll. Window ids are KWin's
// internalId (a UUID string).

const SERVICE = "org.frametop.Float", PATH = "/Float", IFACE = "org.frametop.Float";
let screens = 0;       // outputs WL-0 .. WL-<screens - 1> are screens; the rest are spares
let polling = false;
const watched = {};    // id -> true once its signals are connected
let marking = false;   // the script itself is setting keep-below (see mark)
const settled = {};    // id -> {output, frame, fullScreen, maximized}: where a window belongs (see putBack)
const held = {};       // id -> {w, h, until, asked}: a size asked for, for a second (see hold)
const moved = {};      // id -> true, or "back" once put back: it moved while KWin changed the outputs
let layout = "", layoutOutputs = {}, layoutSince = 0;  // the outputs at the last screensChanged

function send(ev) {
    callDBus(SERVICE, PATH, IFACE, "Event", JSON.stringify(ev));
}

function outputIndex(o) {
    const m = o ? /^WL-(\d+)$/.exec(o.name) : null;
    return m ? parseInt(m[1]) : -1;
}
function isSpare(o) {
    return screens > 0 && outputIndex(o) >= screens;
}
function rect(g) {
    return {x: g.x, y: g.y, w: g.width, h: g.height};
}
function byId(id) {
    const all = workspace.windowList();
    for (let i = 0; i < all.length; ++i)
        if (String(all[i].internalId) === id) return all[i];
    return null;
}
function outputByName(name) {
    const all = workspace.screens;
    for (let i = 0; i < all.length; ++i)
        if (all[i].name === name) return all[i];
    return null;
}
function info(w) {
    const o = w.output;
    return {
        id: String(w.internalId), pid: w.pid, cls: String(w.resourceClass), app: String(w.desktopFileName),
        caption: String(w.caption), output: o ? o.name : "", outputRect: o ? rect(o.geometry) : null,
        frame: rect(w.frameGeometry), client: rect(w.clientGeometry), popup: w.popupWindow,
        transient: w.transient, parent: w.transientFor ? String(w.transientFor.internalId) : "",
        normal: w.normalWindow, dialog: w.dialog, fullScreen: w.fullScreen, minimized: w.minimized,
        onAllDesktops: w.onAllDesktops, maximized: isMaximized(w)
    };
}

// KWin 6.2's scripts have no maximize mode to read: a window is maximized when it fills its
// output's maximize area.
function isMaximized(w) {
    if (!w.normalWindow || !w.output) return false;
    const a = workspace.clientArea(KWin.MaximizeArea, w), g = w.frameGeometry;
    return g.x === a.x && g.y === a.y && g.width === a.width && g.height === a.height;
}

function report(type, w) {
    if (w.deleted) return;  // a window on its way out still changes output and size
    const ev = info(w);
    ev.ev = type;
    send(ev);
}

// KWin's placement memory (its PlacementTracker) keeps each window's geometry for each layout of
// the outputs (every enabled output's name and geometry), and when the outputs come back to a
// layout it has seen, it puts the windows back where they were in it. That's for plugging monitors
// in and out, and it does harm here. A spare output changes size after its window does, so what
// KWin keeps for a spare's size is the window's next size: resizing a floating window back to a
// size it had set off an endless flip between two sizes. And floating or docking one window could
// move others, even onto a spare or off one. So the script keeps where each window belongs
// (settled), tells ft-floatd nothing while KWin changes the outputs, and once KWin is done
// (screensChanged comes after its restore) puts the floating windows back, and the screens' windows
// too when only spares changed.
function outputsNow() {
    const all = workspace.screens, out = {};
    for (let i = 0; i < all.length; ++i) {
        const g = all[i].geometry;
        out[all[i].name] = g.x + "," + g.y + " " + g.width + "x" + g.height;
    }
    return out;
}
function keyOf(outputs) {
    return Object.keys(outputs).sort().map(n => n + "=" + outputs[n]).join(" ");
}
function takeLayout() {
    layoutOutputs = outputsNow();
    layout = keyOf(layoutOutputs);
    layoutSince = 0;
}
// KWin is changing the outputs: they differ from the last screensChanged.
function changingOutputs() {
    if (keyOf(outputsNow()) === layout) {
        layoutSince = 0;
        return false;
    }
    if (!layoutSince) {
        layoutSince = Date.now();
    } else if (Date.now() - layoutSince > 2000) {
        takeLayout();  // screensChanged should have come by now: don't stay quiet for good
        return false;
    }
    return true;
}
function settle(w) {
    if (w.output) {
        settled[String(w.internalId)] = {output: w.output.name, frame: rect(w.frameGeometry), fullScreen: w.fullScreen};
    }
}
// ft-floatd put the window here: it's where it belongs now.
function expect(w, output, c, fullScreen) {
    settled[String(w.internalId)] = {output: output, frame: {x: c.x, y: c.y, w: c.w, h: c.h}, fullScreen: fullScreen};
    hold(w, c.w, c.h);
}

// A size asked for (by ft-floatd, or by the script putting a window back) comes in when the app
// answers, and until then the app can still answer older requests: one KWin's restore made, or,
// just after it opened, its own. For a second, the script asks again instead of taking those;
// then it takes the size the window has (an app can refuse a size, below its minimum).
const holdTimer = new QTimer();
holdTimer.singleShot = true;
holdTimer.timeout.connect(() => {
    const now = Date.now();
    Object.keys(held).forEach(id => {
        const h = held[id];
        if (h.until > now) return;
        delete held[id];
        const w = byId(id);
        if (!h.asked || !w || w.deleted) return;  // (nothing held back: nothing to tell)
        settle(w);
        if (isSpare(w.output)) report("geometry", w);
    });
    if (Object.keys(held).length) holdTimer.start();
});
function hold(w, width, height) {
    held[String(w.internalId)] = {w: width, h: height, until: Date.now() + 1000};
    holdTimer.interval = 1100;
    holdTimer.start();
}
// A size change while a size is held: true when it isn't that size (the script asked again).
// (ft-floatd's sizes can be fractional, the window's are whole: within a pixel is the same.)
function holding(w) {
    const id = String(w.internalId), h = held[id], s = settled[id];
    if (!h) return false;
    const g = w.frameGeometry;
    if (!s || h.until < Date.now() || w.move || w.resize || (Math.abs(g.width - h.w) < 1 && Math.abs(g.height - h.h) < 1)) {
        delete held[id];
        return false;
    }
    w.frameGeometry = {x: s.frame.x, y: s.frame.y, width: h.w, height: h.h};
    h.asked = true;
    return true;
}

// Runs while KWin's output change still counts as going on (nothing reported), so the steps on
// the way don't reach ft-floatd: told only where the window ends up (see reportMoved).
function putBack(w, screensChanged) {
    const id = String(w.internalId), s = settled[id];
    if (w.deleted || !s || screens === 0 || !marked(w)) return;
    const o = outputByName(s.output);
    // KWin's restore sets full screen (and maximized) as it was in that layout too: with a
    // floating window that flipped forever, its output changing size with it. Ask for the
    // state it had: KWin's request hasn't reached the app yet, so it never sees it.
    w.fullScreen = s.fullScreen;
    if (o && isSpare(o) && !s.fullScreen) w.setMaximize(false, false);
    // Not where KWin had to move it: its output went, a screen changed, or it's full screen or
    // maximized (KWin fits those to their output).
    const back = o && !s.fullScreen && !w.fullScreen && !w.move && !w.resize && !s.maximized
        && (isSpare(o) || (!screensChanged && !isMaximized(w)));
    if (!back) return;
    if (!w.output || w.output.name !== s.output) workspace.sendClientToScreen(w, o);
    w.frameGeometry = {x: s.frame.x, y: s.frame.y, width: s.frame.w, height: s.frame.h};
    if (isSpare(o)) hold(w, s.frame.w, s.frame.h);
    // Moved during the change (by KWin, by the lines above, or the size ft-floatd asked for came
    // in): report where it is, and keep settled as it is.
    if (moved[id]) moved[id] = "back";
}
// After an output change: tell ft-floatd where the windows that moved during it are now.
function reportMoved(w) {
    const id = String(w.internalId), s = settled[id], how = moved[id];
    if (!how) return;
    delete moved[id];
    if (w.deleted) return;
    if (how !== "back") settle(w);
    if (!w.output || !s || w.output.name !== s.output) {
        report("output", w);
        mark(w);
    } else if (isSpare(w.output)) {
        report("geometry", w);
    }
}
workspace.screensChanged.connect(() => {
    const before = layoutOutputs, now = outputsNow();
    if (keyOf(now) === layout) return;
    let screensChanged = screens === 0;
    Object.keys(Object.assign({}, before, now)).forEach(name => {
        const m = /^WL-(\d+)$/.exec(name);
        if (before[name] !== now[name] && !(m && parseInt(m[1]) >= screens)) screensChanged = true;
    });
    const all = workspace.windowList();
    all.forEach(w => putBack(w, screensChanged));
    takeLayout();
    all.forEach(reportMoved);
});

// Floating windows, and popups and dialogs on a spare output: tell ft-floatd about changes.
function watch(w) {
    const id = String(w.internalId);
    if (watched[id]) return;
    watched[id] = true;
    const onSpare = () => isSpare(w.output);
    w.frameGeometryChanged.connect(() => {
        if (changingOutputs()) {
            moved[id] = true;
            return;
        }
        if (holding(w)) return;
        settle(w);
        if (onSpare()) report("geometry", w);
    });
    w.outputChanged.connect(() => {
        if (changingOutputs()) {
            moved[id] = true;
            return;
        }
        if (!held[id]) settle(w);  // (held: the place asked for is settled already)
        report("output", w);
        mark(w);
    });
    w.keepBelowChanged.connect(() => keepBelowChanged(w));
    w.interactiveMoveResizeStarted.connect(() => {
        if (onSpare()) send({ev: "move-start", id: id, move: w.move, resize: w.resize, frame: rect(w.frameGeometry)});
    });
    w.interactiveMoveResizeFinished.connect(() => { if (onSpare()) report("move-end", w); });
    w.fullScreenChanged.connect(() => {
        if (changingOutputs()) {
            moved[id] = true;
            return;
        }
        if (settled[id]) settled[id].fullScreen = w.fullScreen;
        if (onSpare()) report("fullscreen", w);
    });
    w.minimizedChanged.connect(() => { if (onSpare()) report("minimized", w); });
    w.maximizedChanged.connect(() => {
        // A floating window stays an ordinary window: its output is its size plus a margin.
        if (onSpare() && w.normalWindow && !w.fullScreen) w.setMaximize(false, false);
    });
}

workspace.windowAdded.connect(w => {
    watch(w);
    settle(w);
    report("added", w);
});
workspace.windowRemoved.connect(w => {
    const id = String(w.internalId);
    send({ev: "removed", id: id});
    delete watched[id];
    delete settled[id];
    delete held[id];
    delete moved[id];
});
workspace.windowActivated.connect(w => {
    if (w && isSpare(w.output)) send({ev: "activated", id: String(w.internalId)});
});
takeLayout();
workspace.windowList().forEach(w => {
    watch(w);
    settle(w);
});

// Keep-below means "floating" in the Frametop desktop. The title bar's float button (Frametop's
// window decoration, decoration/) is the Keep Below button, so setting the flag on a window on
// the screens floats it, and clearing it on a floating one docks it. The script keeps the flag
// set on every floating window, its dialogs included, and cleared everywhere else, however the
// window got there. Kept below, a window alone on its own output only has the wallpaper under it.
function marked(w) {
    return w.managed && !w.deleted && !w.specialWindow && !w.popupWindow;
}
function topOf(w) {
    let top = w;
    for (let n = 0; top.transientFor && n < 10; ++n) top = top.transientFor;
    return top;
}
function mark(w) {
    if (screens === 0 || !marked(w)) return;
    const want = isSpare(w.output);
    if (w.keepBelow === want) return;
    marking = true;
    w.keepBelow = want;
    marking = false;
}
function keepBelowChanged(w) {
    if (marking || screens === 0 || !marked(w)) return;
    const top = topOf(w);
    if (w.keepBelow !== isSpare(top.output)) requestFloat(top);
}

function requestFloat(w) {
    if (!w || !w.normalWindow || w.popupWindow) return;
    report(isSpare(w.output) ? "dock-request" : "float-request", w);
}

registerUserActionsMenu(w => {
    if (!w.normalWindow || w.popupWindow) return null;
    const floating = isSpare(w.output);
    return {
        text: floating ? "Back to Desktop" : "Float in VR",
        icon: floating ? "window-restore" : "window-new",
        triggered: () => requestFloat(w)
    };
});
// The float key is the input relay's (float_toggle, Meta+Shift+F by default): it reaches us as
// "request-pointer". No shortcut of KWin's own, so one press can't float a window and dock it again.

// The window under KWin's pointer (where the 3D mouse or a laser last was on a panel): the top
// one there, a popup or dialog standing for the window it belongs to. Null over the wallpaper
// or the taskbar.
function underPointer() {
    const p = workspace.cursorPos;
    const order = workspace.stackingOrder;
    for (let i = order.length - 1; i >= 0; --i) {
        const w = order[i];
        if (w.deleted || w.minimized || w.hidden || !w.managed) continue;
        const g = w.frameGeometry;
        if (p.x < g.x || p.y < g.y || p.x >= g.x + g.width || p.y >= g.y + g.height) continue;
        const top = topOf(w);
        return top.normalWindow && !top.popupWindow ? top : null;
    }
    return null;
}

function run(c) {
    const w = c.id ? byId(c.id) : null;
    switch (c.cmd) {
        case "config":
            screens = c.screens;
            workspace.windowList().forEach(w => { report("window", w); mark(w); });
            break;
        case "mark":  // after a float that didn't happen: keep-below back as it was
            if (w) mark(w);
            break;
        case "place": {  // onto an output, at a frame rectangle (logical, global)
            if (!w) break;
            const o = outputByName(c.output);
            if (!o) break;
            if (w.fullScreen && !c.keepFullScreen) w.fullScreen = false;
            w.setMaximize(false, false);
            expect(w, o.name, c, w.fullScreen && !!c.keepFullScreen);
            workspace.sendClientToScreen(w, o);
            w.frameGeometry = {x: c.x, y: c.y, width: c.w, height: c.h};
            if (c.onAllDesktops !== undefined) w.onAllDesktops = c.onAllDesktops;
            if (c.maximized) {
                // Maximized: KWin picks the size, and the place above is only where it goes.
                delete held[c.id];
                settled[c.id].maximized = true;
                w.setMaximize(true, true);
            }
            break;
        }
        case "geometry":
            if (!w) break;
            expect(w, settled[c.id] ? settled[c.id].output : (w.output ? w.output.name : ""), c, w.fullScreen);
            w.frameGeometry = {x: c.x, y: c.y, width: c.w, height: c.h};
            break;
        case "close":
            if (w) w.closeWindow();
            break;
        case "activate":
            if (w) workspace.activeWindow = w;
            break;
        case "activate-output": {  // the top window on that output (a spin brought it to the front)
            const order = workspace.stackingOrder;
            for (let i = order.length - 1; i >= 0; --i) {
                const o = order[i];
                if (o.deleted || o.minimized || o.hidden || !o.managed || !o.output) continue;
                if (o.output.name !== c.output || !o.normalWindow || o.popupWindow) continue;
                workspace.activeWindow = o;
                break;
            }
            break;
        }
        case "minimize":
            if (w) w.minimized = c.on;
            break;
        case "info":
            if (w) report("window", w);
            break;
        case "report-all":  // a profile's capture: every window as it is now, then a marker
            workspace.windowList().forEach(w => report("window", w));
            send({ev: "reported", token: c.token});
            break;
        case "request-float":  // ft-float float ID: float it, if it isn't floating
            if (w && !isSpare(w.output)) requestFloat(w);
            break;
        case "request-active":  // ft-float float|dock active
            requestFloat(workspace.activeWindow);
            break;
        case "request-pointer":  // the float key: the window under the pointer, else the active one
            requestFloat(underPointer() || workspace.activeWindow);
            break;
    }
}

function poll() {
    if (polling) return;
    polling = true;
    callDBus(SERVICE, PATH, IFACE, "NextCommand", reply => {
        polling = false;
        if (reply) {
            try {
                JSON.parse(reply).forEach(run);
            } catch (e) {
                print("frametop-float: bad command " + reply + ": " + e);
            }
        }
        poll();
    });
}

send({ev: "hello"});
poll();
