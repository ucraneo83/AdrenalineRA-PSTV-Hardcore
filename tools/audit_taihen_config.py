#!/usr/bin/env python3
"""Report Adrenaline references in local copies of taiHEN's configuration."""

import argparse
from pathlib import Path
import re
import sys


TITLE_IDS = {"pspemucfw": "Adrenaline original", "adra00001": "AdrenalineRA Test"}
ADRENALINE_MODULE = re.compile(
    r"adrenaline(?:_?ra)?[-_]?(?:kernel|user|vsh)(?:[-_]?test)?\.(?:skprx|suprx)"
)


def scan_config(text):
    if "\0" in text:
        raise ValueError("el archivo contiene bytes NUL")

    section = ""
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("*"):
            section = line[1:].strip()
            if section.startswith("!"):
                excluded_title = section[1:].strip()
                section = f"!{excluded_title}"
                if not excluded_title:
                    raise ValueError(f"línea {number}: exclusión sin título")
                if excluded_title.upper() in ("KERNEL", "ALL"):
                    raise ValueError(f"línea {number}: *{section} tiene comportamiento indefinido")
            if not section:
                raise ValueError(f"línea {number}: sección sin nombre")
            continue
        if not section:
            raise ValueError(f"línea {number}: ruta sin sección")

        # taiHEN only treats a '#' at the beginning of a line as a comment.
        parts = re.split(r"[/\\:]+", line.casefold())
        profile = next((TITLE_IDS[part] for part in parts if part in TITLE_IDS), None)
        if not section.startswith("!"):
            profile = profile or TITLE_IDS.get(section.casefold())
        if profile or ADRENALINE_MODULE.fullmatch(parts[-1]):
            yield number, section, line, profile or "módulo de Adrenaline sin perfil identificado", True
        elif section == "KERNEL" and parts[-1] == "usbmc.skprx":
            yield number, section, line, "usbmc puede recargar la configuración después del montaje", False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for volume in ("ux0", "ur0"):
        choices = parser.add_mutually_exclusive_group()
        choices.add_argument(f"--{volume}", type=Path, help=f"copia local de {volume}:tai/config.txt")
        choices.add_argument(f"--{volume}-absent", action="store_true", help=f"se verificó que {volume}:tai/config.txt no existe")
    args = parser.parse_args()

    conflict = False
    incomplete = False
    for volume in ("ux0", "ur0"):
        label = f"{volume}:tai/config.txt"
        path = getattr(args, volume)
        if path is None:
            if getattr(args, f"{volume}_absent"):
                print(f"{label} no existe según la comprobación indicada.")
            else:
                print(f"{label} sin copia para revisar.")
                incomplete = True
            continue

        try:
            findings = list(scan_config(path.read_text(encoding="utf-8-sig")))
        except (OSError, ValueError) as exc:
            print(f"{label} no se pudo inspeccionar. {exc}", file=sys.stderr)
            incomplete = True
            continue
        if not findings:
            print(f"{label} sin referencias reconocidas a Adrenaline ni a usbmc.")
        for line, section, module, reason, related in findings:
            print(f"{label}, línea {line}, *{section}  {module}  ({reason})")
            conflict |= related
            incomplete |= not related

    if args.ux0 is not None:
        print("taiHEN intenta abrir ux0 primero y prueba ur0 si la apertura falla.")
    elif args.ux0_absent and args.ur0 is not None:
        print("Con ux0 ausente, taiHEN abre la configuración de ur0 en un arranque normal.")
    print("El informe no confirma qué módulos están cargados ni habilita AdrenalineRA Test.")
    if conflict:
        return 1
    return 2 if incomplete else 0


if __name__ == "__main__":
    sys.exit(main())
