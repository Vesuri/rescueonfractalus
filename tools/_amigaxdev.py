# Find AmigaXDev's shared Python package (amigaxdev) under any python3.
import os, sys
sys.path.insert(0, os.environ.get('AMIGA_PYLIB') or os.path.expanduser('~/.local/share/amiga/pylib'))
