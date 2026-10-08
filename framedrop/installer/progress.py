#!/usr/bin/env python3
"""The FrameDrop installer's window: what to install, your password for the parts that need it,
then the install's progress.

Usage: progress.py DIR [--dry-run]
  DIR        the installer's folder: get.sh, askpass, and, in a release download, the release
             list frametop-releases.json (then it installs a release: get.sh --release)
  --dry-run  get the files into ~/.cache/frametop-framedrop/dry-run and stop there
             (get.sh --clone-only), for testing this flow

The install runs in a user service of its own (UNIT): Steam ends the title's whole process
tree when it's quit, and starts it with a high OOM score. Closing the window doesn't stop the
install; playing the title again reattaches to it.

Our eye tracker and the Bluetooth fixes need sudo. The window asks for your password on the
headset, checks it with sudo, and keeps it in this process's memory until the install ends.
sudo in the install gets it through askpass (SUDO_ASKPASS), from a socket in a folder only you
can open, and the window answers only programs in the install's service. It's never written to
a file, a log, the service's environment, or a command line.
"""
import os
import pwd
import re
import socket
import struct
import subprocess
import sys
import threading
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, GLib, Gtk, Pango  # noqa: E402

HERE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parent
DRY_RUN = "--dry-run" in sys.argv[2:]
UNIT = "frametop-framedrop-install"
HOME = Path.home()
STATE = HOME / ".cache" / "frametop-framedrop"
LOG = STATE / "install.log"
RELEASES = HERE / "frametop-releases.json"
SOCK_DIR = Path(f"/run/user/{os.getuid()}/frametop-install")
SOCK = SOCK_DIR / "askpass"
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# get.sh and install.sh mark each step with "== N/10 what it is"
STEP = re.compile(r"^== (\d+)/(\d+) (.*)$")
DONE_TEXT = ("Frametop is installed. Restart SteamVR once, or reboot the headset, so it loads "
             "Frametop's driver. Then open Launch a program, then Desktop.")


def unit_state():
    out = subprocess.run(["systemctl", "--user", "show", "-P", "ActiveState", UNIT],
                         capture_output=True, text=True).stdout.strip()
    return out or "inactive"


def password_set():
    """Does this user have a password sudo can take? SteamOS starts without one."""
    user = pwd.getpwuid(os.getuid()).pw_name
    out = subprocess.run(["passwd", "-S", user], capture_output=True, text=True).stdout.split()
    return len(out) < 2 or out[1] == "P"  # P: usable; NP: none; L: locked


def check_password(pw):
    """Does sudo take it? Checked with cached credentials ignored, and forgotten after."""
    try:
        r = subprocess.run(["sudo", "-S", "-k", "-v", "-p", ""], input=bytes(pw) + b"\n",
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return False
    subprocess.run(["sudo", "-k"], stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return r.returncode == 0


def in_unit(pid):
    """Is this process in the install's service?"""
    try:
        with open(f"/proc/{pid}/cgroup") as f:
            return any(line.rstrip("\n").endswith(f"/{UNIT}.service") for line in f)
    except OSError:
        return False


def get_args(eye_tracker, bluetooth):
    args = ["--yes"]
    if RELEASES.exists():
        args = ["--release", "--manifest", str(RELEASES)] + args
    if DRY_RUN:
        args += ["--clone-only", "--dir", str(STATE / "dry-run")]
    if not eye_tracker:
        args.append("--no-eye-tracker")
    if bluetooth:
        args.append("--bluetooth")
    return args


def start_unit(args, askpass):
    subprocess.run(["systemctl", "--user", "stop", UNIT], stderr=subprocess.DEVNULL)
    subprocess.run(["systemctl", "--user", "reset-failed", UNIT], stderr=subprocess.DEVNULL)
    STATE.mkdir(parents=True, exist_ok=True)
    LOG.write_bytes(b"")
    cmd = ["systemd-run", "--user", f"--unit={UNIT}", "--description=Frametop install (FrameDrop)",
           "--property=Type=oneshot", "--property=RemainAfterExit=yes",
           f"--property=StandardOutput=truncate:{LOG}", "--property=StandardError=inherit",
           f"--setenv=HOME={HOME}", f"--setenv=PATH={os.environ.get('PATH', '/usr/bin:/bin')}",
           "--setenv=TERM=dumb", f"--working-directory={HOME}", "--quiet", "--no-block"]
    if askpass:
        cmd += [f"--setenv=SUDO_ASKPASS={HERE / 'askpass'}", f"--setenv=FRAMETOP_ASKPASS_SOCKET={SOCK}"]
    cmd += ["/usr/bin/bash", str(HERE / "get.sh")] + args
    return subprocess.run(cmd).returncode == 0


class Askpass:
    """The socket askpass asks: answers programs in the install's service with the password,
    or holds them while the window asks you for it."""

    def __init__(self, on_ask):
        self.on_ask = on_ask
        self.password = None  # bytearray, while the install runs
        self.waiting = []
        SOCK_DIR.mkdir(mode=0o700, exist_ok=True)
        st = SOCK_DIR.stat()
        if st.st_uid != os.getuid():
            raise OSError(f"{SOCK_DIR} isn't yours")
        os.chmod(SOCK_DIR, 0o700)
        SOCK.unlink(missing_ok=True)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(str(SOCK))
        self.sock.listen(4)
        self.sock.setblocking(False)
        self.watch = GLib.io_add_watch(GLib.IOChannel.unix_new(self.sock.fileno()), GLib.PRIORITY_DEFAULT,
                                       GLib.IO_IN, self.accept)

    def accept(self, *_):
        try:
            conn, _ = self.sock.accept()
        except OSError:
            return True
        pid, uid, _ = struct.unpack("3i", conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED,
                                                          struct.calcsize("3i")))
        if uid != os.getuid() or not in_unit(pid):
            conn.close()
            return True
        if self.password:
            self.answer(conn)
        else:
            self.waiting.append(conn)
            self.on_ask()
        return True

    def answer(self, conn):
        try:
            conn.setblocking(True)
            conn.sendall(bytes(self.password) if self.password else b"")
        except OSError:
            pass
        conn.close()

    def give(self, pw):
        self.password = pw
        while self.waiting:
            self.answer(self.waiting.pop())

    def refuse(self):
        while self.waiting:
            self.waiting.pop().close()

    def close(self):
        if self.password:
            self.password[:] = bytes(len(self.password))
        self.password = None
        self.refuse()
        GLib.source_remove(self.watch)
        self.sock.close()
        SOCK.unlink(missing_ok=True)


class Window(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="Frametop")
        self.set_default_size(900, 640)
        self.offset = 0
        self.partial = ""
        self.askpass = None
        self.checking = False

        self.status = Gtk.Label(label="Install Frametop", xalign=0, wrap=True)
        self.status.add_css_class("title-2")
        self.detail = Gtk.Label(xalign=0, wrap=True)
        self.box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12,
                           margin_top=18, margin_bottom=18, margin_start=18, margin_end=18)
        self.box.append(self.status)
        self.box.append(self.detail)
        view = Adw.ToolbarView(content=self.box)
        view.add_top_bar(Adw.HeaderBar())
        self.set_content(view)
        self.connect("close-request", self.closing)

        if unit_state() == "activating":
            self.show_progress()  # playing the title again: the install is still going
        else:
            self.show_choices()

    # --- what to install, and the password

    def show_choices(self):
        what = "the newest release that fits this SteamOS" if RELEASES.exists() else "Frametop from GitHub"
        self.detail.set_label(f"This installs {what}: the multi-screen desktop, the 3D mouse, gaze "
                              "mode, and Frametop's settings apps. Two optional parts need your "
                              "SteamOS password (sudo):")
        self.eye = Gtk.CheckButton(label="Our own eye tracker for gaze mode (more accurate than SteamVR's)",
                                   active=True)
        self.bt = Gtk.CheckButton(label="Bluetooth fixes (LE mice and keyboards, like the Swiftpoint Z3, "
                                        "reconnect after they sleep)")
        self.pw = Gtk.PasswordEntry(show_peek_icon=True, placeholder_text="Your SteamOS password")
        pw_note = Gtk.Label(label="It's used only for this install, and isn't saved anywhere.", xalign=0,
                            wrap=True)
        pw_note.add_css_class("dim-label")
        self.error = Gtk.Label(xalign=0, wrap=True, visible=False)
        self.error.add_css_class("error")
        self.go = Gtk.Button(label="Install", halign=Gtk.Align.END)
        self.go.add_css_class("suggested-action")
        self.go.connect("clicked", self.install)
        self.pw.connect("activate", self.install)
        self.choices = [self.eye, self.bt, self.pw, pw_note, self.error, self.go]
        if not password_set():
            for c in (self.eye, self.bt):
                c.set_active(False)
                c.set_sensitive(False)
            self.pw.set_visible(False)
            pw_note.set_label("Your user has no password, so sudo can't run and these two can't be "
                              "installed from here. To set one, run passwd in Konsole; then play "
                              "Frametop again, or install them later from a terminal "
                              "(gaze/tracker/install.sh, setup/bluetooth/install.sh).")
        for c in (self.eye, self.bt):
            c.connect("toggled", lambda *_: self.pw.set_sensitive(self.eye.get_active() or self.bt.get_active()))
        for w in self.choices:
            self.box.append(w)

    def install(self, *_):
        if self.checking:
            return
        needs = self.eye.get_active() or self.bt.get_active()
        if not needs:
            return self.begin(None)
        text = self.pw.get_text()
        if not text:
            return self.say("Type your password, or untick the parts that need it.")
        pw = bytearray(text.encode())
        self.pw.set_text("")
        self.checking = True
        self.go.set_sensitive(False)
        self.say("Checking the password...")

        def check():
            ok = check_password(pw)
            GLib.idle_add(checked, ok)

        def checked(ok):
            self.checking = False
            self.go.set_sensitive(True)
            if ok:
                self.begin(pw)
            else:
                pw[:] = bytes(len(pw))
                self.say("sudo didn't take that password. Try again, or untick the parts that need it.")
            return False

        threading.Thread(target=check, daemon=True).start()

    def say(self, text):
        self.error.set_label(text)
        self.error.set_visible(True)

    def begin(self, pw):
        args = get_args(self.eye.get_active(), self.bt.get_active())
        if pw is not None:
            try:
                self.askpass = Askpass(self.ask_again)
            except OSError as e:
                pw[:] = bytes(len(pw))
                return self.say(f"Couldn't make the password's socket: {e}")
            self.askpass.give(pw)
        if not start_unit(args, pw is not None):
            if self.askpass:
                self.askpass.close()
                self.askpass = None
            return self.say("Couldn't start the install service (systemd-run).")
        for w in self.choices:
            self.box.remove(w)
        self.show_progress()

    # --- progress

    def show_progress(self):
        self.status.set_label("Installing Frametop")
        self.detail.set_label("Starting...")
        self.bar = Gtk.ProgressBar()
        self.bar.pulse()
        self.text = Gtk.TextView(editable=False, cursor_visible=False, monospace=True,
                                 wrap_mode=Pango.WrapMode.WORD_CHAR)
        self.text.set_left_margin(8)
        self.text.set_right_margin(8)
        scroll = Gtk.ScrolledWindow(vexpand=True, child=self.text)

        # The install wants the password and the window doesn't have it (played again).
        self.ask_bar = Gtk.Box(spacing=8, visible=False)
        self.ask_pw = Gtk.PasswordEntry(show_peek_icon=True, hexpand=True,
                                        placeholder_text="The next step needs your SteamOS password")
        ok = Gtk.Button(label="OK")
        skip = Gtk.Button(label="Skip that part")
        ok.connect("clicked", self.answer_ask)
        self.ask_pw.connect("activate", self.answer_ask)
        skip.connect("clicked", self.skip_ask)
        for w in (self.ask_pw, ok, skip):
            self.ask_bar.append(w)
        self.ask_error = Gtk.Label(xalign=0, wrap=True, visible=False)
        self.ask_error.add_css_class("error")

        keep = " Keep it open until the install is done: it gives the steps that need it your password." \
            if self.askpass else ""
        self.note = Gtk.Label(label="You can close this window: the install keeps going. Play Frametop "
                                    "again to come back to it." + keep, xalign=0, wrap=True)
        self.note.add_css_class("dim-label")
        close = Gtk.Button(label="Close", halign=Gtk.Align.END)
        close.connect("clicked", lambda *_: self.close())
        for w in (self.bar, scroll, self.ask_bar, self.ask_error, self.note, close):
            self.box.append(w)

        if self.askpass is None:
            try:
                self.askpass = Askpass(self.ask_again)
            except OSError:
                self.askpass = None  # askpass then fails, and install.sh skips those parts
        GLib.timeout_add(500, self.tick)
        self.tick()

    def ask_again(self):
        if hasattr(self, "ask_bar"):
            self.ask_bar.set_visible(True)
            self.ask_pw.grab_focus()

    def answer_ask(self, *_):
        text = self.ask_pw.get_text()
        if not text or self.checking:
            return
        pw = bytearray(text.encode())
        self.ask_pw.set_text("")
        self.checking = True

        def check():
            GLib.idle_add(checked, check_password(pw))

        def checked(ok):
            self.checking = False
            if ok and self.askpass:
                self.askpass.give(pw)
                self.ask_bar.set_visible(False)
                self.ask_error.set_visible(False)
            else:
                pw[:] = bytes(len(pw))
                self.ask_error.set_label("sudo didn't take that password.")
                self.ask_error.set_visible(True)
            return False

        threading.Thread(target=check, daemon=True).start()

    def skip_ask(self, *_):
        if self.askpass:
            self.askpass.refuse()
        self.ask_bar.set_visible(False)
        self.ask_error.set_visible(False)

    def add_lines(self, lines):
        buf = self.text.get_buffer()
        for line in lines:
            m = STEP.match(line)
            if m:
                n, total, what = int(m[1]), int(m[2]), m[3]
                self.bar.set_fraction(max(0, n - 1) / total)
                self.detail.set_label(f"Step {max(n, 1)} of {total}: {what}")
            buf.insert(buf.get_end_iter(), line + "\n")
        # Keep the view at the newest line.
        end = buf.create_mark(None, buf.get_end_iter(), False)
        self.text.scroll_mark_onscreen(end)
        buf.delete_mark(end)

    def tick(self):
        try:
            with open(LOG, "rb") as f:
                f.seek(self.offset)
                data = f.read()
                self.offset += len(data)
        except FileNotFoundError:
            data = b""
        if data:
            text = self.partial + ANSI.sub("", data.decode("utf-8", "replace")).replace("\r", "\n")
            *lines, self.partial = text.split("\n")
            self.add_lines(lines)

        state = unit_state()
        if state == "activating":
            if self.bar.get_fraction() == 0:
                self.bar.pulse()
            return True
        if self.partial:
            self.add_lines([self.partial])
            self.partial = ""
        self.forget()
        self.note.set_visible(False)
        self.ask_bar.set_visible(False)
        if state == "active":
            self.status.set_label("Done")
            self.detail.set_label(DONE_TEXT)
            self.bar.set_fraction(1)
        else:
            self.status.set_label("The install stopped")
            self.detail.set_label("The log above says why. Play Frametop again to try again.")
        return False

    def forget(self):
        if self.askpass:
            self.askpass.close()
            self.askpass = None

    def closing(self, *_):
        self.forget()
        return False


def main():
    app = Adw.Application(application_id="io.github.deejanuz.FrametopInstall")

    def activate(a):
        # Played again while the window is open: show that one.
        win = a.get_active_window() or Window(a)
        win.present()

    app.connect("activate", activate)
    app.run([])


if __name__ == "__main__":
    main()
