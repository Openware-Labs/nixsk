# nixsk

A professional terminal-based toolkit and manager for Unix domain sockets

`nixsk` lets you create, manage, and inspect Unix sockets directly from the command line or through a rich full-screen TUI. It is written in pure C with zero external dependencies beyond `libncurses`, designed to follow the Unix philosophy: do one thing and do it well.

## Features

- **Create & Daemonize**: Spin up background Unix sockets instantly. Handlers default to a built-in `echo` server, but can execute arbitrary commands per connection via `--exec`.
- **System-Wide Discovery**: `nixsk list` merges your managed sockets with a system scan, revealing external sockets like X11 (`/tmp/.X11-unix/X0`), D-Bus (`/var/run/dbus/system_bus_socket`), and Linux abstract namespaces.
- **ncurses TUI**: A clean, color-coded interface to manage the lifecycle of sockets interactively.
- **Portable**: Strictly POSIX. Compiles natively on Linux, FreeBSD, OpenBSD, NetBSD, and macOS.
- **Lightweight**: A single C file, compiled in milliseconds.

## Build & Install

`nixsk` uses a BSD `Makefile` and compiles cleanly with `bmake` or standard `make`.

```sh
bmake
sudo bmake install
```

*(On Linux, install `bmake` via your package manager if your system uses GNU make by default).*

## Usage

### Command Line Interface

**Create a socket:**
```sh
# Creates an echo server at /tmp/myapp.sock
nixsk create --sock=myapp.sock /tmp/

# Creates a socket that executes a shell for every connection
nixsk create --sock=shell.sock --exec='/bin/sh' /tmp/
```

**List sockets:**
```sh
# Show managed and external system sockets
nixsk list

# Show detailed info (owner, permissions, abstract namespace)
nixsk list --verbose
```

**Remove a socket:**
```sh
nixsk remove --sock=myapp.sock
```

### Terminal UI

Run the interactive manager:
```sh
nixsk --ui
```

- `[c]`: Create a new socket (dialog).
- `[d]`: Delete a managed socket (external sockets are read-only).
- `[r]`: Refresh the list (rescans the system).
- `[q]`: Quit.
- `[↑]` / `[↓]`: Navigate.

*Green means managed and running. Cyan means an external system socket.*

## Testing Connectivity

You can connect to any socket managed by `nixsk` using standard Unix tools.

With `nc` (netcat):
```sh
printf 'hello\n' | nc -U /tmp/myapp.sock
```

With `socat`:
```sh
printf 'hello\n' | socat - UNIX-CONNECT:/tmp/myapp.sock
```

With `python3`:
```sh
python3 -c "import socket; s=socket.socket(socket.AF_UNIX); s.connect('/tmp/myapp.sock'); s.sendall(b'hello'); print(s.recv(1024).decode())"
```

## Registry

`nixsk` keeps track of the sockets it manages in a local registry. External sockets found in `/tmp`, `/run`, and `/var/run` are scanned dynamically on every `list` or `refresh` command.

Registry path resolution order:
1. `$NIXSK_HOME/registry`
2. `$XDG_CONFIG_HOME/nixsk/registry`
3. `$HOME/.config/nixsk/registry`
4. `/tmp/nixsk-registry-$UID`

## License

VSL License. See [LICENSE](/LICENSE) for details.
