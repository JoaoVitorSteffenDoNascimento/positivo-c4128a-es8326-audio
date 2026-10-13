# SPDX-License-Identifier: BSD-3-Clause
import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import corrigir_audio as audio
from alsa_controles import Alsa


CARD = {"name": audio.CARD, "index": 0}
SINK = {"name": "alsa_output.internal.analog-stereo", "index": 10, "card": 0,
        "active_port": "analog-output-headphones", "mute": True,
        "volume": {"front-left": {"value": 12345}, "front-right": {"value": 23456}},
        "properties": {"alsa.device": "0"},
        "ports": [{"name": "analog-output-speaker", "availability": "not available"},
                  {"name": "analog-output-headphones", "availability": "available"}]}
CONTROLS = [{"name": "Headphone Switch", "numid": 1, "type": 1, "writable": True, "values": [1]},
            {"name": "Speaker Switch", "numid": 2, "type": 1, "writable": True, "values": [0]},
            {"name": "Headphone Jack", "numid": 3, "type": 1, "writable": False, "values": [1]}]


class FakeAlsa:
    def __init__(self, controls):
        self.items = controls
    def __enter__(self):
        return self
    def __exit__(self, *_):
        pass
    def controls(self):
        return copy.deepcopy(self.items)
    def write(self, control, values):
        assert "Jack" not in control["name"]
        current = next(x for x in self.items if x["numid"] == control["numid"])
        current["values"] = values[:]
        return copy.deepcopy(current)


class AudioTests(unittest.TestCase):
    def test_no_session_never_changes_audio(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(audio, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "Sem acesso"):
                audio.repair({"cards_error": "blocked"}, Path(folder))
            run.assert_not_called()

    def test_another_machine_never_changes_audio(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(audio, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "placa interna"):
                audio.repair({"cards": [{"index": 0, "name": "alsa_card.usb-headset"}], "sinks": [SINK]}, Path(folder))
            run.assert_not_called()

    def test_hdmi_and_usb_are_not_selected(self):
        others = [{"card": 0, "name": "alsa_output.internal.hdmi-stereo", "properties": {"alsa.device": "5"}},
                  {"card": 1, "name": "alsa_output.usb.analog-stereo", "properties": {"alsa.device": "0"}}]
        self.assertEqual(audio.find_sink(CARD, others + [SINK])["name"], SINK["name"])
        with self.assertRaises(RuntimeError):
            audio.find_sink(CARD, others)

    def test_selection_keeps_jack_state_and_saves_original(self):
        controls = copy.deepcopy(CONTROLS)
        state = {"cards": [CARD], "sinks": [copy.deepcopy(SINK)], "default_sink": "old-output", "alsa": controls}
        after = copy.deepcopy(state)
        after["sinks"][0].update(active_port="analog-output-speaker", mute=False)
        with tempfile.TemporaryDirectory() as folder, patch.object(audio, "run") as run, \
                patch.object(audio, "Alsa", side_effect=lambda _: FakeAlsa(controls)), \
                patch.object(audio, "snapshot", return_value=after), \
                patch.object(audio, "playback_test") as playback, patch.object(audio.time, "sleep"):
            audio.repair(state, Path(folder))
            backup = json.loads((Path(folder) / "backup.json").read_text())
            self.assertEqual(backup["sink"]["active_port"], "analog-output-headphones")
            self.assertEqual(controls[2]["values"], [1])
            self.assertEqual(controls[1]["values"], [1])
            self.assertEqual(len(backup["changed_controls"]), 2)
            playback.assert_called_once()
            self.assertTrue(all("sudo" not in c.args[0] for c in run.call_args_list))

    def test_failure_after_selection_requests_rollback(self):
        state = {"cards": [CARD], "sinks": [SINK], "default_sink": "old-output"}
        with tempfile.TemporaryDirectory() as folder, patch.object(audio, "run"), \
                patch.object(audio, "Alsa", side_effect=RuntimeError("controle indisponível")), \
                patch.object(audio, "restore_backup") as restore:
            with self.assertRaisesRegex(RuntimeError, "controle indisponível"):
                audio.repair(state, Path(folder))
            restore.assert_called_once_with(Path(folder) / "backup.json")

    def test_native_writer_rejects_readonly_jack(self):
        alsa = Alsa.__new__(Alsa)
        control = CONTROLS[2]
        with patch.object(alsa, "read", return_value=control):
            with self.assertRaisesRegex(RuntimeError, "não gravável"):
                alsa.write(control, [0])

    def test_native_writer_rejects_excessive_volume(self):
        alsa = Alsa.__new__(Alsa)
        control = {"name": "DAC Playback Volume", "numid": 9, "type": 2,
                   "writable": True, "min": 0, "max": 191, "values": [100]}
        with patch.object(alsa, "read", return_value=control):
            with self.assertRaisesRegex(ValueError, "fora do intervalo"):
                alsa.write(control, [192])

    def test_backup_restores_both_channel_volumes(self):
        self.assertEqual(audio.volumes(SINK), ["12345", "23456"])

    def test_connector_inversion_is_distinguished_from_stuck_signal(self):
        phases = ("sem-fone", "com-fone", "sem-fone-novamente")
        def report(values):
            return {phase: {"Headphone Jack": [value]} for phase, value in zip(phases, values)}
        self.assertIn("normalmente", audio.classify_connector(report([0, 1, 0])))
        self.assertIn("polaridade invertida", audio.classify_connector(report([1, 0, 1])))
        self.assertIn("não distingue", audio.classify_connector(report([1, 1, 1])))
        self.assertIn("incompleta", audio.classify_connector({}))


if __name__ == "__main__":
    unittest.main()
