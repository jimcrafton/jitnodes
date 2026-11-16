"""Runs the Python tests in this folder against the built module.   python runScripts.py <folder holding jitnodes.pyd>"""

import os
import sys
import unittest

sys.path.insert(0, sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.discover(here, pattern="test_*.py"))
sys.exit(0 if result.wasSuccessful() else 1)
