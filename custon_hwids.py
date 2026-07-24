import glob
import json
import os

Import("env")

board_config = env.BoardConfig()
board_config.update("frameworks", ["arduino", "espidf"])

# Work around a pioarduino/platform-espressif32 packaging bug: the riscv32
# toolchain package is not reliably added to PATH by LoadPioPlatform(), even
# though it gets installed correctly on disk. Locate it directly by its
# package.json "name" and prepend its bin dir ourselves.
platform = env.PioPlatform()
for pkg_json in glob.glob(os.path.join(str(platform.packages_dir), "*", "package.json")):
    try:
        with open(pkg_json) as f:
            name = json.load(f).get("name")
    except (OSError, ValueError):
        continue
    if name == "riscv32-esp-elf":
        env.PrependENVPath("PATH", os.path.join(os.path.dirname(pkg_json), "bin"))
        break