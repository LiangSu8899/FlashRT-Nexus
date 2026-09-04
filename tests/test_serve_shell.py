import base64
import os
import tempfile
import unittest
from pathlib import Path

import numpy as np

from flashrt_nexus.library import find_library
from serve.manifest import load_manifest, parse_manifest_text
from serve.session import (
    decode_images,
    normalize_image_arrays,
    sanitize_capsule_id,
)


class ServeShellTests(unittest.TestCase):
    def test_nested_json_and_inline_arrays(self):
        self.assertEqual(parse_manifest_text('{"views": [1, 2]}'), {"views": [1, 2]})
        self.assertEqual(parse_manifest_text('views: ["base", "wrist"]'),
                         {"views": ["base", "wrist"]})

    def test_manifest_mapping_is_copied(self):
        os.environ["NEXUS_TEST_CONFIG"] = "pi05"
        source = {"model": {"config": "$NEXUS_TEST_CONFIG"}}
        loaded = load_manifest(source)
        self.assertEqual(loaded["model"]["config"], "pi05")
        loaded["model"]["config"] = "changed"
        self.assertEqual(source["model"]["config"], "$NEXUS_TEST_CONFIG")

    def test_explicit_library_is_resolved(self):
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory) / "libnexus.so"
            library.touch()
            self.assertEqual(find_library(library), str(library.resolve()))

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

    def test_normalize_image_arrays_rejects_non_rgb(self):
        good = [np.zeros((224, 224, 3), dtype=np.uint8)]
        self.assertEqual(normalize_image_arrays(good, 1)[0].shape,
                         (224, 224, 3))
        bad = [np.zeros((224, 224), dtype=np.uint8)]
        with self.assertRaises(ValueError):
            normalize_image_arrays(bad, 1)

    def test_capsule_id_rejects_paths(self):
        self.assertEqual(sanitize_capsule_id("episode-001"), "episode-001")
        for bad in ("../episode", "/tmp/episode", "", "a/b", "x" * 129):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    sanitize_capsule_id(bad)


if __name__ == "__main__":
    unittest.main()
