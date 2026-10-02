"""Offline physical mouse tests: no input devices, desktop or SteamVR accessed."""
import importlib.machinery
import importlib.util
import math
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch


def module(name, file):
    loader = importlib.machinery.SourceFileLoader(name, str(Path(__file__).with_name(file)))
    spec = importlib.util.spec_from_loader(name, loader)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


relay = module('desktop_relay', 'input-relay.py')
ctl = module('mouse_control', 'ft-mousectl')


class DesktopMouseTests(unittest.TestCase):
    def test_presence_requires_a_routed_grabbed_physical_mouse(self):
        def node(mouse=True, role='pointer', grabbed=True):
            return SimpleNamespace(is_mouse=mouse, role=role, grabbed=grabbed)
        self.assertFalse(relay.desktop_mouse_present([]))
        for n in (node(mouse=False), node(role='ignore'), node(role='passthrough'), node(grabbed=False)):
            self.assertFalse(relay.desktop_mouse_present([n]))
        self.assertTrue(relay.desktop_mouse_present([node(mouse=False), node()]))
    def mouse(self, speed=1):
        with patch.object(relay.socket, 'socket'):
            m = relay.DesktopMouse(speed)
        m.messages = []
        m.send = m.messages.append
        return m

    def test_motion_precedes_click_at_new_location(self):
        m = self.mouse(.5)
        m.motion(relay.REL_X, 12)
        m.motion(relay.REL_Y, -8)
        m.button(272, 1)
        m.button(272, 0)
        m.flush()
        self.assertEqual(m.messages, ['mouse-move 6.000000 -4.000000', 'mouse-button 272 1', 'mouse-button 272 0'])

    def test_hires_replaces_companion_legacy_in_both_orders(self):
        for events in ([(8, 1), (11, 120)], [(11, 120), (8, 1)]):
            m = self.mouse()
            for code, value in events: m.motion(code, value)
            m.flush()
            self.assertEqual(m.messages, ['mouse-wheel 0.000000 1.000000'])

    def test_fractional_scroll_has_no_pulse_timer(self):
        m = self.mouse()
        for value in (15, 15, -30):
            m.motion(11, value); m.flush()
        self.assertEqual(m.messages, ['mouse-wheel 0.000000 0.125000', 'mouse-wheel 0.000000 0.125000', 'mouse-wheel 0.000000 -0.250000'])

    def test_thumb_and_main_wheels_are_independent(self):
        m = self.mouse()
        m.motion(6, -2); m.motion(8, 3); m.motion(11, 60); m.flush()
        self.assertEqual(m.messages, ['mouse-wheel -2.000000 0.500000'])
        m.motion(8, -1); m.flush()
        self.assertEqual(m.messages[-1], 'mouse-wheel 0.000000 -1.000000')

    def test_hotplug_releases_all_buttons_without_stale_motion(self):
        m = self.mouse()
        m.button(272, 1); m.button(275, 1)
        m.motion(0, 20); m.motion(8, 1)
        m.release_all(); m.flush()
        self.assertEqual(m.messages, ['mouse-button 272 1', 'mouse-button 275 1', 'mouse-button 272 0', 'mouse-button 275 0'])
        self.assertFalse(m.held)

    def test_button_repeat_is_ignored(self):
        m = self.mouse()
        m.button(272, 2); m.button(280, 1)
        self.assertEqual(m.messages, [])

    def test_native_messages_never_target_virtual_controller(self):
        with patch.object(relay.socket, 'socket') as sock:
            m = relay.DesktopMouse()
            m.motion(0, 3); m.motion(8, 1); m.button(272, 1); m.release_all()
            self.assertTrue(sock.return_value.sendto.call_args_list)
            for call in sock.return_value.sendto.call_args_list:
                self.assertEqual(call.args[1], relay.SCREENS)

    def test_bad_speed_rejected_before_socket_created(self):
        with patch.object(relay.socket, 'socket') as sock:
            for speed in (math.nan, math.inf, 0, 11):
                with self.assertRaises(ValueError): relay.DesktopMouse(speed)
            sock.assert_not_called()


class ControlTests(unittest.TestCase):
    def test_old_components_cannot_trigger_partial_activation(self):
        with patch.object(ctl, 'status', return_value={'ready': False}), patch.object(ctl, 'configure') as configure, patch.object(ctl, 'save_mode') as save:
            with self.assertRaisesRegex(RuntimeError, 'not loaded'): ctl.switch('desktop', True)
            configure.assert_not_called(); save.assert_not_called()

    def test_rejected_switch_does_not_persist(self):
        with patch.object(ctl, 'require_ready'), patch.object(ctl, 'layout_outputs'), patch.object(ctl, 'configure'), patch.object(ctl, 'query', return_value={'ok': False, 'error': 'held buttons'}), patch.object(ctl, 'save_mode') as save:
            with self.assertRaisesRegex(RuntimeError, 'held buttons'): ctl.switch('desktop', True)
            save.assert_not_called()

    def test_spatial_recovery_does_not_need_kde(self):
        with patch.object(ctl, 'layout_outputs') as layout, patch.object(ctl, 'query', return_value={'ok': True}) as query:
            self.assertEqual(ctl.switch('spatial'), {'mode': 'spatial', 'saved': False})
            layout.assert_not_called(); query.assert_called_once_with(ctl.RELAY, 'mouse-mode spatial')

    def test_failed_persistence_reports_actual_live_mode(self):
        with patch.object(ctl, 'query', return_value={'ok': True}), patch.object(ctl, 'save_mode', side_effect=OSError('read only')):
            with self.assertRaisesRegex(RuntimeError, 'Live mode is spatial'): ctl.switch('spatial', True)

    def test_scaled_outputs_use_actual_logical_geometry(self):
        outs = [{'enabled': True, 'pos': {'x': -1280, 'y': 20}, 'size': {'width': 1920, 'height': 1080}, 'scale': 1.5}, {'enabled': False}]
        with patch.object(ctl, 'request', return_value='ok') as request:
            ctl.configure(outs)
            self.assertEqual([c.args for c in request.call_args_list], [(ctl.SCREENS, 'mouse-layout 1 -1280 20 1280 720 1.5'), (ctl.SCREENS, 'mouse-output-off 2')])

    def test_invalid_later_output_does_not_partially_change_geometry(self):
        outs = [{'enabled': True, 'pos': {'x': 0, 'y': 0}, 'size': {'width': 1920, 'height': 1080}, 'scale': 1}, {'enabled': True, 'scale': 0}]
        with patch.object(ctl, 'request') as request:
            with self.assertRaises(ValueError): ctl.configure(outs)
            request.assert_not_called()

    def test_save_preserves_other_preferences_and_permissions(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'frametop.conf'
            path.write_text('# user settings\nPOINTER=1\nMOUSE_MODE=spatial\nMETA_DASHBOARD=0\n')
            path.chmod(0o600)
            ctl.save_mode('desktop', path)
            self.assertEqual(path.read_text(), '# user settings\nPOINTER=1\nMOUSE_MODE=desktop\nMETA_DASHBOARD=0\n')
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_failed_atomic_save_preserves_old_file(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'frametop.conf'
            path.write_text('MOUSE_MODE=spatial\n')
            with patch.object(ctl.os, 'replace', side_effect=OSError('failed')):
                with self.assertRaises(OSError): ctl.save_mode('desktop', path)
            self.assertEqual(path.read_text(), 'MOUSE_MODE=spatial\n')
            self.assertEqual(list(Path(root).iterdir()), [path])


if __name__ == '__main__': unittest.main()
