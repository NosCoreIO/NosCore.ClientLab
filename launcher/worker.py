"""Private worker protocol for the C# desktop shell (JSON handshake on stdout)."""
import argparse
import json
from pathlib import Path
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from launcher.core import Launcher
from launcher.service import create_server


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", required=True)
    args = parser.parse_args()
    app = Launcher(args.data_dir)
    server = create_server(app)
    print(json.dumps({"url": f"http://127.0.0.1:{server.server_address[1]}/", "token": server.api_token}), flush=True)
    try:
        # The parent keeps stdin open; closing the desktop shell closes the pipe.
        for _ in sys.stdin:
            pass
    finally:
        app.cancel()
        app.close_game()
        server.shutdown()


if __name__ == "__main__":
    main()
