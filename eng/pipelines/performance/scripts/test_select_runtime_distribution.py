# Licensed to the .NET Foundation under one or more agreements.
# The .NET Foundation licenses this file to you under the MIT license.

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import select_runtime_distribution as producer


class SelectRuntimeDistributionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def fixture(self, os_group="linux", architecture="x64", crossgen=True):
        rid = producer.RIDS[(os_group, architecture)]
        shipping = self.root / rid / "Shipping"
        shipping.mkdir(parents=True)
        extension = "zip" if os_group == "windows" else "tar.gz"
        names = [f"dotnet-runtime-12.0.0-ci-{rid}.{extension}",
                 f"Microsoft.NETCore.App.Runtime.{rid}.12.0.0-ci.nupkg",
                 "Microsoft.NETCore.App.Ref.12.0.0-ci.nupkg",
                 f"Microsoft.NETCore.App.Host.{rid}.12.0.0-ci.nupkg"]
        if crossgen:
            names.append(f"Microsoft.NETCore.App.Crossgen2.{rid}.12.0.0-ci.nupkg")
        for name in names:
            (shipping / name).write_bytes(b"opaque producer payload\0" + name.encode())
        return shipping, names

    def test_all_lanes_select_original_files_without_modifying_them(self):
        for os_group, architecture in producer.RIDS:
            with self.subTest(os_group=os_group, architecture=architecture):
                shipping, names = self.fixture(os_group, architecture)
                originals = {path: path.read_bytes() for path in shipping.iterdir()}
                files = producer.select_files(shipping, os_group, architecture)
                self.assertEqual(files, [shipping / name for name in names])
                self.assertEqual({path: path.read_bytes() for path in shipping.iterdir()}, originals)

    def test_optional_crossgen_absent_and_symbols_unrelated_files_ignored(self):
        shipping, names = self.fixture(crossgen=False)
        for name in names[1:]:
            (shipping / name.replace(".nupkg", ".symbols.nupkg")).write_bytes(b"symbols")
        (shipping / "Microsoft.NETCore.App.Crossgen2.linux-x64.12.0.0-ci.symbols.nupkg").write_bytes(b"symbols")
        (shipping / "dotnet-runtime-12.0.0-ci-linux-arm64.tar.gz").write_bytes(b"other RID")
        (shipping / "dotnet-runtime-12.0.0-ci-linux-x64.zip").write_bytes(b"other format")
        self.assertEqual(producer.select_files(shipping, "linux", "x64"), [shipping / name for name in names])

    def test_each_required_file_must_exist(self):
        shipping, names = self.fixture()
        for name in names[:4]:
            with self.subTest(name=name):
                path = shipping / name
                data = path.read_bytes()
                path.unlink()
                with self.assertRaisesRegex(ValueError, "exactly one"):
                    producer.select_files(shipping, "linux", "x64")
                path.write_bytes(data)

    def test_ambiguous_required_or_optional_file_fails(self):
        shipping, names = self.fixture()
        for name in names:
            with self.subTest(name=name):
                extra = shipping / name.replace("12.0.0-ci", "12.0.0-dev")
                extra.write_bytes(b"second version")
                with self.assertRaisesRegex(ValueError, "found 2"):
                    producer.select_files(shipping, "linux", "x64")
                extra.unlink()

    def test_empty_or_directory_output_fails(self):
        shipping, names = self.fixture()
        path = shipping / names[0]
        path.write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "nonempty file"):
            producer.select_files(shipping, "linux", "x64")
        path.unlink()
        path.mkdir()
        with self.assertRaisesRegex(ValueError, "nonempty file"):
            producer.select_files(shipping, "linux", "x64")

    def test_missing_shipping_or_unknown_lane_fails(self):
        with self.assertRaisesRegex(ValueError, "Shipping directory does not exist"):
            producer.select_files(self.root / "missing", "linux", "x64")
        with self.assertRaisesRegex(ValueError, "Unsupported ordinary CoreCLR lane"):
            producer.select_files(self.root, "linux", "x86")

    def test_cli_success_and_missing_input_error(self):
        shipping, names = self.fixture()
        command = [sys.executable, producer.__file__, "--shipping-dir", str(shipping),
                   "--os-group", "linux", "--architecture", "x64"]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "##vso[task.setvariable variable=RuntimeDistributionFiles]" + "%0A".join(names) + "\n")
        command[3] = str(self.root / "missing")
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Shipping directory does not exist", result.stderr)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
