"""Drive the LocalSplitScreen mod's console from the host (a dev build: LSS_DEV_TOOLS).

    python tools/remote.py shell                    an interactive console (tab completes commands with pyreadline3)
    python tools/remote.py -c join -c "wait-until p2.state == playing"
                                                    send lines, print every reply, exit 1 on a failure
    python tools/remote.py run script.txt           send a file line by line, stopping at the first failure
    python tools/remote.py status                   every readable value (exit 2 if the mod is not listening)

    --timeout S        the longest wait for any one reply (default 120; a script's longest `wait` needs more)
    --connect-wait S   keep trying to connect for S seconds (the game starting up); default: one try

Exit status: 0 every line ok, 1 a line failed or was rejected (or the connection ended), 2 no reply within the timeout
or the mod not listening.

The mod listens on 127.0.0.1:4419 (src/control/remote.h): one command per line, UTF-8; every reply is one line
starting with ok, failed, rejected, dropped, value, help or note (src/control/runner.h). `help` lists the commands
the running build registered. Blank lines and # comments in a script are not sent. A script is one client: its
queue, waits and holds are its own, and when it ends (the connection closes) anything it still holds is released, so
a script waits out its holds (`hold ... 180` then `wait 200`). `abort` drops every client's queue, so a script may not
carry one (`run` refuses the file); it is the shell's, typed while something runs.

Taken from Remake's tools/game.py (2026-10-23), its console part only: launching, quitting and deploying stay with
tools/regression. Changed: the port (Remake's 4418 plus one, so both games' consoles can listen at once); `run`
reads a UTF-8 file with or without a byte-order mark; the shell skips # lines (the mod never answers a comment, so
Remake's shell waited out its timeout) and Ctrl+C at an empty prompt aborts nothing; a lost connection is reported,
not a traceback. It replaces the file inbox's tools/control.py (the inbox retired with the harness's move, phase 8 of
the cleanup). As the inbox did, `run` checks a script before any line runs, but by its commands' names only (against
the running build's `help`): a bad argument still stops the script at its line. `run` sends the comments and blank
lines too (the mod numbers them and never answers), so a reply's line number is the script's file line.
"""
import argparse
import pathlib
import socket
import sys
import time

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


def connect(timeout: float = 120.0, wait: float = 0.0) -> Console:
    deadline = time.monotonic() + wait
    while True:
        try:
            return Console(timeout)
        except OSError:
            if time.monotonic() >= deadline:
                print(f"the mod is not listening on {HOST}:{PORT} (is the game running with a dev build of the mod?)")
                sys.exit(2)
            time.sleep(1.0)


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


# The script words the runner itself answers (control/script.h); every other first word is a registered command.
SCRIPT_WORDS = {"get", "status", "help", "wait", "wait-until", "hold", "note"}


def unknown_lines(lines, timeout: float, wait: float) -> list:
    """The lines whose first word the running build does not know, as (file line, word); asked on a connection of its
    own, so the script's line numbers start at 1."""
    console = connect(timeout, wait)
    try:
        known = set(console.names()) | SCRIPT_WORDS
    finally:
        console.close()
    out = []
    for number, line in enumerate(lines, 1):
        words = line.split()
        if words and not words[0].startswith("#") and words[0] not in known:
            out.append((number, words[0]))
    return out


def run_lines(lines, stop_on_failure: bool, timeout: float, wait: float) -> int:
    console = connect(timeout, wait)
    code = 0
    try:
        for line in lines:
            line = line.strip()
            if not line or line.startswith("#"):
                console.send(line)  # numbered by the mod, never answered: the replies keep the file's line numbers
                continue
            if not console.command(line):
                code = 1
                if stop_on_failure:
                    break
    except socket.timeout:
        print(f"no reply within {timeout:.0f} s")
        code = 2
    except ConnectionError as error:
        print(f"the connection ended: {error}")
        code = 1
    finally:
        console.close()
    return code


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
    parser.add_argument("--timeout", type=float, default=120.0, help="the longest wait for any one reply, seconds")
    parser.add_argument("--connect-wait", type=float, default=0.0, help="keep trying to connect this long, seconds")
    sub = parser.add_subparsers(dest="verb")
    sub.add_parser("shell")
    sub.add_parser("run").add_argument("script")
    sub.add_parser("status")
    args = parser.parse_args()
    if args.command:
        return run_lines(args.command, True, args.timeout, args.connect_wait)
    if args.verb == "shell":
        return cmd_shell(args)
    if args.verb == "run":
        lines = pathlib.Path(args.script).read_text(encoding="utf-8-sig").splitlines()
        aborts = [n for n, line in enumerate(lines, 1) if line.split()[:1] == ["abort"]]
        if aborts:
            print(f"line {aborts[0]}: abort is the shell's (it drops every client's queue); a script stops at its "
                  "first failure")
            return 1
        try:
            unknown = unknown_lines(lines, args.timeout, args.connect_wait)
        except (ConnectionError, socket.timeout) as error:
            print(f"the connection ended: {error}")
            return 1
        if unknown:
            for number, word in unknown:
                print(f"line {number}: unknown command '{word}'")
            print("nothing ran")
            return 1
        return run_lines(lines, True, args.timeout, args.connect_wait)
    if args.verb == "status":
        return cmd_status(args)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
