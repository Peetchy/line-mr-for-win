"""Makes lineMR-whisper-x64.zip for a new program release: whisper.cpp's files from an earlier
release's zip plus lineMR_audio.exe, and prints the PROGRAM_ZIP line for speechModel.py.

Usage: python tools/whisper/make_program_zip.py <earlier zip> <lineMR_audio.exe> <output zip>

The earlier zip is checked against its SHA-256 (the one speechModel.py had for it), so only the
whisper.cpp build that was published goes into the new zip. Take lineMR_audio.exe from the
"lineMR-audio-helper" artifact of the build workflow, not from a local build.
"""

import hashlib
import os
import sys
import zipfile

#: whisper.cpp v1.9.5 build of release "whisper-1".
EARLIER_SHA256 = "928347788f7d29fb643519a016f41838e8c0fd71f6747ddb6096959de6deac4e"
HELPER = "lineMR_audio.exe"


def sha256(path):
	h = hashlib.sha256()
	with open(path, "rb") as f:
		for chunk in iter(lambda: f.read(1 << 20), b""):
			h.update(chunk)
	return h.hexdigest()


def main():
	if len(sys.argv) != 4:
		sys.exit(__doc__)
	earlier, helper, output = sys.argv[1:]
	if sha256(earlier) != EARLIER_SHA256:
		sys.exit(f"{earlier} is not the published whisper-1 zip (SHA-256 differs)")
	if os.path.basename(helper).lower() != HELPER.lower():
		sys.exit(f"expected {HELPER}, got {helper}")
	with zipfile.ZipFile(earlier) as src, zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as dst:
		for info in src.infolist():
			if info.filename.lower() == HELPER.lower():
				continue
			dst.writestr(info, src.read(info))
		dst.write(helper, HELPER)
	print(f'PROGRAM_ZIP = ("lineMR-whisper-x64.zip", "{sha256(output)}", {os.path.getsize(output)})')


if __name__ == "__main__":
	main()
