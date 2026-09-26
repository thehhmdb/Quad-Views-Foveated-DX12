#!/usr/bin/env python3
# MIT License
#
# Copyright(c) 2021-2023 Matthieu Bucchianeri
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files(the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions :
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""
Single-source version synchronizer for the Quad-Views-Foveated project.

Reads version.info (the canonical source) and updates every file that embeds
a version number, so you only ever edit one file:

    version.info
        major=1
        minor=2
        patch=3

Files updated by this script:
    1. openxr-api-layer/version.h              (C++ constants)
    2. openxr-api-layer/resource.rc            (Windows VERSIONINFO)
    3. CustomSetup/Properties/AssemblyInfo.cs   (.NET assembly version)
    4. installer/installer.vdproj              (MSI ProductVersion)
    5. installer/README.rtf                    (installer splash text, binary RTF)

Usage:
    python update_version.py            # sync all files from version.info
    python update_version.py 1.2.4      # set version.info to 1.2.4, then sync
    python update_version.py --check    # exit non-zero if any file is out of sync

The script is idempotent: files already at the target version are left
untouched (no unnecessary timestamp bumps).
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def read_version_info(path):
    """Parse a key=value version.info file and return (major, minor, patch) as strings."""
    values = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" in line:
                key, _, val = line.partition("=")
                values[key.strip()] = val.strip()
    for key in ("major", "minor", "patch"):
        if key not in values:
            raise RuntimeError("version.info is missing '%s'" % key)
    return values["major"], values["minor"], values["patch"]


def write_version_info(path, major, minor, patch):
    """Write version.info with the given values."""
    content = "major=%s\nminor=%s\npatch=%s\n" % (major, minor, patch)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)


def update_text_file(path, replacements, check_only=False):
    """Apply a list of (regex, replacement) substitutions to a text file.

    Returns True if the file was changed (or would be changed in check mode).
    """
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()

    changed = False
    for pattern, replacement in replacements:
        new_content, count = re.subn(pattern, replacement, content, flags=re.MULTILINE)
        if count > 0 and new_content != content:
            content = new_content
            changed = True

    if changed and not check_only:
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(content)

    return changed


def update_rtf_file(path, new_version, check_only=False):
    """Update the vX.Y.Z token in a binary RTF file (length-safe).

    Returns True if the file was changed (or would be changed in check mode).
    """
    with open(path, "rb") as f:
        data = f.read()

    # Match a version token like "v1.2.3" followed by an RTF control word
    # (backslash) or whitespace. The 'v' prefix avoids matching version-like
    # substrings inside RTF control words.
    pattern = re.compile(rb"v(\d+)\.(\d+)\.(\d+)(?=\\|\s)")
    new_data, count = pattern.subn(new_version.encode("ascii"), data)
    changed = count > 0 and new_data != data

    if changed and not check_only:
        with open(path, "wb") as f:
            f.write(new_data)

    return changed


def sync_all(root, major, minor, patch, check_only=False):
    """Sync all version-embedded files to the given major.minor.patch.

    Returns a list of (file, changed) tuples.
    """
    ver = "%s.%s.%s" % (major, minor, patch)
    ver4 = "%s.0" % ver
    results = []

    # 1. openxr-api-layer/version.h
    path = os.path.join(root, "openxr-api-layer", "version.h")
    patterns = [
        (r"const unsigned int LayerVersionMajor = \d+;",
         "const unsigned int LayerVersionMajor = %s;" % major),
        (r"const unsigned int LayerVersionMinor = \d+;",
         "const unsigned int LayerVersionMinor = %s;" % minor),
        (r"const unsigned int LayerVersionPatch = \d+;",
         "const unsigned int LayerVersionPatch = %s;" % patch),
    ]
    results.append((path, update_text_file(path, patterns, check_only)))

    # 2. openxr-api-layer/resource.rc
    path = os.path.join(root, "openxr-api-layer", "resource.rc")
    # NOTE: the numeric FILEVERSION/PRODUCTVERSION lines are indented in the .rc
    # file, so the pattern must tolerate leading whitespace (and preserve it via
    # the \1 backreference). A bare '^FILEVERSION' anchor never matches.
    patterns = [
        (r'^([ \t]*)FILEVERSION \d+,\d+,\d+,\d+',
         r'\1FILEVERSION %s,%s,%s,0' % (major, minor, patch)),
        (r'^([ \t]*)PRODUCTVERSION \d+,\d+,\d+,\d+',
         r'\1PRODUCTVERSION %s,%s,%s,0' % (major, minor, patch)),
        (r'VALUE "FileVersion", "\d+\.\d+\.\d+\.\d+"',
         'VALUE "FileVersion", "%s"' % ver4),
        (r'VALUE "ProductVersion", "\d+\.\d+\.\d+\.\d+"',
         'VALUE "ProductVersion", "%s"' % ver4),
    ]
    results.append((path, update_text_file(path, patterns, check_only)))

    # 3. CustomSetup/Properties/AssemblyInfo.cs
    path = os.path.join(root, "CustomSetup", "Properties", "AssemblyInfo.cs")
    patterns = [
        (r'\[assembly: AssemblyVersion\("\d+\.\d+\.\d+\.\d+"\)\]',
         '[assembly: AssemblyVersion("%s")]' % ver4),
        (r'\[assembly: AssemblyFileVersion\("\d+\.\d+\.\d+\.\d+"\)\]',
         '[assembly: AssemblyFileVersion("%s")]' % ver4),
    ]
    results.append((path, update_text_file(path, patterns, check_only)))

    # 4. installer/installer.vdproj
    path = os.path.join(root, "installer", "installer.vdproj")
    patterns = [
        (r'"ProductVersion" = "8:\d+\.\d+\.\d+"',
         '"ProductVersion" = "8:%s"' % ver),
    ]
    results.append((path, update_text_file(path, patterns, check_only)))

    # 5. installer/README.rtf (binary RTF)
    path = os.path.join(root, "installer", "README.rtf")
    results.append((path, update_rtf_file(path, "v%s" % ver, check_only)))

    return results


def main():
    root = HERE
    version_info_path = os.path.join(root, "version.info")

    check_only = "--check" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("--")]

    if args:
        # Set version from command line argument: "1.2.4"
        parts = args[0].split(".")
        if len(parts) != 3 or not all(p.isdigit() for p in parts):
            print("Error: version must be in the form MAJOR.MINOR.PATCH (e.g. 1.2.4)")
            return 2
        major, minor, patch = parts
        if not check_only:
            write_version_info(version_info_path, major, minor, patch)
            print("Set version.info to %s.%s.%s" % (major, minor, patch))
    else:
        # Read version from version.info
        major, minor, patch = read_version_info(version_info_path)

    ver = "%s.%s.%s" % (major, minor, patch)
    print("Syncing all files to version %s%s..." % (ver, " (check only)" if check_only else ""))

    results = sync_all(root, major, minor, patch, check_only)

    out_of_sync = False
    for path, changed in results:
        rel = os.path.relpath(path, root)
        if changed:
            if check_only:
                print("  OUT OF SYNC: %s" % rel)
                out_of_sync = True
            else:
                print("  UPDATED:     %s" % rel)
        else:
            print("  ok:          %s" % rel)

    if check_only and out_of_sync:
        print("\nSome files are out of sync. Run: python update_version.py")
        return 1

    print("\nAll files are at version %s." % ver)
    return 0


if __name__ == "__main__":
    sys.exit(main())
