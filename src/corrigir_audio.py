#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Diagnóstico e seleção reversível de alto-falantes do Positivo C4128A-14."""
import argparse
import datetime as dt
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time
import wave
from array import array

from alsa_controles import Alsa

ROOT = Path(__file__).resolve().parent
CARD = "alsa_card.pci-0000_00_0e.0-platform-sof-essx8336"
ALSA_CARD = "sofessx8336"


def run(args, *, check=True, timeout=15):
    process = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    if check and process.returncode:
        raise RuntimeError((process.stderr or process.stdout).strip() or str(args))
    return process


def save(path, data):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
    temporary.chmod(0o600)
    temporary.replace(path)


def pulse_list(kind):
    return json.loads(run(["pactl", "--format=json", "list", kind]).stdout)


def find_card(cards):
    matches = [card for card in cards if card.get("name") == CARD]
    if len(matches) != 1:
        raise RuntimeError("Não encontrei de forma única a placa interna esperada. Nenhuma outra placa será alterada.")
    return matches[0]


def find_sink(card, sinks):
    matches = [sink for sink in sinks if str(sink.get("card")) == str(card["index"])
               and not sink.get("name", "").endswith((".hdmi-stereo", ".hdmi-stereo-extra1", ".hdmi-stereo-extra2"))
               and (sink.get("properties", {}).get("alsa.device") in ("0", 0)
                    or "analog" in sink.get("name", "")
                    or sink.get("name", "").endswith(".vision14-speakers"))]
    if len(matches) != 1:
        raise RuntimeError("A saída analógica não está ativa ou há mais de uma candidata. Guarde o diagnóstico antes de alterar o perfil.")
    return matches[0]


def read_alsa():
    try:
        with Alsa(ALSA_CARD) as alsa:
            return alsa.controls()
    except (OSError, RuntimeError) as error:
        return {"error": str(error)}


def snapshot():
    data = {"time": dt.datetime.now(dt.timezone.utc).isoformat(), "alsa": read_alsa()}
    for key in ("cards", "sinks"):
        try:
            data[key] = pulse_list(key)
        except (OSError, RuntimeError, ValueError) as error:
            data[key + "_error"] = str(error)
    try:
        data["default_sink"] = run(["pactl", "get-default-sink"]).stdout.strip()
    except (OSError, RuntimeError) as error:
        data["default_sink_error"] = str(error)
    return data


def jack_values(data):
    controls = data.get("alsa", [])
    if not isinstance(controls, list):
        return {}
    return {item["name"]: item.get("values") for item in controls
            if "Jack" in item["name"] and "HDMI" not in item["name"]}


def show_jacks(data):
    jacks = jack_values(data)
    print("Detecção física informada pelo driver:", json.dumps(jacks, ensure_ascii=False) if jacks else "não foi possível ler")


def classify_connector(results):
    phases = ("sem-fone", "com-fone", "sem-fone-novamente")
    values = [results.get(phase, {}).get("Headphone Jack") for phase in phases]
    if any(not isinstance(value, list) or not value for value in values):
        return "Leitura incompleta: ainda não é possível avaliar o sensor do conector."
    states = [any(bool(item) for item in value) for value in values]
    if states == [False, True, False]:
        return "A detecção do conector respondeu normalmente; investigue seleção de saída e mixer."
    if states == [True, False, True]:
        return "A detecção respondeu ao contrário; há indício de polaridade invertida no driver/firmware, a confirmar com repetição."
    if len(set(states)) == 1:
        return "A detecção não mudou: pode haver falha de detecção/interrupt no driver ou problema físico. Este teste sozinho não distingue os dois."
    return "Resposta irregular: repita o teste para distinguir atraso, instabilidade de software e mau contato."


def playback_test(sink):
    # Sinal curto a -26 dBFS, com entrada/saída gradual, sem aumentar o volume.
    sample_rate = 48000
    samples = array("h")
    for channel in (0, 1):
        for index in range(int(sample_rate * 0.7)):
            fade = min(1, index / 1200, (sample_rate * 0.7 - index) / 1200)
            value = int(1500 * fade * math.sin(2 * math.pi * 440 * index / sample_rate))
            samples.extend((value, 0) if channel == 0 else (0, value))
    if sys.byteorder != "little":
        samples.byteswap()
    path = ROOT / "teste-baixo-volume.wav"
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(2)
        stream.setsampwidth(2)
        stream.setframerate(sample_rate)
        stream.writeframes(samples.tobytes())
    print("Reproduzindo dois tons curtos e baixos: esquerdo, depois direito.")
    run(["pw-play", "--target=" + sink["name"], str(path)])


def volumes(sink):
    return [str(item["value"]) for item in sink.get("volume", {}).values()]


def restore_backup(path):
    backup = json.loads(Path(path).read_text())
    card = find_card(pulse_list("cards"))
    sink = find_sink(card, pulse_list("sinks"))
    if sink["name"] != backup["sink"]["name"]:
        raise RuntimeError("A saída mudou desde o backup; restauração automática cancelada.")
    if backup.get("default_sink"):
        run(["pactl", "set-default-sink", backup["default_sink"]])
    original = backup["sink"]
    if original.get("active_port"):
        run(["pactl", "set-sink-port", sink["name"], original["active_port"]])
    previous = volumes(original)
    if previous:
        run(["pactl", "set-sink-volume", sink["name"], *previous])
    run(["pactl", "set-sink-mute", sink["name"], "1" if original.get("mute") else "0"])
    if backup.get("changed_controls"):
        with Alsa(ALSA_CARD) as alsa:
            for control in reversed(backup["changed_controls"]):
                current = next((item for item in alsa.controls()
                                if item["name"] == control["name"]), None)
                if current is None:
                    raise RuntimeError("Controle não encontrado para restauração: " + control["name"])
                alsa.write(current, control["values"])
    print("Saída, volume, mute e controles alterados restaurados.")


def repair(data, report_dir):
    if "cards" not in data or "sinks" not in data:
        raise RuntimeError("Sem acesso à sessão de áudio. Execute este script no terminal do seu desktop, sem sudo.")
    card = find_card(data["cards"])
    sink = find_sink(card, data["sinks"])
    ports = sink.get("ports", [])
    if isinstance(ports, dict):
        ports = [{"name": name, **details} for name, details in ports.items()]
    speaker_port = next((name for name in ("vision14-speaker", "analog-output-speaker")
                         if any(port.get("name") == name for port in ports)), None)
    if speaker_port is None:
        raise RuntimeError("O perfil não oferece a porta dos alto-falantes. O diagnóstico foi salvo; é preciso corrigir o perfil antes.")
    backup = {"sink": sink, "default_sink": data.get("default_sink"), "changed_controls": []}
    backup_file = report_dir / "backup.json"
    save(backup_file, backup)
    changed = False
    try:
        changed = True
        run(["pactl", "set-sink-port", sink["name"], speaker_port])
        run(["pactl", "set-sink-volume", sink["name"], "30%"])
        run(["pactl", "set-sink-mute", sink["name"], "0"])
        run(["pactl", "set-default-sink", sink["name"]])
        # Mesmas chaves de roteamento usadas pelo perfil UCM original.
        # Não escreve no jack, em GPIOs ou nos registros de detecção.
        with Alsa(ALSA_CARD) as alsa:
            controls = alsa.controls()
            for name, value in (("Headphone Switch", 0), ("Speaker Switch", 1)):
                control = next((item for item in controls if item["name"] == name), None)
                if control is None or control.get("type") != 1 or not control.get("writable"):
                    raise RuntimeError("Controle de roteamento necessário indisponível: " + name)
                if control["values"] != [value] * len(control["values"]):
                    backup["changed_controls"].append(control)
                    save(backup_file, backup)
                    alsa.write(control, [value] * len(control["values"]))
        time.sleep(0.5)
        after = snapshot()
        save(report_dir / "depois.json", after)
        active = find_sink(find_card(after["cards"]), after["sinks"])
        if active.get("active_port") != speaker_port or active.get("mute"):
            raise RuntimeError("O gerenciador não manteve a saída dos alto-falantes ativa.")
        playback_test(active)
        show_jacks(after)
        print("Comandos de seleção e reprodução concluídos. Confirme se ouviu o som pelos alto-falantes.")
        print("Para desfazer: python3", str(Path(__file__).resolve()), "restaurar", str(backup_file))
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired, KeyboardInterrupt):
        if changed:
            try:
                restore_backup(backup_file)
            except Exception as error:
                print("Restauração incompleta:", error, "Backup:", backup_file, file=sys.stderr)
        raise


def connector_test(report_dir):
    if not sys.stdin.isatty():
        raise RuntimeError("O teste de conector precisa de um terminal interativo.")
    phases = (("sem-fone", "Retire qualquer fone do conector e pressione Enter."),
              ("com-fone", "Conecte um fone físico e pressione Enter."),
              ("sem-fone-novamente", "Retire o fone novamente e pressione Enter."))
    results = {}
    for phase, prompt in phases:
        input(prompt + "\n")
        time.sleep(2)
        data = snapshot()
        save(report_dir / (phase + ".json"), data)
        show_jacks(data)
        results[phase] = jack_values(data)
    save(report_dir / "teste-conector.json", results)
    print(classify_connector(results))
    print("Teste salvo em", report_dir / "teste-conector.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", nargs="?", default="reparar", choices=["reparar", "diagnosticar", "conector", "restaurar"])
    parser.add_argument("backup", nargs="?")
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("Execute como brendha, sem sudo; a sessão e o backup pertencem ao usuário.")
    if args.action == "restaurar":
        if not args.backup:
            parser.error("Informe o caminho do backup.json")
        restore_backup(args.backup)
        return 0
    report_dir = ROOT / ("coleta-" + dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    report_dir.mkdir(mode=0o700)
    data = snapshot()
    save(report_dir / "antes.json", data)
    (ROOT / "ultima-coleta.txt").write_text(str(report_dir) + "\n")
    print("Pasta do diagnóstico:", report_dir)
    show_jacks(data)
    if args.action == "reparar":
        repair(data, report_dir)
    elif args.action == "conector":
        connector_test(report_dir)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print("Não foi possível concluir:", error, file=sys.stderr)
        sys.exit(1)
