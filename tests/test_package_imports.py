"""Implementation and public modules must import in either order."""

import subprocess
import sys

import pytest


@pytest.mark.parametrize('module', ['serve.deployment', 'serve.library',
                                  'flashrt_nexus', 'flashrt_nexus.library'])
def test_fresh_import(module):
    subprocess.run([sys.executable, '-c', f'import {module}'], check=True)
