#!/usr/bin/env python3
# Licensed to the .NET Foundation under one or more agreements.
# The .NET Foundation licenses this file to you under the MIT license.

"""Select existing producer packages for the pipeline's CopyFiles task."""

import argparse
from pathlib import Path


RIDS = {
    ("linux", "x64"): "linux-x64",
    ("linux", "arm64"): "linux-arm64",
    ("windows", "x64"): "win-x64",
    ("windows", "x86"): "win-x86",
    ("windows", "arm64"): "win-arm64",
}


def select(shipping, pattern, optional=False):
    matches = sorted(path for path in shipping.glob(pattern) if not path.name.endswith(".symbols.nupkg"))
    if len(matches) > 1 or (not matches and not optional):
        count = "at most one" if optional else "exactly one"
        raise ValueError(f"Expected {count} {pattern} in {shipping}; found {len(matches)}.")
    for path in matches:
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"Producer output is not a nonempty file: {path}")
    return matches


def select_files(shipping, os_group, architecture):
    rid = RIDS.get((os_group, architecture))
    if rid is None:
        raise ValueError(f"Unsupported ordinary CoreCLR lane: {os_group}/{architecture}")
    shipping = Path(shipping)
    if not shipping.is_dir():
        raise ValueError(f"Shipping directory does not exist: {shipping}")
    extension = "zip" if os_group == "windows" else "tar.gz"
    patterns = [
        f"dotnet-runtime-*-{rid}.{extension}",
        f"Microsoft.NETCore.App.Runtime.{rid}.*.nupkg",
        "Microsoft.NETCore.App.Ref.*.nupkg",
        f"Microsoft.NETCore.App.Host.{rid}.*.nupkg",
    ]
    files = [path for pattern in patterns for path in select(shipping, pattern)]
    files += select(shipping, f"Microsoft.NETCore.App.Crossgen2.{rid}.*.nupkg", optional=True)
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shipping-dir", type=Path, required=True)
    parser.add_argument("--os-group", required=True)
    parser.add_argument("--architecture", required=True)
    args = parser.parse_args()
    try:
        files = select_files(args.shipping_dir, args.os_group, args.architecture)
    except (ValueError, OSError) as error:
        parser.exit(1, f"Cannot select runtime distribution: {error}\n")
    # Azure Pipelines decodes escaped newlines into CopyFiles' multiline Contents input.
    contents = "\n".join(path.name for path in files).replace("%", "%AZP25").replace("\r", "%0D").replace("\n", "%0A")
    print(f"##vso[task.setvariable variable=RuntimeDistributionFiles]{contents}")


if __name__ == "__main__":
    main()
