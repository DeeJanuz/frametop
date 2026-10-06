#!/usr/bin/env python3
"""Frametop Hand Recorder: record your hands for the open hand dataset (hands/rec/DESIGN.md).

A Kirigami (QML) app with a Python backend. It runs in the dev container:
  - Welcome: the consent text (CONSENT.md), shown the first time and again when its version
    changes. Agreeing writes profile.json with a random contributor id.
  - Before you start: the checklist (objects, controller straps, lighting, sleeves, privacy,
    free space), the camera check (hands/camcheck.py: Start stays off while the upper cameras
    are off, with a Restart SteamVR button; --ignore-cameras overrides it), a full session or a
    quick round (suggested once a full one is done) and what will happen. Start hands it to
    the session runner (session.py).
  - Session: the runner's live status with the step's pose picture and where-to diagram, Next,
    Pause/Resume, Redo, Skip section and Stop (Space: Next, P, R, S and Esc while the window has
    focus; the button on the headset's right side is Next, pause and resume). The prompts appear
    in the headset. Each step waits for Next unless "Advance by itself" was ticked.
  - Review: sessions, their takes, and a viewer for one frame set at a time, where ranges,
    takes and sessions can be deleted (takes.py).
  - Export: compress what's kept into exports/<session>/ at nice 19 (takes.py), with a warning
    when the headset is worn.
  - Upload: the Hugging Face login status, and Upload: hub.py checks the export (validate.py)
    and uploads it as a pull request, in a child process (Cancel ends it). UPLOAD.md explains
    the steps, with the export filled in, and the command to copy for a terminal upload.
Everything lives under ~/.local/share/frametop/hands/contrib (--base). Nothing is uploaded
unless the person presses Upload; while CONSENT.md or UPLOAD.md is a draft, Upload stays off
unless FT_HANDREC_ALLOW_UPLOAD=1 (the maintainer's rehearsal against a test repo, picked with
FT_HANDREC_DATASET). --hub-dry-run does everything but the network calls.
Launch with hands/rec/ft-handrec (host wrapper). The standalone Hand Recorder
(github.com/DeeJanuz/frametop-hand-recorder) runs this backend on the host, from a venv, with
its own window for SteamVR's dashboard: --qml its main.qml, --style Basic.
"""
import argparse
import datetime
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
import uuid

from PySide6.QtCore import Property, QObject, Qt, QTimer, QUrl, Signal, Slot
from PySide6.QtGui import QColor, QDesktopServices, QFont, QGuiApplication, QIcon, QImage, QPainter, QPalette
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtQuick import QQuickImageProvider
from PySide6.QtQuickControls2 import QQuickStyle
import shiboken6

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hub  # noqa: E402
import takes  # noqa: E402  (next to this file)

# The dataset contributions go to (hub.py; FT_HANDREC_DATASET overrides it, for tests).
HF_DATASET = hub.HF_DATASET
CONSENT_PATH = hub.CONSENT_PATH
UPLOAD_PATH = hub.UPLOAD_PATH
HUB_PATH = os.path.join(HERE, "hub.py")
VALIDATE_PATH = os.path.join(HERE, "validate.py")
AWAKE_UNIT = "frametop-handrec-awake.service"
SCRIPT_PATH = os.path.join(HERE, "script.json")
# The headset counts as worn while vrcompositor runs and a display panel is lit: SteamVR turns
# the panels off 5 s after the headset comes off (frame-job's check; the proximity sensor's
# readings are too noisy). The dev container sees the host's processes and /sys.
BACKLIGHTS = "/sys/class/backlight"
ROUND_BYTES = 10 * 1000 ** 3  # about what one round of recording takes
# The checklist's choices; the keys are what session.json stores.
OBJECTS = [("pencil", "Pencil or pen"), ("phone", "Phone"), ("cup", "Cup or mug (empty)"),
           ("keyboard", "Keyboard"), ("mouse", "Mouse"), ("gamepad", "Gamepad"),
           ("small", "Something small (a coin, a key, a bottle cap)")]
# "auto": the cameras' measurement (session.classify_lighting: indoor or daylight); the rest
# correct it, since the cameras can't tell a dim room from a bright one.
LIGHTING = [("auto", "Measured by the cameras"), ("dim", "Dim: one lamp only"), ("room", "Normal room light"),
            ("daylight", "Daylight near a window")]
LIGHTING_TEXT = dict(LIGHTING[1:], indoor="Indoor light")
SLEEVES = [("short", "Short sleeves or bare arms"), ("long", "Long sleeves"), ("", "Rather not say")]
HANDEDNESS = [("", "Rather not say"), ("right", "Right-handed"), ("left", "Left-handed"),
              ("both", "Both (ambidextrous)")]
ACTIVE_STATES = ("starting", "intro", "ready", "countdown", "running", "paused", "between", "nohands")
# After Restart SteamVR: look at the cameras again this often, for at most this long.
RECHECK_S = 3
RECHECK_FOR_S = 120
# Shown side by side in the viewer at this height; thumbnails are smaller.
SET_HEIGHT = 480
THUMB_HEIGHT = 96


def read_text(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except OSError:
        return ""


def consent_version():
    """The "Version: ..." line of CONSENT.md: a new one asks everyone to agree again."""
    m = re.search(r"^Version:\s*(\S+)", read_text(CONSENT_PATH), re.M)
    return m.group(1) if m else "unknown"


def process_running(name):
    """Like pgrep -x NAME, from /proc."""
    for pid in os.listdir("/proc"):
        if pid.isdigit():
            try:
                with open(f"/proc/{pid}/comm") as f:
                    if f.read().strip() == name:
                        return True
            except OSError:
                pass
    return False


def headset_worn():
    """True while vrcompositor runs and any panel's backlight is on; False if unreadable."""
    lit = False
    try:
        for name in os.listdir(BACKLIGHTS):
            try:
                with open(os.path.join(BACKLIGHTS, name, "brightness")) as f:
                    lit = lit or int(f.read().strip()) > 0
            except (OSError, ValueError):
                pass
    except OSError:
        return False
    return lit and process_running("vrcompositor")


def gigabytes(n):
    return f"{n / 1000 ** 3:.1f} GB"


def session_label(sid):
    """20261002-101500 -> 2026-10-02 10:15; 20261002-101500-2 -> 2026-10-02 10:15 (2)."""
    stamp, _, n = sid[:15], sid[15:16], sid[16:]
    try:
        label = datetime.datetime.strptime(stamp, "%Y%m%d-%H%M%S").strftime("%Y-%m-%d %H:%M")
    except ValueError:
        return sid
    return f"{label} ({n})" if n else label


def grey_image(cam):
    """A camera's raw 8-bit pixels as a QImage that owns its data."""
    img = QImage(cam["pixels"], cam["width"], cam["height"], cam["width"], QImage.Format_Grayscale8)
    return img.copy()


class FrameProvider(QQuickImageProvider):
    """image://frames/set/SESSION/TAKE/INDEX: one frame set, every camera side by side.
    image://frames/thumb/SESSION/TAKE: the take's first slam_left image, small.
    Anything after "?" is ignored (it makes QML load the image again)."""

    def __init__(self, store):
        super().__init__(QQuickImageProvider.ImageType.Image)
        self.store = store

    def requestImage(self, ident, size, requested):
        parts = ident.split("?", 1)[0].split("/")
        try:
            if parts[0] == "thumb" and len(parts) == 3:
                return self._thumb(parts[1], parts[2])
            if parts[0] == "set" and len(parts) == 4:
                return self._set(parts[1], parts[2], int(parts[3]),
                                 requested.height() if requested.height() > 0 else SET_HEIGHT)
        except (OSError, ValueError, IndexError, KeyError) as e:
            print(f"ft-handrec: image {ident}: {e}", file=sys.stderr)
        img = QImage(4, 3, QImage.Format_Grayscale8)
        img.fill(0)
        return img

    def _index(self, session, take):
        return takes.take_index(self.store.take_dir(session, take))

    def _thumb(self, session, take):
        index = self._index(session, take)
        if not len(index):
            raise ValueError("no sets")
        names = [c["name"] for c in index.cams]
        cams = index.read_set(0, only="slam_left" if "slam_left" in names else names[0])
        return grey_image(cams[0]).scaledToHeight(THUMB_HEIGHT, Qt.SmoothTransformation)

    def _set(self, session, take, i, height):
        cams = self._index(session, take).read_set(i)
        scaled = [grey_image(c).scaledToHeight(height, Qt.SmoothTransformation) for c in cams]
        gap = 8
        out = QImage(sum(s.width() for s in scaled) + gap * (len(scaled) - 1), height, QImage.Format_RGB32)
        out.fill(QColor(30, 30, 30))
        p = QPainter(out)
        font = QFont()
        font.setPixelSize(max(12, height // 28))
        p.setFont(font)
        x = 0
        for cam, img in zip(cams, scaled):
            p.drawImage(x, 0, img)
            p.setPen(QColor(255, 200, 80))
            p.drawText(x + 6, 6 + font.pixelSize(), cam["name"])
            x += img.width() + gap
        p.end()
        return out


class Backend(QObject):
    profileChanged = Signal()
    diskChanged = Signal()
    lightingChanged = Signal()
    statusChanged = Signal()
    sessionsChanged = Signal()
    exportChanged = Signal()
    message = Signal(str, bool)  # text, is error
    # From other threads (the session runner, export, the lighting check): queued to this one.
    _statusArrived = Signal(dict)
    _lightingArrived = Signal(str, str)
    _exportProgress = Signal(float, str)
    _exportFinished = Signal(str, str)  # path, error ("" when it worked; "cancelled")
    loginChanged = Signal()
    uploadChanged = Signal()
    _loginArrived = Signal(dict)
    _loginLine = Signal(dict)
    _uploadLine = Signal(dict)
    _uploadFinished = Signal(dict)
    cameraChanged = Signal()
    _cameraArrived = Signal(dict)

    def __init__(self, store, session_options=None, hub_dry_run=False, ignore_cameras=False):
        super().__init__()
        self.store = store
        self._session_options = session_options or {}  # Session options: dry_run, speed, poses_dir (tests), button
        self._hub_dry_run = hub_dry_run
        self._login = {"state": "dry" if hub_dry_run else "unknown"}
        self._login_busy = False
        self._login_proc = None      # hub.py login: waiting for the browser
        self._login_cancelled = False
        self._upload_proc = None
        self._upload_thread = None
        self._upload_cancelled = False
        self._upload = {}
        os.makedirs(store.base, mode=0o700, exist_ok=True)
        self._session_mod = None
        self._session_error = ""
        self._session = None
        self._session_id = ""
        self._status = {}
        self._lighting_note = ""
        self._lighting_measured = ""
        self._lighting_busy = False
        self._camd_started = False   # ft-camd started for the light check: stopped on quit
        self._awake = set()          # what keeps the Frame awake now: "export", "upload"
        self._awake_lock = threading.Lock()
        self._export_cancel = None
        self._export_thread = None
        self._export_fraction = 0.0
        self._export_text = ""
        self._export_session = ""
        self._statusArrived.connect(self._on_status)
        self._ignore_cameras = ignore_cameras
        self._camera = {}            # the last camera check (session.camera_check)
        self._camera_busy = False
        self._restarting = False
        self._cameraArrived.connect(self._on_camera)
        self._lightingArrived.connect(self._on_lighting)
        self._exportProgress.connect(self._on_export_progress)
        self._exportFinished.connect(self._on_export_finished)
        self._loginArrived.connect(self._on_login)
        self._loginLine.connect(self._on_login_line)
        self._uploadLine.connect(self._on_upload_line)
        self._uploadFinished.connect(self._on_upload_finished)
        self.disk_timer = QTimer(interval=30000, timeout=self.diskChanged.emit)
        self.disk_timer.start()

    def _thread(self, fn):
        thread = threading.Thread(target=fn, daemon=True)
        thread.start()
        return thread

    # --- the session runner, imported when first needed (it's written separately)
    def _runner(self):
        if self._session_mod is None and not self._session_error:
            try:
                import session as mod
                self._session_mod = mod
            except Exception as e:  # missing, or broken: say so, the rest of the app still works
                self._session_error = f"The session runner (hands/rec/session.py) can't be loaded: {e}"
        return self._session_mod

    @Property(str, notify=statusChanged)
    def runnerError(self):
        self._runner()
        return self._session_error

    # --- consent and profile
    @Property(str, constant=True)
    def consentText(self):
        return read_text(CONSENT_PATH) or "CONSENT.md is missing."

    @Property(str, constant=True)
    def toolVersion(self):
        """What session.json records as the tool (takes.tool_version), for the window to show."""
        return takes.tool_version()

    @Property(str, constant=True)
    def consentVersion(self):
        return consent_version()

    @Property(bool, constant=True)
    def textsDraft(self):
        return hub.texts_draft()

    @Property(bool, notify=profileChanged)
    def needsConsent(self):
        profile = self.store.profile()
        consent = profile.get("consent") or {}
        return not (profile.get("contributor") and consent.get("adult") is True
                    and consent.get("version") == consent_version())

    @Property(str, notify=profileChanged)
    def contributor(self):
        return self.store.profile().get("contributor", "")

    @Property(str, notify=profileChanged)
    def consentAccepted(self):
        return (self.store.profile().get("consent") or {}).get("accepted", "")

    @Property(str, notify=profileChanged)
    def handedness(self):
        return (self.store.profile().get("optional") or {}).get("handedness", "")

    @Property("QVariantList", constant=True)
    def handednessChoices(self):
        return [{"value": k, "text": v} for k, v in HANDEDNESS]

    @Slot(bool, bool, bool, str)
    def acceptConsent(self, adult, region, agree, handedness):
        """Write profile.json. A contributor id, once made, stays (a new consent version keeps it).
        region: not living in Illinois, Texas or Washington (CONSENT.md "Who can take part")."""
        if not (adult and region and agree):
            self.message.emit("All three boxes need ticking to take part", True)
            return
        profile = self.store.profile()
        optional = profile.get("optional") if isinstance(profile.get("optional"), dict) else {}
        optional["handedness"] = handedness if handedness in dict(HANDEDNESS) else ""
        optional.setdefault("notes", "")
        profile = {"schema": 1, "contributor": profile.get("contributor") or str(uuid.uuid4()),
                   "consent": {"version": consent_version(),
                               "accepted": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                               "adult": True, "region_ok": True},
                   "optional": optional}
        takes.write_json(self.store.profile_path, profile)
        self.profileChanged.emit()
        self.message.emit("Thank you. Your contributor id: " + profile["contributor"], False)

    # --- the checklist
    @Property("QVariantList", constant=True)
    def objects(self):
        return [{"value": k, "text": v} for k, v in OBJECTS]

    @Property("QVariantList", constant=True)
    def lightingChoices(self):
        return [{"value": k, "text": v} for k, v in LIGHTING]

    @Property("QVariantList", constant=True)
    def sleeveChoices(self):
        return [{"value": k, "text": v} for k, v in SLEEVES]

    @Property(str, notify=diskChanged)
    def freeText(self):
        return gigabytes(takes.free_bytes(self.store.base))

    @Property(bool, notify=diskChanged)
    def diskOk(self):
        return takes.free_bytes(self.store.base) >= ROUND_BYTES

    @Property(str, notify=lightingChanged)
    def lightingNote(self):
        """A warning: this light is like an earlier round's, or it couldn't be measured."""
        return self._lighting_note

    @Property(str, notify=lightingChanged)
    def lightingMeasured(self):
        """What the cameras see: "Indoor light" or "Daylight…", "Measuring…", or ""."""
        return "Measuring…" if self._lighting_busy else self._lighting_measured

    @Slot(str)
    def checkLighting(self, chosen):
        """Measure the light (starting ft-camd if nothing runs it) and compare it with earlier
        rounds (session.similar_lighting): a round in light like an earlier one adds less."""
        mod = self._runner()
        if not mod or self._lighting_busy:
            return
        base = self.store.base
        self._lighting_busy = True
        self.lightingChanged.emit()

        def run():
            measured = ""
            try:
                ring = mod.ring_lighting()
                # A dry run starts nothing, the camera broker included.
                if ring is None and not self.sessionActive and not self._session_options.get("dry_run"):
                    if mod.start_camd():
                        self._camd_started = True
                    time.sleep(1.0)    # the first near-black frames
                    ring = mod.ring_lighting()
                if ring is None:
                    note = ("The cameras aren't running, so the light can't be measured now. The session "
                            "measures it when it starts.")
                else:
                    kind = mod.classify_lighting(ring)
                    ir = mod.ambient_ir(ring)
                    measured = (f"{LIGHTING_TEXT.get(kind, kind)} (infrared {ir:.1f})" if kind
                                else "The cameras couldn't measure the light yet.")
                    match = mod.similar_lighting(base, {"chosen": chosen, "ring": ring})
                    note = "" if not match else (
                        f"The cameras see about the same light as in your round of {session_label(match[0])} "
                        f"({LIGHTING_TEXT.get(match[1], match[1] or 'not given')}). A different light helps the "
                        "dataset more: change the lighting if you can, or go ahead anyway.")
            except Exception as e:
                note = f"Couldn't measure the light: {e}"
            self._lightingArrived.emit(note, measured)
        self._thread(run)

    def _on_lighting(self, note, measured):
        self._lighting_busy = False
        self._lighting_note = note
        self._lighting_measured = measured
        self.lightingChanged.emit()

    # --- the camera check (hands/camcheck.py through session.camera_check)
    @Property(str, notify=cameraChanged)
    def cameraState(self):
        """ok, degraded, unknown; checking while it runs; "" before the first check."""
        if self._camera_busy and not self._camera:
            return "checking"
        return self._camera.get("status", "")

    @Property(str, notify=cameraChanged)
    def cameraText(self):
        """What to tell the person when the cameras aren't all running ("" otherwise)."""
        mod = self._runner()
        return mod.camera_text(self._camera) if mod and self._camera else ""

    @Property(str, notify=cameraChanged)
    def cameraSummary(self):
        return self._camera.get("summary", "")

    @Property(str, notify=cameraChanged)
    def cameraEvidence(self):
        return "\n".join(self._camera.get("evidence", []))

    @Property(bool, constant=True)
    def camerasIgnored(self):
        return self._ignore_cameras

    @Property(bool, notify=cameraChanged)
    def cameraBusy(self):
        return self._camera_busy

    @Property(bool, notify=cameraChanged)
    def restartingSteamVR(self):
        return self._restarting

    @Property(bool, notify=cameraChanged)
    def camerasBlockStart(self):
        """Start stays off: the check found the cameras degraded (unless --ignore-cameras)."""
        return not self._ignore_cameras and self._camera.get("status") == "degraded"

    def _check_now(self, repair=False):
        """repair (off this thread only: it may restart ft-camd, up to 15 s): see session.camera_check."""
        mod = self._runner()
        if not mod or self._session_options.get("dry_run"):
            return {"status": "unknown", "summary": "not checked (dry run)", "reason": "dry run", "evidence": []}
        return mod.camera_check(repair=repair)

    @Slot()
    def checkCameras(self):
        """Run the camera check off this thread (it reads the XRService log: a few MB)."""
        if self._camera_busy:
            return
        self._camera_busy = True
        self.cameraChanged.emit()
        self._thread(lambda: self._cameraArrived.emit(self._check_now(repair=True)))

    def _on_camera(self, result):
        self._camera = result
        self._camera_busy = self._restarting
        self.cameraChanged.emit()

    @Slot()
    def restartSteamVR(self):
        """systemctl --user restart steamvr.service on the host (--no-block: the job runs in
        systemd, so it finishes even when this window closes with the Frametop desktop), then
        the camera check every few seconds until the new XRService has opened its cameras."""
        mod = self._runner()
        if not mod or self._restarting or self.sessionActive:
            return
        self._restarting = self._camera_busy = True
        self.cameraChanged.emit()
        before = self._camera.get("log", "") if self._camera else ""   # each XRService start writes a new log

        def run():
            result = None
            try:
                r = subprocess.run(mod.host_command("systemctl", "--user", "restart", "--no-block",
                                                    "steamvr.service"), capture_output=True, text=True, timeout=60)
                if r.returncode != 0:
                    raise RuntimeError((r.stderr or r.stdout).strip() or "exit %d" % r.returncode)
                end = time.monotonic() + RECHECK_FOR_S
                time.sleep(RECHECK_S * 2)
                while time.monotonic() < end:
                    result = self._check_now(repair=True)
                    # Done once a new XRService (a new log) has opened its cameras, ok or not.
                    if result.get("status") in ("ok", "degraded") and result.get("log") != before:
                        break
                    time.sleep(RECHECK_S)
            except Exception as e:
                result = {"status": "unknown", "summary": f"unknown: couldn't restart SteamVR ({e})",
                          "reason": str(e), "evidence": []}
            self._restarting = False
            self._cameraArrived.emit(result or self._check_now())
        self._thread(run)

    # --- the session
    @Property("QVariantMap", notify=statusChanged)
    def status(self):
        return self._status

    @Property(bool, notify=statusChanged)
    def sessionActive(self):
        return self._session is not None and self._status.get("state", "starting") in ACTIVE_STATES

    @Property(str, notify=statusChanged)
    def sessionId(self):
        return self._session_id

    @Slot("QVariantMap", bool, bool, result=str)
    def planText(self, checklist, auto, quick):
        """How long a session with these answers takes (session.plan_summary), or a quick round."""
        mod = self._runner()
        if not mod:
            return ""
        try:
            script = mod.load_script(SCRIPT_PATH)
            plan, _ = mod.build_plan(script, dict(checklist), quick=quick)
            return mod.plan_summary(script, plan, auto=auto)
        except Exception as e:
            return f"Couldn't read the script: {e}"

    @Property(bool, notify=sessionsChanged)
    def hasFullSession(self):
        """A full session (not a quick round, not a dry run) went to the end: the window then
        suggests a quick round, in another light."""
        for s in self.store.sessions():
            meta = takes.read_json(os.path.join(self.store.session_dir(s["id"]), "session.json"))
            if meta.get("status") == "done" and not meta.get("quick") and not meta.get("dry_run"):
                return True
        return False

    @Slot("QVariantMap", str, bool, bool, result=bool)
    def startSession(self, checklist, lighting, auto, quick):
        if self.sessionActive:
            return False
        mod = self._runner()
        if not mod:
            self.message.emit(self._session_error, True)
            return False
        if self.needsConsent:
            self.message.emit("Agree to the consent text first (Welcome page)", True)
            return False
        if not self._ignore_cameras and not self._session_options.get("dry_run"):
            self._camera = self._check_now()   # fresh: it takes a fraction of a second
            self.cameraChanged.emit()
            if self._camera.get("status") == "degraded":
                self.message.emit(mod.camera_text(self._camera), True)
                return False
        checklist = dict(checklist)
        checklist["objects"] = [o for o in checklist.get("objects", []) if o in dict(OBJECTS)]
        checklist["own_objects"] = [o.strip() for o in checklist.get("own_objects", []) if str(o).strip()]
        try:
            self._session = mod.Session(self.store.base, self.store.profile(), checklist, lighting, SCRIPT_PATH,
                                        on_status=lambda s: self._statusArrived.emit(dict(s)), auto=auto,
                                        quick=quick, **self._session_options)
        except Exception as e:
            self.message.emit(f"Couldn't set up the session: {e}", True)
            return False
        self._session_id = ""
        self._status = {"state": "starting"}
        self.statusChanged.emit()
        session = self._session

        def run():
            try:
                session.start()
            except Exception as e:
                self._statusArrived.emit({"state": "error", "error": str(e)})
        self._thread(run)
        return True

    def _on_status(self, status):
        self._status = status
        if status.get("camera") and status.get("camera") is not self._camera:
            self._camera = status["camera"]   # the session's own check (at its start, or after no hands)
            self.cameraChanged.emit()
        if self._session is not None and not self._session_id:
            self._session_id = os.path.basename(str(getattr(self._session, "session_dir", "") or ""))
        self.statusChanged.emit()
        if status.get("state") in ("done", "stopped", "error"):
            self.sessionsChanged.emit()

    def _control(self, name):
        if self._session is None:
            return
        try:
            getattr(self._session, name)()
        except Exception as e:
            self.message.emit(f"{name}: {e}", True)

    @Slot()
    def nextStep(self):
        if self.sessionActive and self._status.get("waiting"):
            self._control("next_step")

    @Slot()
    def redo(self):
        if self.sessionActive and self._status.get("can_redo"):
            self._control("redo")

    @Slot()
    def togglePause(self):
        if self._status.get("state") == "paused":
            self._control("resume")
        elif self.sessionActive:
            self._control("pause")

    @Slot()
    def skipSection(self):
        if self.sessionActive:
            self._control("skip")

    @Slot()
    def stopSession(self):
        """Stop: the take in progress is kept, as far as it got. It can block while the
        recording is written out, so it runs off this thread."""
        if self.sessionActive:
            session = self._session
            self._thread(lambda: session.stop())

    def shutdown(self):
        """The window closes: end a running session (blocking, so its files are complete)."""
        if self.sessionActive:
            try:
                self._session.stop()
            except Exception:
                pass
        if self._export_cancel:
            self._export_cancel.set()  # and wait, so export can remove its half-written copy
            self._export_thread.join(15)
        self.cancelLogin()
        if self._upload_proc:
            self.cancelUpload()
            self._upload_thread.join(10)
        if self._awake and self._session_mod:
            try:
                self._session_mod.stop_unit(AWAKE_UNIT)
            except Exception:
                pass
        if self._camd_started and self._session_mod:
            try:
                self._session_mod.stop_unit(self._session_mod.CAMD_UNIT)
            except Exception:
                pass

    # --- review
    @Property("QVariantList", notify=sessionsChanged)
    def sessions(self):
        out = []
        for s in self.store.sessions():
            s["label"] = session_label(s["id"])
            s["sizeText"] = takes.human_bytes(s["bytes"])
            s["exportText"] = takes.human_bytes(s["export_bytes"]) if s["exported"] else ""
            s["lightingText"] = LIGHTING_TEXT.get(s["lighting"], "")
            s["active"] = self.sessionActive and s["id"] == self._session_id
            s["statusText"] = {"recording": "" if s["active"] else "interrupted", "error": "ended with an error",
                               "stopped": "stopped early"}.get(s["status"], "")
            out.append(s)
        return out

    @Slot()
    def refreshSessions(self):
        self.sessionsChanged.emit()
        self.diskChanged.emit()

    @Slot(str, result="QVariantList")
    def takeList(self, session):
        try:
            rows = self.store.takes(session)
        except ValueError:
            return []
        for t in rows:
            t["durationText"] = f"{int(t['duration_s'] // 60)}:{int(t['duration_s'] % 60):02d}"
            t["sizeText"] = takes.human_bytes(t["bytes"])
            t.pop("ranges")
        return rows

    @Slot(str, str, result="QVariantMap")
    def takeInfo(self, session, take):
        """For the viewer: {count, title, cams, ranges: [[first, last] set indexes]}."""
        try:
            index = takes.take_index(self.store.take_dir(session, take))
            ranges = self.store.ranges(session, take)
        except ValueError:
            return {"count": 0, "title": take, "cams": [], "ranges": []}
        marks = []
        for a, b in ranges:
            inside = [i for i in range(len(index)) if a <= index.time_ns(i) <= b]
            marks.append([inside[0], inside[-1]] if inside else [-1, -1])
        return {"count": len(index), "title": self.store.take_meta(session, take).get("title") or take,
                "cams": [c["name"] for c in index.cams], "ranges": marks}

    @Slot(str, str, int, result=str)
    def setTime(self, session, take, i):
        """Set i's time from the take's first set, m:ss.s."""
        try:
            index = takes.take_index(self.store.take_dir(session, take))
            t = (index.time_ns(i) - index.time_ns(0)) / 1e9
        except (ValueError, IndexError):
            return ""
        return f"{int(t // 60)}:{t % 60:04.1f}"

    def _active_guard(self, session):
        if self.sessionActive and session == self._session_id:
            self.message.emit("This session is still recording: stop it first", True)
            return True
        return False

    @Slot(str, str, int, int)
    def deleteRange(self, session, take, first, last):
        """Leave sets first..last (indexes, either order) out of the export."""
        if self._active_guard(session):
            return
        index = takes.take_index(self.store.take_dir(session, take))
        first, last = sorted((max(0, first), min(len(index) - 1, last)))
        if first > last:
            return
        self.store.delete_range(session, take, index.time_ns(first), index.time_ns(last))
        self.message.emit(f"Sets {first + 1} to {last + 1} won't be exported", False)
        self.sessionsChanged.emit()

    @Slot(str, str, int)
    def restoreRange(self, session, take, k):
        self.store.restore_range(session, take, k)
        self.message.emit("Range restored", False)
        self.sessionsChanged.emit()

    @Slot(str, str)
    def deleteTake(self, session, take):
        if self._active_guard(session):
            return
        try:
            self.store.delete_take(session, take)
            self.message.emit(f"Deleted {take}", False)
        except (OSError, ValueError) as e:
            self.message.emit(f"Couldn't delete {take}: {e}", True)
        self.refreshSessions()

    @Slot(str)
    def deleteSession(self, session):
        if self._active_guard(session):
            return
        try:
            self.store.delete_session(session)
            self.message.emit(f"Deleted the session of {session_label(session)}", False)
        except (OSError, ValueError) as e:
            self.message.emit(f"Couldn't delete the session: {e}", True)
        self.refreshSessions()

    # --- export
    @Slot(result=bool)
    def headsetWorn(self):
        return headset_worn()

    @Property(bool, constant=True)
    def zstdFound(self):
        return takes.find_zstd() is not None

    @Property(bool, notify=exportChanged)
    def exporting(self):
        return self._export_cancel is not None

    @Property(float, notify=exportChanged)
    def exportFraction(self):
        return self._export_fraction

    @Property(str, notify=exportChanged)
    def exportText(self):
        return self._export_text

    @Property(str, notify=exportChanged)
    def exportSessionId(self):
        return self._export_session

    def _stay_awake(self, why, on):
        """Hold off sleep while an export or upload runs, so the headset can be taken off and
        left plugged in: a host unit running systemd-inhibit, started with the first reason
        and stopped with the last."""
        mod = self._runner()
        if not mod or self._hub_dry_run:
            return

        def run():
            with self._awake_lock:
                had = bool(self._awake)
                (self._awake.add if on else self._awake.discard)(why)
                try:
                    if self._awake and not had:
                        mod.start_unit(AWAKE_UNIT, "keeps the Frame awake while exporting or uploading",
                                       ["systemd-inhibit", "--what=sleep:idle", "--mode=block",
                                        "--who=Frametop Hand Recorder", "--why=Exporting or uploading hand recordings",
                                        "sleep", "infinity"])
                    elif had and not self._awake:
                        mod.stop_unit(AWAKE_UNIT)
                except Exception as e:
                    print(f"keeping the Frame awake: {e}", file=sys.stderr, flush=True)
        self._thread(run)

    @Slot(str, bool)
    def exportSession(self, session, keep_notes):
        if self.exporting or self._active_guard(session):
            return
        cancel = threading.Event()
        self._export_cancel = cancel
        self._export_session = session
        self._export_fraction = 0.0
        self._export_text = "Starting"
        self.exportChanged.emit()
        self._stay_awake("export", True)

        def run():
            try:
                path = self.store.export(session, progress=lambda f, text: self._exportProgress.emit(f, text),
                                         cancel=cancel, keep_notes=keep_notes)
                self._exportFinished.emit(path, "")
            except takes.Cancelled:
                self._exportFinished.emit("", "cancelled")
            except Exception as e:
                self._exportFinished.emit("", str(e) or type(e).__name__)
        self._export_thread = self._thread(run)

    @Slot()
    def cancelExport(self):
        if self._export_cancel:
            self._export_cancel.set()
            self._export_text = "Cancelling"
            self.exportChanged.emit()

    def _on_export_progress(self, fraction, text):
        if self._export_cancel and not self._export_cancel.is_set():
            self._export_fraction, self._export_text = fraction, text
            self.exportChanged.emit()

    def _on_export_finished(self, path, error):
        self._export_cancel = None
        self._stay_awake("export", False)
        if error == "cancelled":
            self._export_text = "Cancelled: nothing was kept"
        elif error:
            self._export_text = "Failed: " + error
            self.message.emit("Export failed: " + error, True)
        else:
            self._export_fraction = 1.0
            self._export_text = f"Exported to {path} ({takes.human_bytes(takes.tree_bytes(path))})"
            self.message.emit("Export ready", False)
        self.exportChanged.emit()
        self.refreshSessions()

    @Slot(str)
    def deleteExport(self, session):
        try:
            self.store.delete_export(session)
            self.message.emit("Export deleted; the session stays", False)
        except (OSError, ValueError) as e:
            self.message.emit(f"Couldn't delete the export: {e}", True)
        self.refreshSessions()

    @Property(str, constant=True)
    def exportsDir(self):
        return self.store.exports_dir

    # --- upload
    @Property(str, constant=True)
    def dataset(self):
        try:
            return hub.dataset_id()
        except hub.HubError:
            return os.environ.get(hub.DATASET_ENV, HF_DATASET)

    @Property(str, constant=True)
    def datasetUrl(self):
        return hub.dataset_url(self.dataset)

    @Property(bool, constant=True)
    def hubDryRun(self):
        return self._hub_dry_run

    @Property(bool, constant=True)
    def uploadAllowed(self):
        """Real uploads may start: the texts aren't drafts, or FT_HANDREC_ALLOW_UPLOAD=1."""
        return hub.upload_allowed()

    @Property(bool, constant=True)
    def allowUploadSet(self):
        return os.environ.get(hub.ALLOW_ENV) == "1"

    def _hub_argv(self, *args):
        return [sys.executable, HUB_PATH, "--base", self.store.base, *args]

    # The login: hub.py whoami in a child process (it asks huggingface.co)
    @Property("QVariantMap", notify=loginChanged)
    def login(self):
        """{"state": unknown|checking|ok|none|read|error|missing|dry|starting|waiting, "name", "role",
        "text", "link"}; while waiting, "url" and "code" (the browser login's)."""
        return self._login

    @Slot()
    def checkLogin(self):
        if self._hub_dry_run or self._login_busy:
            return
        self._login_busy = True
        self._login = {"state": "checking", "text": "Checking your Hugging Face login"}
        self.loginChanged.emit()
        argv = self._hub_argv("whoami", "--json")

        def run():
            try:
                r = subprocess.run(argv, capture_output=True, text=True, timeout=60)
                result = self._last_json(r.stdout) or {"error": {"kind": "hub", "text": (r.stderr.strip().splitlines()
                                                                                          or ["hub.py failed"])[-1]}}
            except (OSError, subprocess.TimeoutExpired) as e:
                result = {"error": {"kind": "network", "text": f"The login check didn't finish: {e}"}}
            self._loginArrived.emit(result)
        self._thread(run)

    @staticmethod
    def _last_json(text):
        for line in reversed(text.strip().splitlines()):
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if isinstance(obj, dict):
                return obj
        return None

    def _on_login(self, result):
        self._login_busy = False
        self._login_proc = None
        if result.get("cancelled"):
            self._login = {"state": "unknown"}
            self.checkLogin()   # whatever was saved before is still there
            return
        if "done" in result:
            who = result["done"]
            role = who.get("role", "")
            if role == "read":
                self._login = {"state": "read", "name": who.get("name", ""), "role": role, "link": hub.TOKENS_URL,
                               "text": f"Logged in as {who.get('name', '')}, but with a read-only token, which "
                                       "can't upload. Log in again."}
            else:
                self._login = {"state": "ok", "name": who.get("name", ""), "role": role,
                               "text": f"Logged in as {who.get('name', '')}"}
        else:
            e = result.get("error") or {}
            state = {"login": "none", "missing": "missing"}.get(e.get("kind"), "error")
            self._login = {"state": state, "text": e.get("text", "The login check failed"), "link": e.get("link", "")}
        self.loginChanged.emit()

    # The browser login: hub.py login --json in a child process. It gets a link and a short code;
    # the link opens in the browser, the person types the code there (Hugging Face doesn't fill it
    # in, 2026-10-03) and approves, and hub.py saves the token. The token never reaches this process.
    @Slot()
    def logIn(self):
        if self._hub_dry_run or self._login_proc or self._login_busy:
            return
        try:
            proc = subprocess.Popen(self._hub_argv("login", "--json"), stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        except OSError as e:
            self._on_login({"error": {"kind": "hub", "text": f"Couldn't start the login: {e}"}})
            return
        self._login_proc = proc
        self._login_cancelled = False
        self._login_busy = True
        self._login = {"state": "starting", "text": "Getting a login link from Hugging Face"}
        self.loginChanged.emit()

        def run():
            last = {}
            for line in proc.stdout:
                try:
                    obj = json.loads(line)
                except ValueError:
                    continue
                if isinstance(obj, dict):
                    if "done" in obj or "error" in obj:
                        last = obj
                    else:
                        self._loginLine.emit(obj)
            err = proc.stderr.read()
            proc.wait()
            proc.stdout.close()
            proc.stderr.close()
            if self._login_cancelled:
                last = {"cancelled": True}
            elif not last:
                tail = (err.strip().splitlines() or [f"exit status {proc.returncode}"])[-1]
                last = {"error": {"kind": "hub", "text": f"The login stopped: {tail}"}}
            self._loginArrived.emit(last)
        self._thread(run)

    @Slot()
    def cancelLogin(self):
        if self._login_proc and self._login_proc.poll() is None:
            self._login_cancelled = True
            self._login_proc.terminate()

    def _on_login_line(self, obj):
        if obj.get("phase") == "code" and obj.get("url"):
            self._login = {"state": "waiting", "url": obj["url"], "code": obj.get("code", ""),
                           "expires_in": obj.get("expires_in", 0), "text": obj.get("text", "")}
            self.loginChanged.emit()
            QDesktopServices.openUrl(QUrl(obj["url"]))

    # The upload: hub.py upload --json in a child process; its lines say how it goes
    @Property(bool, notify=uploadChanged)
    def uploading(self):
        return self._upload_proc is not None

    @Property("QVariantMap", notify=uploadChanged)
    def upload(self):
        """{"session", "phase", "text", "fraction" (-1: unknown), "log", "result": {...}, "error": {...}}."""
        return self._upload

    @Slot(str, result="QVariantMap")
    def uploadInfo(self, session):
        """{"previous": the last upload of this same export or {}, "uploads": how many in all}."""
        try:
            export = self.store.export_dir(session)
            before = hub.previous_upload(self.store, session, hub.export_sha(export))
            count = len(hub.uploads(self.store, session))
        except (OSError, ValueError):
            return {"previous": {}, "uploads": 0}
        return {"previous": before or {}, "uploads": count}

    @Slot(str, bool)
    def startUpload(self, session, again):
        if self.uploading or self.exporting or self._active_guard(session):
            return
        if not self._hub_dry_run and not hub.upload_allowed():
            self.message.emit("Contributions aren't open yet", True)
            return
        argv = self._hub_argv("upload", session, "--json")
        if self._hub_dry_run:
            argv.append("--dry-run")
        if again:
            argv.append("--again")
        try:
            proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, bufsize=1)
        except OSError as e:
            self.message.emit(f"Couldn't start the upload: {e}", True)
            return
        self._upload_proc = proc
        self._upload_cancelled = False
        self._upload = {"session": session, "phase": "check", "text": "Starting", "fraction": 0.0, "log": "",
                        "result": {}, "error": {}, "pr_url": ""}
        self.uploadChanged.emit()
        self._stay_awake("upload", True)

        def run():
            last = {}
            for line in proc.stdout:
                try:
                    obj = json.loads(line)
                except ValueError:
                    continue
                if isinstance(obj, dict):
                    if "done" in obj or "error" in obj:
                        last = obj
                    else:
                        self._uploadLine.emit(obj)
            err = proc.stderr.read()
            proc.wait()
            proc.stdout.close()
            proc.stderr.close()
            if not last:
                tail = (err.strip().splitlines() or [f"exit status {proc.returncode}"])[-1]
                last = {"error": {"kind": "cancelled" if self._upload_cancelled else "hub",
                                  "text": "Cancelled" if self._upload_cancelled else f"The upload stopped: {tail}"}}
            self._uploadFinished.emit(last)
        self._upload_thread = self._thread(run)

    @Slot()
    def cancelUpload(self):
        if self._upload_proc and self._upload_proc.poll() is None:
            self._upload_cancelled = True
            self._upload_proc.terminate()
            self._upload = dict(self._upload, text="Cancelling")
            self.uploadChanged.emit()

    def _on_upload_line(self, obj):
        up = dict(self._upload)
        if "log" in obj:
            up["log"] = (up.get("log", "") + obj["log"] + "\n")[-20000:]
        if "phase" in obj:
            f = obj.get("fraction")
            up.update(phase=obj["phase"], text=obj.get("text", ""), fraction=-1.0 if f is None else float(f))
        if obj.get("pr_url"):
            up["pr_url"] = obj["pr_url"]   # the pull request is open: the files are on their way
        self._upload = up
        self.uploadChanged.emit()

    def _on_upload_finished(self, last):
        self._upload_proc = None
        self._stay_awake("upload", False)
        up = dict(self._upload)
        if "done" in last:
            result = last["done"]
            up.update(result=result, error={}, phase="done", fraction=1.0,
                      text="Dry run: nothing was uploaded" if result.get("dry_run") else "Uploaded")
            if not result.get("dry_run"):
                self.message.emit("Uploaded: the pull request is open", False)
        else:
            e = last.get("error") or {}
            cancelled = e.get("kind") == "cancelled"
            up.update(error=e, result={}, phase="failed", fraction=0.0,
                      text=("Cancelled. Its pull request stays open: press Upload again to finish it there."
                            if up.get("pr_url") else "Cancelled") if cancelled else e.get("text", ""))
            if not cancelled:
                self.message.emit("Upload failed", True)
        self._upload = up
        self.uploadChanged.emit()
        self.sessionsChanged.emit()

    @Slot(str, result=str)
    def uploadText(self, session):
        """UPLOAD.md with this export's path and size filled in."""
        try:
            path = self.store.export_dir(session)
        except ValueError:
            return ""
        values = {"EXPORT_PATH": path, "EXPORT_SIZE": takes.human_bytes(takes.tree_bytes(path)),
                  "CONTRIBUTOR": self.contributor or "CONTRIBUTOR", "SESSION": session, "DATASET": self.dataset}
        text = read_text(UPLOAD_PATH) or "UPLOAD.md is missing."
        if hub.is_draft(UPLOAD_PATH):
            text = text.split("\n", 1)[-1]   # the page's banner says it already
        return re.sub(r"@([A-Z_]+)@", lambda m: values.get(m.group(1), m.group(0)), text)

    @Slot(QColor)
    def setLinkColor(self, color):
        palette = QGuiApplication.palette()
        palette.setColor(QPalette.Link, color)
        QGuiApplication.setPalette(palette)

    @Slot(str)
    def copy(self, text):
        QGuiApplication.clipboard().setText(text)
        self.message.emit("Copied", False)


def main():
    ap = argparse.ArgumentParser(description="Frametop Hand Recorder")
    ap.add_argument("--base", default=takes.DEFAULT_BASE, help="where profile.json, sessions/ and exports/ go")
    ap.add_argument("--page", default="", help="open on this page: welcome, checklist, session, review, export, upload")
    ap.add_argument("--dry-run", action="store_true",
                    help="test: sessions start no processes and print the panel's commands")
    ap.add_argument("--speed", type=float, default=1.0, help="test, with --dry-run: run sessions this much faster")
    ap.add_argument("--poses", help="test: the pose pictures' folder (default hands/rec/poses)")
    ap.add_argument("--no-headset-button", action="store_true",
                    help="sessions don't read the headset's button (Next, pause, resume)")
    ap.add_argument("--hub-dry-run", action="store_true",
                    help="test: Upload checks the export and says what it would send, with no network calls")
    ap.add_argument("--ignore-cameras", action="store_true",
                    help="start sessions even if the camera check (hands/camcheck.py) finds the upper cameras off")
    ap.add_argument("--qml", default=os.path.join(HERE, "main.qml"),
                    help="the window (default: this folder's main.qml, which needs Kirigami)")
    ap.add_argument("--style", default="org.kde.desktop", help="the Qt Quick Controls style")
    a, qt_args = ap.parse_known_args()
    app = QGuiApplication([sys.argv[0]] + qt_args)
    app.setApplicationName("ft-handrec")
    app.setApplicationDisplayName("Frametop Hand Recorder")
    app.setDesktopFileName("ft-handrec")
    if not QIcon.themeName():
        QIcon.setThemeName("breeze")
    QQuickStyle.setStyle(a.style)
    store = takes.Store(a.base)
    engine = QQmlApplicationEngine()
    engine.addImageProvider("frames", FrameProvider(store))
    options = {"dry_run": True, "speed": a.speed} if a.dry_run else {}
    if a.poses:
        options["poses_dir"] = a.poses
    if a.no_headset_button:
        options["button"] = False
    if a.ignore_cameras:
        options["check_cameras"] = False
    backend = Backend(store, options, hub_dry_run=a.hub_dry_run, ignore_cameras=a.ignore_cameras)
    app.aboutToQuit.connect(backend.shutdown)
    engine.rootContext().setContextProperty("backend", backend)
    engine.rootContext().setContextProperty("startPage", a.page)
    engine.load(QUrl.fromLocalFile(os.path.abspath(a.qml)))
    if not engine.rootObjects():
        sys.exit(1)

    # SIGTERM and Ctrl+C quit as closing does, so a running session still stops cleanly. Python
    # runs signal handlers between bytecodes: the timer gives it some while Qt waits.
    def on_signal(*_):
        engine.rootObjects()[0].setProperty("quitting", True)
        app.quit()
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    tick = QTimer(interval=500, timeout=lambda: None)
    tick.start()
    code = app.exec()
    # The window goes before the backend: otherwise its bindings run again against a deleted
    # backend as Python tears down ("Cannot read property ... of null", one per binding).
    shiboken6.delete(engine)
    sys.exit(code)


if __name__ == "__main__":
    main()
