import importlib.metadata
from pathlib import Path
import sys

import pylibseekdb as seekdb

assert importlib.metadata.version("pylibseekdb") == sys.argv[1]
assert seekdb.__version__ == sys.argv[1]
assert Path(seekdb.__file__).is_relative_to(Path(sys.prefix))
instance = seekdb.open(str(Path.cwd() / "database"))
try:
    connection = instance.connect("test")
    try:
        cursor = connection.cursor()
        try:
            cursor.execute("CREATE TABLE smoke_test (value INT)")
            cursor.execute("INSERT INTO smoke_test VALUES (42)")
            connection.commit()
            cursor.execute("SELECT value FROM smoke_test")
            assert cursor.fetchone()[0] == 42
        finally:
            cursor.close()
    finally:
        connection.close()
finally:
    instance.close()
print("seekdb installed wheel smoke passed")
