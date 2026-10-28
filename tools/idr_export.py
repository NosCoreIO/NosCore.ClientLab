"""Drive IDR (GUI-only) to analyse a Delphi exe and export its MAP and IDC symbol files."""
import os
import subprocess
import sys
import time

from pywinauto import Application, Desktop


def wait_until(predicate, timeout, interval=2):
    end = time.time() + timeout
    while time.time() < end:
        if predicate():
            return True
        time.sleep(interval)
    raise TimeoutError()


def visible(app, class_name):
    return any(w.class_name() == class_name and w.is_visible() for w in app.windows(visible_only=False))


def wait_file_stable(path, app, timeout):
    def stable():
        if not os.path.exists(path) or os.path.getsize(path) == 0:
            return False
        size = os.path.getsize(path)
        time.sleep(5)
        return os.path.getsize(path) == size and not visible(app, "TFProgressBar")

    wait_until(stable, timeout)


def save_as(app, path):
    dialog = app.window(title="Save As", class_name="#32770")
    dialog.wait("visible", timeout=60)
    dialog.child_window(class_name="Edit", found_index=0).set_edit_text(path)
    dialog.child_window(title="&Save", class_name="Button").click()


def main():
    idr, exe = sys.argv[1], os.path.abspath(sys.argv[2])
    base = os.path.splitext(exe)[0]
    for ext in (".map", ".idc"):
        if os.path.exists(base + ext):
            os.remove(base + ext)

    proc = subprocess.Popen([idr, exe], cwd=os.path.dirname(idr))
    app = Application(backend="win32").connect(process=proc.pid, timeout=60)
    main_window = app.window(class_name="TFMain_11011981")
    main_window.wait("exists", timeout=60)
    wait_until(lambda: "(Delphi" in main_window.window_text(), 120)
    time.sleep(10)
    wait_until(lambda: not visible(app, "TFProgressBar"), 3600, 10)

    for item, ext in (("MAP Generator", ".map"), ("IDC Generator", ".idc")):
        app.window(class_name="TApplication").restore()
        main_window.wait("visible", timeout=30)
        main_window.menu_select("Tools->" + item)
        save_as(app, base + ext)
        wait_file_stable(base + ext, app, 1800)
        print(f"{item}: {base + ext} ({os.path.getsize(base + ext)} bytes)")

    proc.kill()


if __name__ == "__main__":
    main()
