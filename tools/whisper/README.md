# Speech models for voice message transcription

When the user turns on "Transcribe voice messages", the add-on downloads whisper.cpp and the speech
model chosen in the settings (`addon/appModules/line/speechModel.py` holds every file's name, size
and SHA-256). The program and the Typhoon models are assets of the GitHub release `whisper-1`;
OpenAI's Whisper models come from huggingface.co/ggerganov/whisper.cpp (whisper.cpp's own files;
their SHA-256 is the Git LFS oid that the Hugging Face API lists). A new program build needs a new
release tag and `SET_ID`; new models can be added to the release.

## lineMR-whisper-x64.zip

whisper.cpp v1.9.5 `whisper-cli.exe` and its DLLs, with the C runtime linked in (the official
`whisper-bin-x64.zip` needs the Visual C++ runtime: MSVCP140, VCRUNTIME140, VCOMP140) and without
OpenMP. The CPU-specific `ggml-cpu-*.dll` variants are kept; ggml picks one at run time.

```
git clone --depth 1 --branch v1.9.5 https://github.com/ggml-org/whisper.cpp
# in a "x64 Native Tools" prompt; a short build path (Ninja hits MAX_PATH under %TEMP%):
cmake -S whisper.cpp -B C:\wbld -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_POLICY_DEFAULT_CMP0091=NEW ^
  -DBUILD_SHARED_LIBS=ON -DGGML_BACKEND_DL=ON -DGGML_CPU_ALL_VARIANTS=ON -DGGML_NATIVE=OFF ^
  -DGGML_OPENMP=OFF -DWHISPER_BUILD_TESTS=OFF -DWHISPER_SDL2=OFF
cmake --build C:\wbld --target whisper-cli
```

Zip `C:\wbld\bin\*.exe`, `*.dll` (flat) plus whisper.cpp's `LICENSE` as `LICENSE-whisper.cpp.txt`.
`dumpbin /dependents` should list only Windows DLLs and the zip's own.

## Typhoon models

SCB 10X's Typhoon Whisper models (MIT): Whisper trained further on ~11,000 hours of Thai.

| Release file | Source | Quantization |
|---|---|---|
| `ggml-typhoon-whisper-medium-q8_0.bin` | [typhoon-ai/typhoon-whisper-medium](https://huggingface.co/typhoon-ai/typhoon-whisper-medium) (f32, 3.1 GB) | q8_0 |
| `ggml-typhoon-whisper-turbo-q8_0.bin` | [typhoon-ai/typhoon-whisper-turbo](https://huggingface.co/typhoon-ai/typhoon-whisper-turbo) (bf16, 1.6 GB) | q8_0 |
| `ggml-typhoon-whisper-large-v3-q5_0.bin` | [typhoon-ai/typhoon-whisper-large-v3](https://huggingface.co/typhoon-ai/typhoon-whisper-large-v3) (f32, two shards, 6.2 GB) | q5_0 |

1. Download `config.json`, `vocab.json` and `model.safetensors` (or the shards and
   `model.safetensors.index.json`) into one folder, and `whisper/assets/mel_filters.npz` from
   github.com/openai/whisper into the same folder (it holds the 80 and the 128 mel filters).
2. `python convert_hf_to_ggml.py <folder> ggml-f16.bin` (needs only numpy; a port of whisper.cpp's
   `models/convert-h5-to-ggml.py` without torch; reads one tensor at a time).
3. `whisper-quantize ggml-f16.bin <release file> q8_0` (or `q5_0`; the tool is in whisper.cpp's
   release zip, or build the `whisper-quantize` target).

Measured on an i5-1235U (10 threads): an 8 s English clip in 30 s with the full 30 s window, a 7 s
Thai voice message in 8-10 s with `-ac 400` (the add-on shortens the window for recordings under
29 s). For Turbo, q5_0 (574 MB) was slower than q8_0 (874 MB) here, f16 slower still; Large v3 is
q5_0 to keep its memory use near the others'.

## Adding a model to the list

The add-on reads the list of models from `speech-models.json` at the root of this repository (main
branch, fetched from raw.githubusercontent.com when NVDA starts and when the settings or the dialog
before a transcription open), so a new model shows to every user without an add-on update. Add an
entry and push to main:

- `id`: lowercase letters, digits, `.`, `_`, `-` (not `custom`); kept in the user's settings, so
  never reuse or rename one.
- `name`: shown as is (not translated).
- `file`: the file name on disk (`*.bin`); `url` must end with `/<file>` and start with
  `https://github.com/Peetchy/line-mr-for-win/releases/download/` or `https://huggingface.co/`.
- `sha256` (lowercase hex) and `size` (bytes): checked after the download. For Hugging Face, the
  LFS oid of the tree API is the SHA-256.
- `strength`: `bestForThai`, `thaiOnly`, `thaiOnlyAccurate` or `manyLanguages` (the add-on has the
  translated words); or `note`: `{"en": "...", "th": "..."}` with words of its own (at most 120
  characters each).
- `speed` (optional): first guess of seconds of work per second of audio (Turbo 3, Large v3 16).

An entry that breaks a rule makes the add-on ignore the whole file and keep its last good copy (or
its built-in list), so check it with a JSON validator first. Models the add-on knows from its own
list (`BUILTIN` in `addon/appModules/line/speechModel.py`) stay offered even if the file drops them.
