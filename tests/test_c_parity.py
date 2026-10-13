# SPDX-License-Identifier: BSD-3-Clause
import copy
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import audio_interno
import corrigir_audio
from test_audio_interno import controls, DigitalAlsa

ROOT = Path(__file__).resolve().parents[1]

class CParityTests(unittest.TestCase):
    def require_binary(self, name):
        binary = ROOT/'build'/name
        if not binary.is_file():
            self.skipTest('Execute make check para compilar os testes C primeiro.')
        return binary

    def test_c_wave_matches_python_sample_for_sample(self):
        binary=self.require_binary('test_native_helpers')
        with tempfile.TemporaryDirectory() as t:
            folder=Path(t);native=folder/'native.wav'
            subprocess.run([str(binary),'--wave',str(native)],capture_output=True,check=True)
            with patch.object(corrigir_audio,'ROOT',folder),patch.object(corrigir_audio,'run'):
                corrigir_audio.playback_test({'name':'fixture-only-no-audio'})
            self.assertEqual(native.read_bytes(),(folder/'teste-baixo-volume.wav').read_bytes())

    def test_python_plan_matches_c_fixture_stereo_and_optional_controls(self):
        binary=self.require_binary('test_mixer')
        subprocess.run([str(binary)],capture_output=True,check=True)
        for optional in (True,False):
            state=controls()
            for control in state:
                control['values']*=2
            if not optional:
                state=[c for c in state if c['name']!='HPL Playback Volume']
            expected=copy.deepcopy(state)
            for c in expected:
                if c['name']=='Headphone Switch':c['values']=[0,0]
                if c['name']=='Speaker Switch':c['values']=[1,1]
                if c['name'] in audio_interno.DIGITAL_VOLUMES:c['values']=[191,191]
            with tempfile.TemporaryDirectory() as t,patch.object(corrigir_audio,'ROOT',Path(t)),patch.object(audio_interno,'validate_hardware'),patch.object(audio_interno,'Alsa',return_value=DigitalAlsa(state)):
                audio_interno.initialize()
            self.assertEqual(state,expected)

    def test_c_cli_requires_explicit_action_and_does_not_enable_hardware_by_default(self):
        binary=self.require_binary('audio_interno')
        self.assertEqual(subprocess.run([str(binary),'--help'],capture_output=True).returncode,0)
        self.assertEqual(subprocess.run([str(binary)],capture_output=True).returncode,2)
        self.assertEqual(subprocess.run([str(binary),'aplicar','--dry-run'],capture_output=True).returncode,2)

if __name__=='__main__':unittest.main()
