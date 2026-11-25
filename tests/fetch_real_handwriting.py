#!/usr/bin/env python3
"""
Download public-domain 18th-19th century Portuguese handwritten parish registers
from Wikimedia Commons (Arquivo Nacional da Torre do Tombo / Arquivo da
Universidade de Coimbra / Arquivo Distrital do Porto) and write curated
ground-truth JSON fixtures for `scribe-bench`.

All scans are public domain (PD-ineligible / PD-old-100) and SHA-256 verified.
Images are skipped when already present with matching SHA-256.
"""

import argparse
import hashlib
import json
import os
import shutil
import sys
import urllib.parse
import urllib.request

USER_AGENT = "axil-scribe-benchmark/0.1 (https://tty.pt; archival-ocr-eval)"

RECORDS = [
    {
        "filename": "01_baptism_1875_alzira_costa.png",
        "wiki_file": "File:Assento de baptismo, Alzira Costa (27 Junho 1875).png",
        "sha256": "db796d8de679acb64a0979aaa45ac3f73093e5d880090747d66dcf58287aed12",
        "source": "Arquivo da Universidade de Coimbra (1875), PD-ineligible",
        "truth": {
            "image": "01_baptism_1875_alzira_costa.png",
            "template_id": "baptism_starter",
            "entries": [
                {
                    "fields": {
                        "person_name": "Alzira",
                        "birth_date": "1875-04-20",
                        "baptism_date": "1875-06-27",
                        "parish": "Egreja Parochial da Exaltação da Sancta Cruz, Oliveira do Hospital",
                        "father_name": "Albano Mendes d'Abreu",
                        "mother_name": "Dona Emilia de Barros Coêlho e Campos",
                        "godparents": "António Vieira de Tovar Pinto de Magalhães; Dona Maria Carlota Vieira de Tovar Pinto de Magalhães",
                        "officiant": "Manuel Côelho da Fonseca",
                        "book_page_entry": None,
                    }
                }
            ],
        },
    },
    {
        "filename": "02_baptism_1872_sidonio_pais.png",
        "wiki_file": "File:Assento de baptismo, Sidónio Pais (6 Jun. 1872).png",
        "sha256": "8b69a3c25db875179817d975ca3bead7078a9bfadfecc9c74b498c42ef324e93",
        "source": "Arquivo Nacional da Torre do Tombo (1872), PD-ineligible",
        "truth": None,  # Sample scan for interactive workbench upload
    },
    {
        "filename": "03_marriage_1786_manoel_marques.jpg",
        "wiki_file": "File:Assento de casamento de Manoel Marques e Magdalena Rosa (15Fev1786).jpg",
        "sha256": "4c53f7d9d0931ac21a173f51ee1e3745969f4093542d26fac15df2ab21ccad65",
        "source": "Paróquia de Santa Maria de Alcáçova, Elvas (1786), PD-ineligible",
        "truth": {
            "image": "03_marriage_1786_manoel_marques.jpg",
            "template_id": "wedding_starter",
            "entries": [
                {
                    "fields": {
                        "groom_name": "Manoel Marques",
                        "bride_name": "Magdalena Roza",
                        "marriage_date": "1786-02-15",
                        "parish": "Parrochial Igreja de Santa Maria de Alcaçova, Elvas",
                        "groom_parents": "Domingos Marques e Anna Thereza",
                        "bride_parents": "Christovão Joze e Catharina Barbara",
                        "witnesses": "Antonio dos Santos e Joze Joaquim",
                        "officiant": "Martiniano Procopio",
                        "book_page_entry": None,
                    }
                }
            ],
        },
    },
    {
        "filename": "04_baptism_1894_joao_almeida.png",
        "wiki_file": "File:Assento de baptismo de João Augusto Ferreira de Almeida (3-Maio-1894).png",
        "sha256": "2c604b9a7e3f05e88eabb8a64ff267573a5e957b614bef399d4641d8a468e7dd",
        "source": "Arquivo Distrital do Porto (1894), PD-old-100",
        "truth": {
            "image": "04_baptism_1894_joao_almeida.png",
            "template_id": "baptism_starter",
            "entries": [
                {
                    "fields": {
                        "person_name": "João Augusto",
                        "birth_date": "1894-04-03",
                        "baptism_date": "1894-05-03",
                        "parish": "São João da Foz do Douro, Porto",
                        "father_name": "João Ferreira d'Almeida",
                        "mother_name": "Angelina Augusta",
                        "godparents": "Joaquim Augusto Ferreira d'Almeida; Maria Emilia de Jesus Albertina",
                        "officiant": "Antonio Ribeiro dos Reis Lima",
                        "book_page_entry": "N.º 71, fl. 36",
                    }
                }
            ],
        },
    },
    {
        "filename": "05_baptism_1827_eugenio_almeida.png",
        "wiki_file": "File:Assento de baptismo de Eugénio Ricardo Monteiro de Almeida (São João da Praça, 12Mar1827).png",
        "sha256": "89bc01e8427e755499cea5494138e270b82b4f7fee555160312e6e8ee8d2c797",
        "source": "Arquivo Nacional da Torre do Tombo, Lisboa (1827), PD-ineligible",
        "truth": {
            "image": "05_baptism_1827_eugenio_almeida.png",
            "template_id": "baptism_starter",
            "entries": [
                {
                    "fields": {
                        "person_name": "Eugenio",
                        "birth_date": "1826-07-08",
                        "baptism_date": "1827-03-12",
                        "parish": "Paroquial Igreja de S. João da Praça de Lisboa",
                        "father_name": "Francisco Monteiro",
                        "mother_name": "D. Maria Joaquina",
                        "godparents": "Frederico Ricardo de Gois",
                        "officiant": "O Prior Antonio de Padua",
                        "book_page_entry": None,
                    }
                }
            ],
        },
    },
]


def file_sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def resolve_commons_url(wiki_file: str) -> str:
    params = urllib.parse.urlencode(
        {
            "action": "query",
            "titles": wiki_file,
            "prop": "imageinfo",
            "iiprop": "url",
            "format": "json",
        }
    )
    api_url = f"https://commons.wikimedia.org/w/api.php?{params}"
    req = urllib.request.Request(api_url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    pages = data.get("query", {}).get("pages", {})
    for page in pages.values():
        ii = page.get("imageinfo")
        if ii and ii[0].get("url"):
            return ii[0]["url"]
    raise RuntimeError(f"Could not resolve Wikimedia Commons URL for {wiki_file}")


def fetch_file(wiki_file: str, dst_path: str, expected_sha: str) -> None:
    url = resolve_commons_url(wiki_file)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    tmp_path = dst_path + ".part"
    with urllib.request.urlopen(req, timeout=60) as resp, open(tmp_path, "wb") as out:
        shutil.copyfileobj(resp, out)
    actual_sha = file_sha256(tmp_path)
    if expected_sha and actual_sha != expected_sha:
        os.unlink(tmp_path)
        raise RuntimeError(
            f"SHA-256 mismatch for {dst_path}: expected {expected_sha}, got {actual_sha}"
        )
    os.replace(tmp_path, dst_path)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--out",
        default="tests/fixtures/real_handwriting",
        help="Destination directory for scans and .truth.json files",
    )
    ap.add_argument(
        "--cache-dir",
        default=None,
        help="Optional local cache directory to copy verified scans from without network",
    )
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    for rec in RECORDS:
        dst = os.path.join(args.out, rec["filename"])
        expected = rec["sha256"]
        if os.path.isfile(dst) and file_sha256(dst) == expected:
            print(f"[ok]    {rec['filename']} (cached)")
        elif args.cache_dir and os.path.isfile(os.path.join(args.cache_dir, rec["filename"])):
            src = os.path.join(args.cache_dir, rec["filename"])
            if file_sha256(src) == expected:
                shutil.copy2(src, dst)
                print(f"[copy]  {rec['filename']} (from cache-dir)")
            else:
                fetch_file(rec["wiki_file"], dst, expected)
                print(f"[fetch] {rec['filename']} ({rec['source']})")
        else:
            fetch_file(rec["wiki_file"], dst, expected)
            print(f"[fetch] {rec['filename']} ({rec['source']})")

        if rec["truth"] is not None:
            stem = os.path.splitext(rec["filename"])[0]
            truth_path = os.path.join(args.out, f"{stem}.truth.json")
            with open(truth_path, "w", encoding="utf-8") as fp:
                json.dump(rec["truth"], fp, ensure_ascii=False, indent=2)
                fp.write("\n")

    print(f"Real handwriting benchmark pack ready in: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
