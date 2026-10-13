# SPDX-License-Identifier: BSD-3-Clause
import copy
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import audio_interno as interno
import corrigir_audio as audio
from test_audio import CONTROLS, CARD, FakeAlsa


def controls():
    items = copy.deepcopy(CONTROLS)
    for index, name in enumerate(interno.DIGITAL_VOLUMES, 10):
        items.append({"name": name, "numid": index, "type": 2, "writable": True,
                      "min": 0, "max": 191, "values": [0]})
    items.append({"name": "HPL Volume", "numid": 30, "type": 2, "writable": True,
                  "min": 0, "max": 5, "values": [3]})
    return items


class DigitalAlsa(FakeAlsa):
    def neutral_volume(self, control):
        return 191


class InternalAudioTests(unittest.TestCase):
    def test_mixer_preserves_sensor_and_analog_amplifier(self):
        state = controls()
        alsa = DigitalAlsa(state)
        with tempfile.TemporaryDirectory() as folder, patch.object(audio, "ROOT", Path(folder)), \
                patch.object(interno, "validate_hardware"), patch.object(interno, "Alsa", return_value=alsa):
            interno.initialize()
            by_name = {c["name"]: c for c in state}
            self.assertEqual(by_name["Headphone Jack"]["values"], [1])
            self.assertEqual(by_name["HPL Volume"]["values"], [3])
            self.assertEqual(by_name["Speaker Switch"]["values"], [1])
            self.assertEqual(by_name["Headphone Switch"]["values"], [0])
            self.assertTrue(all(by_name[name]["values"] == [191] for name in interno.DIGITAL_VOLUMES))
            self.assertEqual(len(list(Path(folder).glob("mixer-*.json"))), 1)

    def test_unreadable_decibels_never_change_any_control(self):
        state = controls()
        before = copy.deepcopy(state)
        alsa = DigitalAlsa(state)
        with patch.object(interno, "validate_hardware"), patch.object(interno, "Alsa", return_value=alsa), \
                patch.object(alsa, "neutral_volume", side_effect=RuntimeError("TLV indisponível")):
            with self.assertRaisesRegex(RuntimeError, "TLV"):
                interno.initialize()
        self.assertEqual(state, before)

    def test_invalid_decibels_never_change_any_control(self):
        state = controls()
        before = copy.deepcopy(state)
        alsa = DigitalAlsa(state)
        with patch.object(interno, "validate_hardware"), patch.object(interno, "Alsa", return_value=alsa), \
                patch.object(alsa, "neutral_volume", return_value=192):
            with self.assertRaisesRegex(RuntimeError, "fora do intervalo"):
                interno.initialize()
        self.assertEqual(state, before)

    def test_failed_hardware_write_rolls_back_previous_writes(self):
        state = controls()
        before = copy.deepcopy(state)
        alsa = DigitalAlsa(state)
        original_write = alsa.write
        def fail_once(control, values):
            if control["name"] == "DAC Playback Volume" and values == [191]:
                raise RuntimeError("Falha de escrita")
            return original_write(control, values)
        with tempfile.TemporaryDirectory() as folder, patch.object(audio, "ROOT", Path(folder)), \
                patch.object(interno, "validate_hardware"), patch.object(interno, "Alsa", return_value=alsa), \
                patch.object(alsa, "write", side_effect=fail_once):
            with self.assertRaisesRegex(RuntimeError, "Falha de escrita"):
                interno.initialize()
        self.assertEqual(state, before)

    def test_activation_without_session_does_not_restart_or_change_audio(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(audio, "ROOT", Path(folder)), \
                patch.object(interno, "validate_hardware"), \
                patch.object(audio, "snapshot", return_value={"cards_error": "sessão bloqueada"}), \
                patch.object(audio, "run", side_effect=RuntimeError("sessão bloqueada")) as run:
            with self.assertRaisesRegex(RuntimeError, "sessão bloqueada"):
                interno.apply()
            run.assert_called_once()
            self.assertNotIn("restart", run.call_args.args[0])
            self.assertEqual(len(list(Path(folder).glob("ativacao-*/erro.json"))), 1)

    def test_old_helper_recognizes_the_exclusive_speaker_sink(self):
        sink = {"name": "alsa_output.internal.vision14-speakers", "card": 0, "properties": {}}
        self.assertEqual(audio.find_sink(CARD, [sink]), sink)


if __name__ == "__main__":
    unittest.main()
