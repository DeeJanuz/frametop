#!/usr/bin/env python3
"""Progress window for the FrameDrop installer: follows the install service's log and state.

Usage: progress.py UNIT LOG

Closing the window doesn't stop the install; it keeps running in the user service UNIT.
"""
import re
import subprocess
import sys

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, GLib, Gtk, Pango  # noqa: E402

UNIT, LOG = sys.argv[1], sys.argv[2]
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# install.sh marks each step with "== N/10 what it is"
STEP = re.compile(r"^== (\d+)/(\d+) (.*)$")
DONE_TEXT = ("Frametop is installed. Restart SteamVR once, or reboot the headset, so it loads "
             "Frametop's driver. Then open Launch a program, then Desktop.")


def unit_state():
    out = subprocess.run(
        ["systemctl", "--user", "show", "-P", "ActiveState", UNIT],
        capture_output=True, text=True).stdout.strip()
    return out or "inactive"


class Window(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="Frametop")
        self.set_default_size(900, 600)
        self.offset = 0
        self.partial = ""

        self.status = Gtk.Label(label="Installing Frametop", xalign=0, wrap=True)
        self.status.add_css_class("title-2")
        self.detail = Gtk.Label(label="Starting...", xalign=0, wrap=True)
        self.bar = Gtk.ProgressBar()
        self.bar.pulse()

        self.text = Gtk.TextView(editable=False, cursor_visible=False, monospace=True,
                                 wrap_mode=Pango.WrapMode.WORD_CHAR)
        self.text.set_left_margin(8)
        self.text.set_right_margin(8)
        scroll = Gtk.ScrolledWindow(vexpand=True, child=self.text)
        self.scroll = scroll

        self.note = note = Gtk.Label(
            label="You can close this window: the install keeps going. Play Frametop again to "
                  "come back to it.", xalign=0, wrap=True)
        note.add_css_class("dim-label")
        close = Gtk.Button(label="Close", halign=Gtk.Align.END)
        close.connect("clicked", lambda *_: self.close())

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12,
                      margin_top=18, margin_bottom=18, margin_start=18, margin_end=18)
        for w in (self.status, self.detail, self.bar, scroll, note, close):
            box.append(w)
        view = Adw.ToolbarView(content=box)
        view.add_top_bar(Adw.HeaderBar())
        self.set_content(view)

        self.finished = False
        GLib.timeout_add(500, self.tick)
        self.tick()

    def add_lines(self, lines):
        buf = self.text.get_buffer()
        for line in lines:
            m = STEP.match(line)
            if m:
                n, total, what = int(m[1]), int(m[2]), m[3]
                self.bar.set_fraction((n - 1) / total)
                self.detail.set_label(f"Step {n} of {total}: {what}")
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
        self.note.set_visible(False)
        if state == "active":
            self.status.set_label("Done")
            self.detail.set_label(DONE_TEXT)
            self.bar.set_fraction(1)
        else:
            self.status.set_label("The install stopped")
            self.detail.set_label("The log above says why. Play Frametop again to try again.")
        return False


def main():
    app = Adw.Application(application_id="io.github.deejanuz.FrametopInstall")
    app.connect("activate", lambda a: Window(a).present())
    app.run([])


if __name__ == "__main__":
    main()
