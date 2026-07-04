import base64
import os
import unittest

import numpy as np

from serve.manifest import parse_manifest_text
from serve.session import decode_images


class ServeShellTests(unittest.TestCase):
    def test_manifest_env_and_nested_scalars(self):
        os.environ["PI05_CHECKPOINT"] = "/tmp/pi05"
        data = parse_manifest_text(
            """
model:
  checkpoint: $PI05_CHECKPOINT
  precision: fp16
  num_views: 3
serve:
  transport: act_http
  port: 8080
"""
        )
        self.assertEqual(data["model"]["checkpoint"], "/tmp/pi05")
        self.assertEqual(data["model"]["num_views"], 3)
        self.assertEqual(data["serve"]["port"], 8080)

    def test_decode_images_accepts_base64_rgb(self):
        frame = np.zeros((224, 224, 3), dtype=np.uint8)
        payload = [base64.b64encode(frame.tobytes()).decode()]
        out = decode_images(payload, 1)
        self.assertEqual(out[0].shape, (224, 224, 3))
        self.assertTrue(out[0].flags["C_CONTIGUOUS"])


if __name__ == "__main__":
    unittest.main()
