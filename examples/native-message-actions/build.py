"""Package the standalone reference example, not LINE MR."""
from pathlib import Path
import zipfile

root = Path(__file__).resolve().parent
out = root / "dist" / "lineNativeActions-0.1.0-test.nvda-addon"
out.parent.mkdir(exist_ok=True)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
	for p in sorted((root / "addon").rglob("*")):
		if p.is_file() and "__pycache__" not in p.parts and p.suffix != ".pyc":
			z.write(p, p.relative_to(root / "addon"))
print(out)
