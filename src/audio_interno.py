#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Ativa e verifica a rota permanente dos alto-falantes do ES8326."""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import time

from alsa_controles import Alsa
import corrigir_audio as audio

PROFILE = "vision14-interno"
PORT = "vision14-speaker"
DIGITAL_VOLUMES = ("DAC Playback Volume", "HPL Playback Volume", "HPR Playback Volume",
                   "SPKL Playback Volume", "SPKR Playback Volume")


def validate_hardware():
    name = Path("/sys/class/dmi/id/product_name").read_text().strip()
    driver = Path("/sys/bus/i2c/devices/i2c-ESSX8326:00/driver").resolve()
    if name != "C4128A-14" or driver.name != "es8326":
        raise RuntimeError("Este ajuste só se aplica ao C4128A-14 com o driver es8326.")


def initialize():
    validate_hardware()
    with Alsa(audio.ALSA_CARD) as alsa:
        controls = alsa.controls()
        by_name = {item["name"]: item for item in controls}
        plan = []
        for name, value in (("Headphone Switch", 0), ("Speaker Switch", 1)):
            control = by_name.get(name)
            if not control or control.get("type") != 1 or not control.get("writable"):
                raise RuntimeError("Controle de rota indisponível: " + name)
            plan.append((control, [value] * len(control["values"])))
        if "DAC Playback Volume" not in by_name:
            raise RuntimeError("O controle DAC esperado não existe; não vou estimar um volume.")
        for name in DIGITAL_VOLUMES:
            control = by_name.get(name)
            if control is None:
                continue
            if control.get("type") != 2 or not control.get("writable"):
                raise RuntimeError("Controle digital indisponível: " + name)
            value = alsa.neutral_volume(control)
            if not control["min"] <= value <= control["max"]:
                raise RuntimeError("O nível calculado está fora do intervalo de " + name)
            plan.append((control, [value] * len(control["values"])))
        history = audio.ROOT / ("mixer-" + dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f") + ".json")
        audio.save(history, {"before": controls, "plan": [{"name": c["name"], "values": v} for c, v in plan]})
        written = []
        try:
            for control, values in plan:
                if control["values"] != values:
                    written.append(control)
                    result = alsa.write(control, values)
                    if result.get("values") != values:
                        raise RuntimeError("O driver não manteve o ajuste de " + control["name"])
            after = alsa.controls()
            audio.save(history, {"before": controls, "after": after,
                                  "plan": [{"name": c["name"], "values": v} for c, v in plan]})
        except Exception:
            for control in reversed(written):
                try:
                    alsa.write(control, control["values"])
                except Exception as error:
                    print("Falha ao restaurar", control["name"], ":", error, file=sys.stderr)
            raise
    print("Mixer: alto-falantes ligados; volumes digitais ajustados pelo nível em dB do driver.")


def apply():
    validate_hardware()
    directory = audio.ROOT / ("ativacao-" + dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    directory.mkdir(mode=0o700)
    (audio.ROOT / "ultima-ativacao.txt").write_text(str(directory) + "\n")
    try:
        activate(directory)
    except Exception as error:
        audio.save(directory / "erro.json", {"error": str(error)})
        try:
            audio.save(directory / "estado-na-falha.json", audio.snapshot())
        except Exception:
            pass
        print("Diagnóstico da tentativa:", directory, file=sys.stderr)
        raise


def activate(directory):
    # Sem sessão acessível, nenhum comando de alteração é emitido.
    audio.run(["systemctl", "--user", "show", "wireplumber.service", "--property=LoadState", "--value"])
    audio.save(directory / "antes.json", audio.snapshot())
    audio.run(["systemctl", "--user", "daemon-reload"])
    audio.run(["systemctl", "--user", "restart", "pipewire.service", "pipewire-pulse.service", "wireplumber.service"], timeout=30)
    card = None
    for _ in range(24):
        try:
            card = audio.find_card(audio.pulse_list("cards"))
            break
        except (OSError, RuntimeError, ValueError):
            time.sleep(0.25)
    if card is None:
        raise RuntimeError("A placa interna não reapareceu. O backup permite desfazer a configuração.")
    audio.run(["pactl", "set-card-profile", card["name"], PROFILE])
    sink = None
    for _ in range(24):
        try:
            sinks = [item for item in audio.pulse_list("sinks") if str(item.get("card")) == str(card["index"])]
        except (OSError, RuntimeError, ValueError):
            time.sleep(0.25)
            continue
        if len(sinks) == 1 and ".vision14-speakers" in sinks[0].get("name", ""):
            sink = sinks[0]
            break
        time.sleep(0.25)
    if sink is None:
        raise RuntimeError("O perfil não criou exatamente uma saída de alto-falantes. Guarde o diagnóstico.")
    ports = sink.get("ports", [])
    if isinstance(ports, dict):
        ports = [{"name": name, **details} for name, details in ports.items()]
    if len(ports) != 1 or ports[0].get("name") != PORT:
        raise RuntimeError("A rota de alto-falantes não foi carregada como porta única.")
    audio.run(["pactl", "set-sink-port", sink["name"], PORT])
    audio.run(["pactl", "set-sink-volume", sink["name"], "40%"])
    audio.run(["pactl", "set-sink-mute", sink["name"], "0"])
    audio.run(["pactl", "set-default-sink", sink["name"]])
    initialize()
    for stream in audio.pulse_list("sink-inputs"):
        audio.run(["pactl", "move-sink-input", str(stream["index"]), sink["name"]], check=False)
    after = audio.snapshot()
    audio.save(directory / "depois.json", after)
    active = audio.find_sink(audio.find_card(after.get("cards", [])), after.get("sinks", []))
    if active.get("active_port") != PORT or active.get("mute") or after.get("default_sink") != sink["name"]:
        raise RuntimeError("O gerenciador não manteve os alto-falantes como saída padrão sem mute.")
    print("Saída ativa: Alto-falantes do PC. A configuração será carregada nas próximas sessões.")
    audio.playback_test(sink)
    audio.save(directory / "apos-teste.json", audio.snapshot())
    print("Reprodução enviada à placa. Confirme se o som foi audível. Diagnóstico:", directory)


def undo():
    directory = Path((audio.ROOT / "backup-audio-interno.txt").read_text().strip())
    manifest = json.loads((directory / "manifest.json").read_text())
    expected = {str(Path.home() / relative) for relative in (
        ".config/wireplumber/wireplumber.conf.d/51-vision14-es8326.conf",
        ".config/systemd/user/wireplumber.service.d/51-vision14-speakers.conf",
        ".config/alsa-card-profile/vision14-interno.conf",
        ".config/alsa-card-profile/vision14-speaker.conf")}
    if {item["path"] for item in manifest["files"]} != expected:
        raise RuntimeError("Manifesto de restauração inválido.")
    for item in manifest["files"]:
        path = Path(item["path"])
        if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() != item.get("applied_sha256"):
            raise RuntimeError("O arquivo recebeu mudanças posteriores; restauração cancelada: " + str(path))
    for item in manifest["files"]:
        path = Path(item["path"])
        if item["existed"]:
            shutil.copy2(directory / item["file"], path)
        else:
            path.unlink(missing_ok=True)
    audio.run(["systemctl", "--user", "daemon-reload"])
    audio.run(["systemctl", "--user", "restart", "pipewire.service", "pipewire-pulse.service", "wireplumber.service"], timeout=30)
    print("Perfil exclusivo retirado; configuração anterior restaurada.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", nargs="?", choices=["aplicar", "inicializar", "desfazer"], default="aplicar")
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("Execute como usuário normal, sem sudo.")
    {"aplicar": apply, "inicializar": initialize, "desfazer": undo}[args.action]()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("Não foi possível concluir:", error, file=sys.stderr)
        sys.exit(1)
