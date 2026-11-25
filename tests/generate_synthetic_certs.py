#!/usr/bin/env python3
"""
Deterministic synthetic archival certificate & register fixture generator
for axil-scribe benchmarking (100% fictional data — zero real PII).

Generates aged, slightly tilted, noisy PNG scans with parish stamps and
marginal notes alongside canonical *.truth.json files.
"""

import argparse
import json
import os
import random
from PIL import Image, ImageDraw, ImageFont

FIXTURES = [
    {
        "slug": "01_confirmation_clean",
        "template_id": "confirmation_starter",
        "title": "CERTIFICATE OF CONFIRMATION",
        "tilt": -1.2,
        "noise": 8,
        "entries": [
            {
                "fields": {
                    "person_name": "Clara Beatriz Monteiro",
                    "confirmation_name": "Teresa",
                    "confirmation_date": "1958-06-14",
                    "parish": "Paróquia de São João",
                    "sponsor": "Helena Duarte",
                    "parents": "Artur Monteiro e Inês Costa",
                    "officiant": "D. Manuel Nogueira",
                    "baptism_date": "1946-03-09",
                    "baptism_parish": "Paróquia de Santa Cruz",
                    "book_page_entry": "Livro 4, Fl. 18, Assento 12",
                },
                "marginal_notes": "Casou em 1971-09-04 na Sé de Braga",
            }
        ],
    },
    {
        "slug": "02_wedding_aged",
        "template_id": "wedding_starter",
        "title": "CERTIDÃO DE CASAMENTO",
        "tilt": 1.8,
        "noise": 14,
        "entries": [
            {
                "fields": {
                    "groom_name": "Henrique Afonso Tavares",
                    "bride_name": "Sofia da Conceição Lima",
                    "marriage_date": "1964-08-22",
                    "parish": "Igreja Matriz de Santo António",
                    "groom_parents": "João Tavares e Amélia Rocha",
                    "bride_parents": "Álvaro Lima e Rosa Martins",
                    "witnesses": "Eduardo Neves e Margarida Reis",
                    "officiant": "Pe. Joaquim Figueiredo",
                    "book_page_entry": "Livro 9, Fl. 77, Assento 34",
                },
                "marginal_notes": None,
            }
        ],
    },
    {
        "slug": "03_baptism_faded",
        "template_id": "baptism_starter",
        "title": "ASSENTO DE BAPTISMO",
        "tilt": -2.1,
        "noise": 18,
        "entries": [
            {
                "fields": {
                    "person_name": "Tiago Lourenço Almeida",
                    "birth_date": "1951-11-03",
                    "baptism_date": "1951-11-25",
                    "parish": "Paróquia de Nossa Senhora da Graça",
                    "father_name": "Fernando Almeida",
                    "mother_name": "Beatriz Lourenço",
                    "godparents": "Carlos Mendes e Lurdes Silva",
                    "officiant": "Pe. António Barbosa",
                    "book_page_entry": "Livro 12, Fl. 5, Assento 2",
                },
                "marginal_notes": "Crismado em 1963-05-19",
            }
        ],
    },
    {
        "slug": "04_confirmation_ledger_2entries",
        "template_id": "confirmation_starter",
        "title": "LIVRO DE REGISTO DE CRISMA — FOLHA 42",
        "tilt": 0.9,
        "noise": 12,
        "entries": [
            {
                "fields": {
                    "person_name": "Diogo Mendes Correia",
                    "confirmation_name": "Paulo",
                    "confirmation_date": "1961-05-21",
                    "parish": "Paróquia de São Pedro",
                    "sponsor": "Rui Vasconcelos",
                    "parents": "Luís Correia e Graça Mendes",
                    "officiant": "D. Francisco Pacheco",
                    "baptism_date": "1949-01-12",
                    "baptism_parish": "Paróquia de São Pedro",
                    "book_page_entry": "Livro 6, Fl. 42, Assento 81",
                },
                "marginal_notes": None,
            },
            {
                "fields": {
                    "person_name": "Isabel Cristina Azevedo",
                    "confirmation_name": "Lúcia",
                    "confirmation_date": "1961-05-21",
                    "parish": "Paróquia de São Pedro",
                    "sponsor": "Adelaide Fonseca",
                    "parents": "Vítor Azevedo e Fernanda Lopes",
                    "officiant": "D. Francisco Pacheco",
                    "baptism_date": "1949-04-30",
                    "baptism_parish": "Paróquia de São Pedro",
                    "book_page_entry": "Livro 6, Fl. 42, Assento 82",
                },
                "marginal_notes": "Nota marginal: averbado em 1975",
            },
        ],
    },
]


def load_font(size: int, italic: bool = False):
    candidates = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Italic.ttf"
        if italic
        else "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSerif-Italic.ttf"
        if italic
        else "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
    ]
    for path in candidates:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def render_fixture(spec: dict, out_dir: str, rng: random.Random):
    w, h = 1100, 1450
    bg = (242, 235, 218)
    img = Image.new("RGB", (w, h), bg)
    draw = ImageDraw.Draw(img)

    # Decorative border
    draw.rectangle([45, 45, w - 45, h - 45], outline=(95, 82, 66), width=3)
    draw.rectangle([55, 55, w - 55, h - 55], outline=(140, 125, 105), width=1)

    f_title = load_font(30, italic=False)
    f_label = load_font(20, italic=False)
    f_hand = load_font(24, italic=True)
    f_margin = load_font(17, italic=True)

    draw.text((110, 85), spec["title"], fill=(45, 38, 30), font=f_title)
    draw.line([(90, 132), (w - 90, 132)], fill=(95, 82, 66), width=2)

    y = 165
    for idx, entry in enumerate(spec["entries"]):
        if len(spec["entries"]) > 1:
            draw.text((90, y), f"--- Registo #{idx + 1} ---", fill=(85, 70, 55), font=f_label)
            y += 34

        for k, v in entry["fields"].items():
            label = k.replace("_", " ").title() + ":"
            draw.text((95, y), label, fill=(75, 65, 55), font=f_label)
            if v:
                ink = (32 + rng.randint(0, 20), 42 + rng.randint(0, 20), 82 + rng.randint(0, 25))
                draw.text((370, y - 3), str(v), fill=ink, font=f_hand)
            draw.line([(365, y + 28), (w - 110, y + 28)], fill=(195, 185, 165), width=1)
            y += 46

        if entry.get("marginal_notes"):
            draw.text((95, y + 4), "Obs. marginal: " + entry["marginal_notes"],
                      fill=(110, 50, 45), font=f_margin)
            y += 38
        y += 24

    # Simulated circular parish stamp
    cx, cy, r = w - 210, h - 190, 72
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(85, 95, 145), width=3)
    draw.ellipse([cx - r + 8, cy - r + 8, cx + r - 8, cy + r - 8], outline=(85, 95, 145), width=1)
    draw.text((cx - 48, cy - 10), "PARÓQUIA", fill=(85, 95, 145), font=f_margin)

    # Subtle deterministic grain noise
    noise_amp = spec.get("noise", 10)
    px = img.load()
    for _ in range(18000):
        nx = rng.randrange(0, w)
        ny = rng.randrange(0, h)
        r0, g0, b0 = px[nx, ny]
        d = rng.randint(-noise_amp, noise_amp)
        px[nx, ny] = (
            max(0, min(255, r0 + d)),
            max(0, min(255, g0 + d)),
            max(0, min(255, b0 + d)),
        )

    # Gentle scan tilt
    img = img.rotate(spec.get("tilt", 0.0), resample=Image.BICUBIC, fillcolor=bg)

    img_name = f"{spec['slug']}.png"
    truth_name = f"{spec['slug']}.truth.json"
    img.save(os.path.join(out_dir, img_name), format="PNG")

    truth = {
        "image": img_name,
        "template_id": spec["template_id"],
        "entries": spec["entries"],
    }
    with open(os.path.join(out_dir, truth_name), "w", encoding="utf-8") as fp:
        json.dump(truth, fp, ensure_ascii=False, indent=2)


def main():
    ap = argparse.ArgumentParser(description="Generate synthetic archival OCR fixtures")
    ap.add_argument("--out", default="tests/fixtures/synthetic", help="Output directory")
    ap.add_argument("--seed", type=int, default=20261125, help="Deterministic RNG seed")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    rng = random.Random(args.seed)
    for spec in FIXTURES:
        render_fixture(spec, args.out, rng)
    print(f"Generated {len(FIXTURES)} synthetic fixtures in {args.out}")


if __name__ == "__main__":
    main()
