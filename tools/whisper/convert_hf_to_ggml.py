# Hugging Face Whisper (safetensors, bf16/f16/f32) -> whisper.cpp ggml f16, numpy only.
# Port of whisper.cpp models/convert-h5-to-ggml.py without torch / transformers.
# Usage: convert_hf_to_ggml.py <hf dir with model.safetensors (or shards + index), vocab.json, config.json, mel_filters.npz> <out.bin>
import json
import struct
import sys
from pathlib import Path

import numpy as np

conv_map = {
	'self_attn.k_proj': 'attn.key',
	'self_attn.q_proj': 'attn.query',
	'self_attn.v_proj': 'attn.value',
	'self_attn.out_proj': 'attn.out',
	'self_attn_layer_norm': 'attn_ln',
	'encoder_attn.q_proj': 'cross_attn.query',
	'encoder_attn.v_proj': 'cross_attn.value',
	'encoder_attn.out_proj': 'cross_attn.out',
	'encoder_attn_layer_norm': 'cross_attn_ln',
	'fc1': 'mlp.0',
	'fc2': 'mlp.2',
	'final_layer_norm': 'mlp_ln',
	'encoder.layer_norm.bias': 'encoder.ln_post.bias',
	'encoder.layer_norm.weight': 'encoder.ln_post.weight',
	'encoder.embed_positions.weight': 'encoder.positional_embedding',
	'decoder.layer_norm.bias': 'decoder.ln.bias',
	'decoder.layer_norm.weight': 'decoder.ln.weight',
	'decoder.embed_positions.weight': 'decoder.positional_embedding',
	'decoder.embed_tokens.weight': 'decoder.token_embedding.weight',
	'proj_out.weight': 'decoder.proj.weight',
}


def bytes_to_unicode():
	bs = list(range(ord("!"), ord("~") + 1)) + list(range(ord("¡"), ord("¬") + 1)) + list(range(ord("®"), ord("ÿ") + 1))
	cs = bs[:]
	n = 0
	for b in range(2 ** 8):
		if b not in bs:
			bs.append(b)
			cs.append(2 ** 8 + n)
			n += 1
	return dict(zip(bs, [chr(c) for c in cs]))


def load(path):
	f = open(path, "rb")
	n = struct.unpack("<Q", f.read(8))[0]
	header = json.loads(f.read(n))
	base = 8 + n
	for name, info in header.items():
		if name == "__metadata__":
			continue
		start, end = info["data_offsets"]
		f.seek(base + start)
		raw = np.frombuffer(f.read(end - start), dtype=np.uint16 if info["dtype"] in ("BF16", "F16") else np.float32)
		if info["dtype"] == "BF16":
			data = (raw.astype(np.uint32) << 16).view(np.float32)
		elif info["dtype"] == "F16":
			data = raw.view(np.float16).astype(np.float32)
		else:
			data = raw
		yield name, data.reshape(info["shape"])


src, out = Path(sys.argv[1]), Path(sys.argv[2])
hp = json.load((src / "config.json").open(encoding="utf8"))
max_length = hp.get("max_length") or hp.get("max_target_positions", 448)
tokens = json.load((src / "vocab.json").open(encoding="utf8"))
filters = np.load(src / "mel_filters.npz")[f"mel_{hp['num_mel_bins']}"]

fout = open(out, "wb")
for v in (0x67676d6c, hp["vocab_size"], hp["max_source_positions"], hp["d_model"], hp["encoder_attention_heads"],
		hp["encoder_layers"], max_length, hp["d_model"], hp["decoder_attention_heads"], hp["decoder_layers"],
		hp["num_mel_bins"], 1):
	fout.write(struct.pack("i", v))
fout.write(struct.pack("ii", *filters.shape))
fout.write(filters.astype(np.float32).tobytes())
byte_decoder = {v: k for k, v in bytes_to_unicode().items()}
fout.write(struct.pack("i", len(tokens)))
for key, _ in sorted(tokens.items(), key=lambda x: x[1]):
	text = bytes(byte_decoder[c] for c in key)
	fout.write(struct.pack("i", len(text)))
	fout.write(text)

def tensors(src):
	"""model.safetensors, or the shards listed in model.safetensors.index.json (large models)."""
	if (src / "model.safetensors").exists():
		yield from load(src / "model.safetensors")
		return
	index = json.load((src / "model.safetensors.index.json").open(encoding="utf8"))
	for shard in sorted(set(index["weight_map"].values())):
		yield from load(src / shard)


count = 0
for name, data in tensors(src):
	if name == "proj_out.weight":
		continue
	nn = name.split(".")[1:]
	if nn[1] == "layers":
		nn[1] = "blocks"
		inner = ".".join(nn[3:-1])
		if inner == "encoder_attn.k_proj":
			mapped = "attn.key" if nn[0] == "encoder" else "cross_attn.key"
		else:
			mapped = conv_map[inner]
		mname = ".".join(nn[:3] + [mapped] + nn[-1:])
	else:
		mname = ".".join(nn)
		mname = conv_map.get(mname, mname)
	data = data.squeeze()
	if mname in ("encoder.conv1.bias", "encoder.conv2.bias"):
		data = data.reshape(data.shape[0], 1)
	n_dims = len(data.shape)
	if n_dims < 2 or mname in ("encoder.conv1.bias", "encoder.conv2.bias", "encoder.positional_embedding", "decoder.positional_embedding"):
		data, ftype = data.astype(np.float32), 0
	else:
		data, ftype = data.astype(np.float16), 1
	s = mname.encode()
	fout.write(struct.pack("iii", n_dims, len(s), ftype))
	for i in range(n_dims):
		fout.write(struct.pack("i", data.shape[n_dims - 1 - i]))
	fout.write(s)
	fout.write(np.ascontiguousarray(data).tobytes())
	count += 1
fout.close()
print("tensors", count, "->", out)
