"""Drive the LocalSplitScreen mod's console from the host (a dev build: LSS_DEV_TOOLS).

    python tools/remote.py shell                    an interactive console (tab completes commands with pyreadline3)
    python tools/remote.py -c join -c "wait-until p2.state == playing"
                                                    send lines, print every reply, exit 1 on a failure
    python tools/remote.py run script.txt           send a file line by line, stopping at the first failure
    python tools/remote.py status                   every readable value (exit 2 if the mod is not listening)

The mod listens on 127.0.0.1:4419 (src/control/remote.h): one command per line, UTF-8; every reply is one line
starting with ok, failed, rejected, dropped, value, help or note (src/control/runner.h). `help` lists the commands
the running build registered. Blank lines and # comments in a script are not sent.

Taken from Remake's tools/game.py (2026-10-23), its console part only: launching, quitting and deploying stay with
tools/regression. Changed: the port (Remake's 4418 plus one, so both games' consoles can listen at once); `run`
reads a UTF-8 file with or without a byte-order mark; the shell skips # lines (the mod never answers a comment, so
Remake's shell waited out its timeout) and Ctrl+C at an empty prompt aborts nothing; a lost connection is reported,
not a traceback.
"""
import argparse
import pathlib
import socket
import sys

HOST, PORT = "127.0.0.1", 4419


class Console:
    """One connection to the mod's console."""

    def __init__(self, timeout: float = 120.0):
        self.sock = socket.create_connection((HOST, PORT), timeout=5.0)
        self.sock.settimeout(timeout)
        self.buffer = b""
        self.sent = 0

    def close(self):
        self.sock.close()

    def send(self, line: str) -> int:
        """Sends one line; returns its number as the mod will name it in replies."""
        self.sock.sendall((line.rstrip("\r\n") + "\n").encode("utf-8"))
        self.sent += 1
        return self.sent

    def reply(self) -> str:
        """The next reply line (blocks up to the timeout)."""
        while b"\n" not in self.buffer:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("the mod closed the connection")
            self.buffer += chunk
        line, _, self.buffer = self.buffer.partition(b"\n")
        return line.decode("utf-8", errors="replace").rstrip("\r")

    def command(self, line: str, echo=print) -> bool:
        """Sends a line and prints its replies until the one that completes it. False on failed/rejected/dropped."""
        number = str(self.send(line))
        while True:
            reply = self.reply()
            echo(reply)
            word, _, rest = reply.partition(" ")
            if rest.split(" ", 1)[0] != number:
                continue
            if word in ("ok", "note"):
                return True
            if word in ("failed", "rejected", "dropped"):
                return False

    def values(self) -> dict:
        number = str(self.send("status"))
        out = {}
        while True:
            word, _, rest = self.reply().partition(" ")
            if word == "value":
                key, _, value = rest.partition(" ")
                out[key] = value
            elif word in ("ok", "failed") and rest.split(" ", 1)[0] == number:
                return out

    def names(self) -> list:
        number = str(self.send("help"))
        out = []
        while True:
            word, _, rest = self.reply().partition(" ")
            if word == "help":
                out.append(rest.split(" ", 1)[0])
            elif word in ("ok", "failed") and rest.split(" ", 1)[0] == number:
                return out


def connect(timeout: float = 120.0) -> Console:
    try:
        return Console(timeout)
    except OSError:
        sys.exit(f"the mod is not listening on {HOST}:{PORT} (is the game running with a dev build of the mod?)")


def cmd_shell(args) -> int:
    console = connect(timeout=3600.0)
    names = console.names() + ["get", "status", "help", "wait", "wait-until", "hold", "note", "abort"]
    try:
        import readline  # noqa: F401  (pyreadline3 on Windows, if installed)
        readline.set_completer(lambda text, state: ([n for n in names if n.startswith(text)] + [None])[state])
        readline.parse_and_bind("tab: complete")
    except ImportError:
        pass
    print(f"connected to {HOST}:{PORT}; `help` lists the commands; Ctrl+C while a command runs aborts every client's "
          "queue (a running script's too); Ctrl+D leaves")
    try:
        while True:
            try:
                line = input("lss> ").strip()
            except EOFError:
                print()
                break
            except KeyboardInterrupt:
                print()
                continue  # nothing runs: nothing to abort
            if not line or line.startswith("#"):
                continue  # the mod numbers a comment but never answers it
            if line in ("exit", "quit-shell"):
                break
            try:
                console.command(line)
            except KeyboardInterrupt:
                print()
                console.command("abort")
    except (ConnectionError, socket.timeout) as error:
        print(f"the connection ended: {error}")
        return 1
    finally:
        console.close()
    return 0


def run_lines(lines, stop_on_failure: bool) -> int:
    console = connect()
    ok = True
    try:
        for line in lines:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if not console.command(line):
                ok = False
                if stop_on_failure:
                    break
    except (ConnectionError, socket.timeout) as error:
        print(f"the connection ended: {error}")
        ok = False
    console.close()
    return 0 if ok else 1


def cmd_status(args) -> int:
    try:
        console = Console(10.0)
    except OSError:
        print("not listening")
        return 2
    try:
        for key, value in console.values().items():
            print(f"{key} = {value}")
    except (ConnectionError, socket.timeout) as error:
        print(f"the connection ended: {error}")
        return 1
    finally:
        console.close()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-c", dest="command", action="append", help="a line to send (repeatable)")
    sub = parser.add_subparsers(dest="verb")
    sub.add_parser("shell")
    sub.add_parser("run").add_argument("script")
    sub.add_parser("status")
    args = parser.parse_args()
    if args.command:
        return run_lines(args.command, stop_on_failure=True)
    if args.verb == "shell":
        return cmd_shell(args)
    if args.verb == "run":
        text = pathlib.Path(args.script).read_text(encoding="utf-8-sig")
        return run_lines(text.splitlines(), stop_on_failure=True)
    if args.verb == "status":
        return cmd_status(args)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
