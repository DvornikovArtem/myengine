"""MaleCNS v1.0 connectome -> .fbrain (docs/flybrain/flybrain.md).

    python convert_malecns.py --source <folder> --out assets/flybrain/malecns_v1.fbrain [--select traced|superclass]

Two sources of the same release (CC BY 4.0), found by the file names in --source:
  - the HuggingFace mirror QuixiAI/MaleCNS (fetch_malecns.ps1 names its files hf-model.safetensors, hf-neurons.parquet,
    hf-neurotransmitters.parquet): the edge list of all 88 million bodies as safetensors, read through np.memmap;
  - the Janelia flat connectome (three feather files under their release names).

Neurons: status == Traced (164,789; --select traced, the default), every edge between two of them without a weight
threshold (25,563,197 edges, 124,025,046 contacts; the converter stops when the numbers differ). --select superclass keeps
what DOOMFLY (github.com/nftechie/doomfly, MIT, Alex Wormuth) keeps: every body with a neuronal superclass that is not glia.
The sign of a synapse comes from the predicted transmitter of the presynaptic neuron, as in doom/transmitters.py of
DOOMFLY: acetylcholine +1; GABA, glutamate, histamine -1; anything else (dopamine, octopamine, serotonin, unclear, missing,
two fast signs) +1.

The layout of the file is game/flybrain/src/FbrainFormat.h; keep both in sync. Needs pyarrow and numpy only.
"""

import argparse
import hashlib
import json
import struct
import sys
import time
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.feather as feather
import pyarrow.ipc as ipc
import pyarrow.parquet as pq

VERSION = 1
ALIGNMENT = 16
MAX_WEIGHT = 32767
NO_HEX = -32768

# Janelia flat connectome (gs://flyem-male-cns/v1.0/connectome-data/flat-connectome/)
FILES = {
    "annotations": "body-annotations-male-cns-v1.0-minconf-0.5.feather",
    "neurotransmitters": "body-neurotransmitters-male-cns-v1.0.feather",
    "edges": "connectome-weights-male-cns-v1.0-minconf-0.5.feather",
}

# HuggingFace mirror QuixiAI/MaleCNS (CC BY 4.0, the same release repackaged without changes), local names of fetch_malecns.ps1
HF_FILES = {
    "model": "hf-model.safetensors",
    "neurons": "hf-neurons.parquet",
    "neurotransmitters": "hf-neurotransmitters.parquet",
}

# status == Traced of MaleCNS v1.0 (README of QuixiAI/MaleCNS): neurons, edges between them, contacts
TRACED_SUBGRAPH = {"neurons": 164789, "edges": 25563197, "contacts": 124025046}

SIDES = {"L": 1, "R": 2, "M": 3, "C": 3, "CENTER": 3, "MIDLINE": 3, "LEFT": 1, "RIGHT": 2}
TRANSMITTERS = {"acetylcholine": 1, "gaba": 2, "glutamate": 3, "histamine": 4, "dopamine": 5, "octopamine": 6,
                "serotonin": 7, "unclear": 8}

ATTRIBUTION = """MaleCNS v1.0 connectome (Janelia FlyEM, University of Cambridge, MRC LMB, Google Research), released 08.06.2026.
Berg S. et al. (2026) Sexual dimorphism in the complete Drosophila male central nervous system connectome. Cell 189(18):5504-5526.e15. doi:10.1016/j.cell.2026.08.015
License: CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/). Release: https://male-cns.janelia.org/download/ (flat-connectome, minconf 0.5).
Read from: {source}
Changed by myengine (tools/flybrain/convert_malecns.py): {selection}; edges converted to CSR; weight = synaptic contacts x sign of the predicted neurotransmitter of the presynaptic neuron (ACh +, GABA/Glu/His -, other +); annotations reduced to type, class, superclass, side, transmitter, position, optic lobe column.
Signs follow DOOMFLY (github.com/nftechie/doomfly, MIT, Alex Wormuth)."""

SOURCES = {
    "hf": "HuggingFace mirror QuixiAI/MaleCNS (Hartford E. 2026, doi:10.57967/hf/10410), the release repackaged without changes",
    "janelia": "Janelia flat connectome files",
}
SELECTIONS = {
    "traced": "neurons with status Traced only",
    "superclass": "bodies with a neuronal superclass that are not glia only",
}


def log(message):
    print(message, flush=True)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while chunk := handle.read(16 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def column(table, *names, required=True):
    for name in names:
        if name in table.column_names:
            return table.column(name)
    if required:
        raise SystemExit(f"none of the columns {names} is in the table; columns: {table.column_names}")
    return None


def strings_of(chunked):
    return ["" if value is None else str(value) for value in chunked.to_pylist()]


def side_of(text):
    return SIDES.get(text.strip().upper(), 0) if text else 0


def transmitter_sign(text):
    """+1 / -1 and whether the sign is a declared guess (DOOMFLY transmitter_signs, ambiguous_sign = +1)."""
    tokens = set(text.lower().replace(" ", "").split(",")) if text else set()
    fast = ({1} if "acetylcholine" in tokens else set()) | ({-1} if tokens & {"gaba", "glutamate", "histamine"} else set())
    if len(fast) != 1:
        return 1, True
    return next(iter(fast)), False


def transmitter_code(text):
    tokens = [t for t in text.lower().replace(" ", "").split(",") if t] if text else []
    if not tokens:
        return 0
    return TRANSMITTERS.get(tokens[0], 8)


def positions_of(table, count):
    """Soma position if annotated, else another point of the body; (0, 0, 0) when there is none."""
    xyz = np.zeros((count, 3), dtype=np.float32)
    found = np.zeros(count, dtype=bool)
    used = []
    for name in ("somaLocation", "rootLocation", "tosomaLocation", "position", "location"):
        if name not in table.column_names:
            continue
        values = table.column(name).to_pylist()
        hits = 0
        for i, value in enumerate(values):
            if found[i] or value is None:
                continue
            if isinstance(value, dict):
                value = [value.get("x"), value.get("y"), value.get("z")]
            elif isinstance(value, str):
                value = [v for v in value.strip("[]() ").replace(",", " ").split()]
            try:
                point = [float(v) for v in value][:3]
            except (TypeError, ValueError):
                continue
            if len(point) == 3 and all(np.isfinite(point)):
                xyz[i] = point
                found[i] = True
                hits += 1
        used.append(f"{name}: {hits}")
    return xyz, found, used


def hex_of(table, name):
    """Optic lobe column per row, NO_HEX where there is none."""
    data = table.column(name).to_pylist() if name in table.column_names else [None] * table.num_rows
    out = np.full(table.num_rows, NO_HEX, dtype=np.int16)
    for i, value in enumerate(data):
        if value is None:
            continue
        try:
            number = float(value)
        except (TypeError, ValueError):
            continue
        if np.isfinite(number) and -32767 <= number <= 32767:
            out[i] = int(round(number))
    return out


def build_image(sections, header_fields):
    def align(value):
        return (value + ALIGNMENT - 1) // ALIGNMENT * ALIGNMENT

    table_size = 40 + 24 * len(sections)
    offset = align(table_size)
    entries = []
    for tag, payload in sections:
        assert len(tag) == 8, tag
        entries.append((tag, offset, len(payload)))
        offset = align(offset + len(payload))
    header = struct.pack("<8sIIIIQII", b"FLYBRAIN", VERSION, len(sections), header_fields["neurons"], header_fields["strings"],
                         header_fields["edges"], header_fields["types"], 0)
    return header, entries, offset


def write_image(path, sections, header_fields):
    header, entries, total = build_image(sections, header_fields)
    temporary = Path(str(path) + ".partial")
    with open(temporary, "wb") as out:
        out.write(header)
        for tag, offset, size in entries:
            out.write(struct.pack("<8sQQ", tag.encode("ascii"), offset, size))
        for (tag, payload), (_, offset, size) in zip(sections, entries):
            out.write(b"\0" * (offset - out.tell()))
            out.write(payload)
        out.write(b"\0" * (total - out.tell()))
    temporary.replace(path)
    return total


def read_safetensors(path):
    """Header of a safetensors file: name -> (numpy dtype, shape, absolute offset). The arrays are opened with np.memmap."""
    dtypes = {"I64": np.int64, "I32": np.int32, "I8": np.int8, "F64": np.float64, "F32": np.float32, "U8": np.uint8}
    with open(path, "rb") as handle:
        size = struct.unpack("<Q", handle.read(8))[0]
        header = json.loads(handle.read(size))
    tensors = {}
    for name, info in header.items():
        if name == "__metadata__":
            continue
        begin, _ = info["data_offsets"]
        tensors[name] = (dtypes[info["dtype"]], tuple(info["shape"]), 8 + size + begin)
    return tensors


def open_tensor(path, tensors, name):
    dtype, shape, offset = tensors[name]
    return np.memmap(path, dtype=np.dtype(dtype).newbyteorder("<"), mode="r", offset=offset, shape=shape)


def janelia_edges(paths, ids):
    """(pre, post, contacts) of the kept neurons from the feather edge list, batch by batch, plus the source totals."""
    reader = ipc.open_file(pa.memory_map(str(paths["edges"]), "r"))
    log(f"edges: {reader.num_record_batches} batches, schema: {reader.schema}")
    n = len(ids)
    totals = {"rows": 0, "contacts": 0}
    parts = []
    for number in range(reader.num_record_batches):
        batch = reader.get_batch(number)
        pre = np.asarray(batch.column(batch.schema.get_field_index("body_pre")).to_numpy(zero_copy_only=False), dtype=np.uint64)
        post = np.asarray(batch.column(batch.schema.get_field_index("body_post")).to_numpy(zero_copy_only=False), dtype=np.uint64)
        weight = np.asarray(batch.column(batch.schema.get_field_index("weight")).to_numpy(zero_copy_only=False))
        totals["rows"] += len(pre)
        totals["contacts"] += int(weight.sum(dtype=np.uint64))
        i = np.searchsorted(ids, pre)
        j = np.searchsorted(ids, post)
        ok = (i < n) & (j < n)
        ok &= ids[np.minimum(i, n - 1)] == pre
        ok &= ids[np.minimum(j, n - 1)] == post
        parts.append((i[ok].astype(np.uint32), j[ok].astype(np.uint32), weight[ok].astype(np.int64)))
        if number % 50 == 0:
            log(f"  batch {number + 1}/{reader.num_record_batches}: {totals['rows']} rows read")
    return parts, totals


def hf_edges(paths, ids):
    """The same from the safetensors of the HuggingFace mirror: edge_src/edge_dst index neuron_id (88 million bodies)."""
    tensors = read_safetensors(paths["model"])
    neuron_id = open_tensor(paths["model"], tensors, "neuron_id")
    src = open_tensor(paths["model"], tensors, "edge_src")
    dst = open_tensor(paths["model"], tensors, "edge_dst")
    contacts = open_tensor(paths["model"], tensors, "synapse_count")
    log(f"safetensors: {len(neuron_id)} bodies, {len(src)} edges")

    # Body index of the mirror -> our neuron index, -1 for the bodies we do not keep
    to_ours = np.full(len(neuron_id), -1, dtype=np.int32)
    chunk = 1 << 24
    found = 0
    for begin in range(0, len(neuron_id), chunk):
        block = np.asarray(neuron_id[begin:begin + chunk]).astype(np.uint64)
        position = np.searchsorted(ids, block)
        hit = (position < len(ids)) & (ids[np.minimum(position, len(ids) - 1)] == block)
        to_ours[begin:begin + chunk][hit] = position[hit]
        found += int(hit.sum())
    if found != len(ids):
        raise SystemExit(f"{len(ids) - found} kept neurons have no body in the safetensors")

    totals = {"rows": 0, "contacts": 0}
    parts = []
    for begin in range(0, len(src), chunk):
        i = to_ours[np.asarray(src[begin:begin + chunk])]
        j = to_ours[np.asarray(dst[begin:begin + chunk])]
        weight = np.asarray(contacts[begin:begin + chunk]).astype(np.int64)
        totals["rows"] += len(weight)
        totals["contacts"] += int(weight.sum())
        ok = (i >= 0) & (j >= 0)
        parts.append((i[ok].astype(np.uint32), j[ok].astype(np.uint32), weight[ok]))
        log(f"  edges {min(begin + chunk, len(src))}/{len(src)}")
    return parts, totals


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", required=True, type=Path, help="folder with the source files (HuggingFace mirror or Janelia feather)")
    parser.add_argument("--out", required=True, type=Path, help="the .fbrain to write")
    parser.add_argument("--select", choices=["traced", "superclass"], default="traced",
                        help="traced: status == Traced (default); superclass: every body with a neuronal superclass, as DOOMFLY")
    args = parser.parse_args()
    started = time.time()

    kind = "hf" if (args.source / HF_FILES["model"]).exists() else "janelia"
    paths = {key: args.source / name for key, name in (HF_FILES if kind == "hf" else FILES).items()}
    for key, path in paths.items():
        if not path.exists():
            raise SystemExit(f"missing {path}")
    log(f"source: {'HuggingFace mirror QuixiAI/MaleCNS' if kind == 'hf' else 'Janelia flat connectome'} in {args.source}")

    # ---------------------------------------------------------------- neurons
    annotations = pq.read_table(paths["neurons"]) if kind == "hf" else feather.read_table(paths["annotations"])
    log(f"annotations: {annotations.num_rows} rows, columns: {', '.join(annotations.column_names)}")
    body = np.asarray(column(annotations, "bodyId", "body_id").to_numpy(zero_copy_only=False), dtype=np.uint64)
    superclass = strings_of(column(annotations, "superclass"))
    status = strings_of(column(annotations, "status", required=False)) if "status" in annotations.column_names else [""] * len(body)
    if args.select == "traced":
        keep = np.array([st == "Traced" for st in status], dtype=bool)
    else:
        keep = np.array([bool(s) and st != "Glia" for s, st in zip(superclass, status)], dtype=bool)
    order = np.argsort(body[keep], kind="stable")
    kept_rows = np.flatnonzero(keep)[order]
    ids = body[kept_rows]
    if np.any(ids[1:] <= ids[:-1]):
        raise SystemExit("duplicate body ids among the kept neurons")
    n = len(ids)
    log(f"kept {n} neurons ({args.select}) of {len(body)} bodies")

    def pick(values):
        return [values[i] for i in kept_rows]

    types = pick(strings_of(column(annotations, "type")))
    classes = pick(strings_of(column(annotations, "class"))) if "class" in annotations.column_names else [""] * n
    supers = pick(superclass)
    soma_side = strings_of(column(annotations, "somaSide", required=False)) if "somaSide" in annotations.column_names else [""] * len(body)
    root_side = strings_of(column(annotations, "rootSide", required=False)) if "rootSide" in annotations.column_names else [""] * len(body)
    sides = np.array([side_of(soma_side[i]) or side_of(root_side[i]) for i in kept_rows], dtype=np.uint8)

    xyz_all, found_all, position_columns = positions_of(annotations, annotations.num_rows)
    positions = xyz_all[kept_rows]
    log(f"positions from {position_columns}; {int(found_all[kept_rows].sum())} of {n} neurons have one")

    hex_rows = {name: hex_of(annotations, name)[kept_rows] for name in ("assignedOlHex1", "assignedOlHex2")}

    # ---------------------------------------------------------------- transmitters
    nt_table = pq.read_table(paths["neurotransmitters"]) if kind == "hf" else feather.read_table(paths["neurotransmitters"])
    log(f"neurotransmitters: {nt_table.num_rows} rows, columns: {', '.join(nt_table.column_names)}")
    nt_body = np.asarray(column(nt_table, "body", "bodyId").to_numpy(zero_copy_only=False), dtype=np.uint64)
    nt_name = strings_of(column(nt_table, "consensus_nt", "predicted_nt", "nt"))
    nt_order = np.argsort(nt_body, kind="stable")
    nt_body = nt_body[nt_order]
    position = np.searchsorted(nt_body, ids)
    found = (position < len(nt_body)) & (nt_body[np.minimum(position, len(nt_body) - 1)] == ids)
    transmitter_text = [nt_name[nt_order[p]] if f else "" for p, f in zip(position, found)]
    signs = np.empty(n, dtype=np.int32)
    transmitters = np.empty(n, dtype=np.uint8)
    uncertain = 0
    for i, text in enumerate(transmitter_text):
        sign, guess = transmitter_sign(text)
        signs[i] = sign
        transmitters[i] = transmitter_code(text)
        uncertain += guess
    log(f"transmitter found for {int(found.sum())} neurons; sign is a declared +1 guess for {uncertain}")

    # ---------------------------------------------------------------- edges
    parts, totals = hf_edges(paths, ids) if kind == "hf" else janelia_edges(paths, ids)
    source_rows = totals["rows"]
    source_contacts = totals["contacts"]
    pre = np.concatenate([part[0] for part in parts])
    post = np.concatenate([part[1] for part in parts])
    count = np.concatenate([part[2] for part in parts])
    del parts
    log(f"kept {len(pre)} of {source_rows} edge rows, {int(count.sum())} of {source_contacts} contacts")

    key = (pre.astype(np.uint64) << np.uint64(32)) | post.astype(np.uint64)
    edge_order = np.argsort(key, kind="stable")
    key = key[edge_order]
    pre = pre[edge_order]
    post = post[edge_order]
    count = count[edge_order]
    del edge_order
    first = np.ones(len(key), dtype=bool)
    first[1:] = key[1:] != key[:-1]
    if not first.all():
        starts = np.flatnonzero(first)
        log(f"merging {len(key) - len(starts)} duplicate pairs")
        count = np.add.reduceat(count, starts)
        pre = pre[starts]
        post = post[starts]
    del key, first
    # The numbers the HuggingFace mirror publishes for the traced subgraph of MaleCNS v1.0: anything else is an error
    if args.select == "traced":
        expected = TRACED_SUBGRAPH
        got = {"neurons": n, "edges": int(len(pre)), "contacts": int(count.sum())}
        if got != expected:
            raise SystemExit(f"the traced subgraph does not match MaleCNS v1.0: got {got}, expected {expected}")
        log(f"control numbers match: {got}")
    signed = count * signs[pre]
    clamped = int(np.count_nonzero(np.abs(signed) > MAX_WEIGHT))
    weights = np.clip(signed, -MAX_WEIGHT, MAX_WEIGHT).astype(np.int16)
    row_start = np.zeros(n + 1, dtype=np.uint64)
    row_start[1:] = np.cumsum(np.bincount(pre, minlength=n))
    if row_start[-1] > 0xFFFFFFFF:
        raise SystemExit("too many edges for uint32 offsets")
    row_start = row_start.astype(np.uint32)
    targets = post.astype(np.uint32)
    log(f"{len(targets)} edges, {clamped} weights clamped to +-{MAX_WEIGHT}, max contacts {int(count.max())}")

    # ---------------------------------------------------------------- strings and type groups
    strings = [""]
    string_ids = {"": 0}

    def intern(text):
        if text not in string_ids:
            string_ids[text] = len(strings)
            strings.append(text)
        return string_ids[text]

    type_ids = np.array([intern(t) for t in types], dtype=np.uint32)
    class_ids = np.array([intern(c) for c in classes], dtype=np.uint32)
    super_ids = np.array([intern(s) for s in supers], dtype=np.uint32)
    type_names = sorted({t for t in types if t})
    type_string_ids = np.array([string_ids[t] for t in type_names], dtype=np.uint32)
    index_of_type = {t: k for k, t in enumerate(type_names)}
    members = [[] for _ in type_names]
    for i, t in enumerate(types):
        if t:
            members[index_of_type[t]].append(i)
    type_start = np.zeros(len(type_names) + 1, dtype=np.uint32)
    type_start[1:] = np.cumsum([len(m) for m in members])
    type_neurons = np.array([i for m in members for i in m], dtype=np.uint32)

    encoded = [s.encode("utf-8") for s in strings]
    string_offsets = np.zeros(len(encoded) + 1, dtype=np.uint32)
    string_offsets[1:] = np.cumsum([len(e) for e in encoded])
    string_chars = b"".join(encoded)

    # ---------------------------------------------------------------- write
    def raw(array, dtype):
        return np.ascontiguousarray(array, dtype=dtype).astype(np.dtype(dtype).newbyteorder("<"), copy=False).tobytes()

    sections = [
        ("ATTRIB  ", ATTRIBUTION.format(source=SOURCES[kind], selection=SELECTIONS[args.select]).encode("utf-8")),
        ("STROFFS ", raw(string_offsets, np.uint32)),
        ("STRCHARS", string_chars),
        ("BODYID  ", raw(ids, np.uint64)),
        ("TYPE    ", raw(type_ids, np.uint32)),
        ("CLASS   ", raw(class_ids, np.uint32)),
        ("SUPERCLS", raw(super_ids, np.uint32)),
        ("SIDE    ", raw(sides, np.uint8)),
        ("NT      ", raw(transmitters, np.uint8)),
        ("HEX1    ", raw(hex_rows["assignedOlHex1"], np.int16)),
        ("HEX2    ", raw(hex_rows["assignedOlHex2"], np.int16)),
        ("POS     ", raw(positions.reshape(-1), np.float32)),
        ("ROWSTART", raw(row_start, np.uint32)),
        ("TARGETS ", raw(targets, np.uint32)),
        ("WEIGHTS ", raw(weights, np.int16)),
        ("TYPENAME", raw(type_string_ids, np.uint32)),
        ("TYPESTRT", raw(type_start, np.uint32)),
        ("TYPENEUR", raw(type_neurons, np.uint32)),
    ]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    size = write_image(args.out, sections, {"neurons": n, "strings": len(strings), "edges": len(targets), "types": len(type_names)})

    report = {
        "format_version": VERSION,
        "source": SOURCES[kind],
        "select": args.select,
        "neurons": n,
        "edges": int(len(targets)),
        "contacts": int(count.sum()),
        "source_annotation_rows": int(len(body)),
        "source_edge_rows": int(source_rows),
        "source_contacts": int(source_contacts),
        "types": len(type_names),
        "clamped_weights": clamped,
        "sign_guessed_neurons": int(uncertain),
        "neurons_without_position": int(n - found_all[kept_rows].sum()),
        "bytes": int(size),
        "source_sha256": {name: sha256(path) for name, path in paths.items()},
        "seconds": round(time.time() - started, 1),
    }
    report_path = args.out.with_suffix(".report.json")
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    log(json.dumps(report, indent=2))
    log(f"wrote {args.out} ({size / 1048576:.1f} MB) in {report['seconds']} s")


if __name__ == "__main__":
    sys.exit(main())
