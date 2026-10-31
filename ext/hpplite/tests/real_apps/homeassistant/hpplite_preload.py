# HPPLite into Python's sqlite3, once per process, before anything opens a
# database: later connections whose file HPPLITE_FILES names are captured
import os, sqlite3
_ext = os.environ.get("HPPLITE_EXT")
if _ext:
    _c = sqlite3.connect(":memory:")
    _c.enable_load_extension(True)
    _c.load_extension(os.path.splitext(_ext)[0])
    _c.close()
