# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

**Linux:**
```bash
g++ src/server.cpp -o build/server -pthread
```

**Windows:**
```bash
g++ src/server.cpp -o build/server.exe -lws2_32 -pthread
```

## Run

Place `index.html` and `style.css` in the working directory, then:
```bash
./build/server      # Linux
build/server.exe    # Windows
```

Server listens on `http://localhost:8080`. Stop with `Ctrl+C`.

## Architecture

Single-file C++ HTTP server (`src/server.cpp`) with no external dependencies.

**Key globals:**
- `html_cache` / `css_cache` — in-memory content for `index.html` and `style.css`
- `html_mod_time` / `css_mod_time` — last known `mtime` for change detection
- `running` (atomic bool) — controls the main accept loop and watcher thread
- `server_fd` — the listening socket (global so `stop_server` signal handler can close it)

**Threading model:**
- Main thread: blocking `accept()` loop, handles one connection at a time (no concurrency)
- Watcher thread (detached): polls `stat()` every 500ms and reloads files into cache on change

**Request handling flow:**
1. `recv` raw HTTP into a 4096-byte buffer
2. `get_request_path` extracts the path from `GET /path HTTP/1.1`
3. `/` maps to `index.html`; `index.html` and `style.css` are served from cache; all other paths are read from disk on demand
4. Binary vs text branching via `get_content_type` — `text/*` types use `std::string`, others use `std::vector<char>`

**Cross-platform:** `#ifdef _WIN32` guards wrap socket initialization (`WSAStartup`/`WSACleanup`), socket close calls (`closesocket` vs `close`), and `setsockopt` casting differences.

## Planned Extensions (from README)

- Auto-reload multiple CSS/JS files
- Connection and error logging to console
- Configurable port via CLI args
- Live reload without page refresh via WebSocket
