# axil-scribe

[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](LICENSE)
[![Offline First](https://img.shields.io/badge/mode-Offline%20%7C%20Web-16A34A)](#)

> Pure-C handwritten and archival document OCR, structured field extraction, and human-in-the-loop verification module for [axil](https://github.com/tty-pt/axil).

`axil-scribe` is an open, standalone archival transcription system designed for handwritten certificates and bound register books (e.g., parish Confirmation, Wedding, and Baptism records, as well as arbitrary archival forms). It runs either **100% offline on a local workstation/LAN** or as a **hosted web service**, built entirely on the `tty.pt` pure-C stack:

- **HTTP & module runtime:** `axil` + `libxylem`
- **Template-agnostic schemas & storage:** `libhyle` + `libhyle-source` + `libcorm` + `liblemma` (+ optional `libsepal`)
- **Offline & cloud VLM extraction:** `axil-auren` (`/v1/chat/completions` with `image_url`, powered locally by `llama.cpp` `mtmd` or optional zero-retention cloud upstreams)
- **SSR & progressive verification UI:** `libbud` + `libhyle-bud` + `libstele` (No-JS fallback + isomorphic WASM split-screen viewer)
- **Cross-OS distribution:** `mk`, `tty-pt/ci` (`deb`, `rpm`, `apk`, `pacman`, `brew`, `openbsd`, `winget`), native OS services, and `@tty-pt/scribe` on NPM

## Status

Initial architecture and roadmap phase. See **[ROADMAP.md](ROADMAP.md)** for the data layout, architectural invariants, phased implementation plan, and verification gates.

## License

[BSD-2-Clause](LICENSE) © 2026 tty-pt.
