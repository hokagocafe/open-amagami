# The `ARC` format

The game's own container. Used by `SCENARIO.ARC`, `GRAPH/GRAPH0.ARC`, `GRAPH/GRAPH1.ARC`, `GRAPH/GRAPH2.ARC` and `SOUND/MUSIC.ARC`.

Confidence levels: **confirmed**, **hypothesis**, **pending**. See [disc.md](disc.md).

## Layout (confirmed)

All integers are little-endian. `n` is the number of files.

| Offset | Type | Field |
|---|---|---|
| `0` | `u32` | `n`, number of files |
| `4` | `u32` | Offset where data starts; equals `8 + 20·n` |
| `8` | `u16[n]` | For each bucket, how many entries it holds |
| `8 + 2·n` | `u16[n]` | For each bucket, the index of its first entry |
| `8 + 4·n` | entry`[n]` | Entry table |
| after | bytes | File data, uncompressed |

Each entry is 16 bytes:

| Offset | Type | Field |
|---|---|---|
| `0` | `u32` | Always `1` in the files checked; meaning pending |
| `4` | `u32` | Name hash |
| `8` | `u32` | Data offset from the start of the `ARC` |
| `12` | `u32` | Size in bytes |

The index is a hash table with `n` buckets. A file's bucket is `hash % n`.

### Looking up a file

```
h      = hash(name)
bucket = h % n
for i from first[bucket] to first[bucket] + count[bucket] - 1:
    if entry[i].hash == h: return entry[i]
```

### Verification

On `SCENARIO.ARC`, `GRAPH0.ARC` and `MUSIC.ARC`:

- the per-bucket counts add up to `n`;
- every entry sits in bucket `hash % n`;
- the last entry ends exactly at the end of the file.

When a bucket is empty, its first-entry index is 0.

### Ordering rules (confirmed)

These are what a rebuilt archive must follow to be identical:

- File data is stored back to back with no padding, in an order that is neither by hash nor by bucket (presumably the order the files were added).
- The entry table is grouped by bucket, in bucket order. Inside a bucket, entries keep the order of their data.
- A non-empty bucket's first-entry index is the running total of the counts before it.

`tools/hokago.py arc verify` rebuilds an archive from its parsed contents using only these rules and compares. All five archives on the disc come out byte-identical.

## Hash function (confirmed by data; not yet located in the executable)

With it, 6,256 of the 6,273 entries on the disc resolve to sensible names.

Names are not stored, only a 32-bit hash:

```
h = 0
for each byte c of the name:
    h = (h * 16301 + c) mod 2^32
```

The name is the path exactly as the engine builds it: forward slashes, original case, with extension.

It was found by brute-forcing multiplicative hash families against names that appear literally in the executable. With multiplier 16301 and initial value 0, six of them land on existing entries:

| Name | Archive |
|---|---|
| `bg/000_00.tm2` | `GRAPH1.ARC` |
| `stand/b0_Aax.tm2` | `GRAPH2.ARC` |
| `stand/b0_Aaz.zb` | `GRAPH2.ARC` |
| `stand/f0_Ax.tm2` | `GRAPH2.ARC` |
| `sys_se0.hd` | `MUSIC.ARC` |
| `sys_se0.bd` | `MUSIC.ARC` |

No other multiplier below 70,000 matched more than one name. The routine itself has not been located in the executable yet.

## Recovering names (confirmed for 99.7% of entries)

Since the hash cannot be inverted, names are rebuilt by generating candidates and keeping those whose hash exists. `tools/hokago.py arc_names` does this from three sources:

- **`SCF` class names.** Every entry of `SCENARIO.ARC` is named `<ClassName>.scf`.
- **Paths in the executable**, with `printf`-style patterns expanded: `bg/%03d_%1d%1d`, `epi/e%1x_%02d%c0`, `stand/%s%c`, `%s%1d.hd` and the like.
- **String tokens in the scripts and the system image**, used both directly and to fill `%s`.

A candidate is only accepted if its extension agrees with the file's contents (`.tm2` for TIM2 data, and so on), which removes chance collisions.

| Archive | Named | Naming |
|---|---|---|
| `SCENARIO.ARC` | 2,485 of 2,485 | `<ClassName>.scf` |
| `GRAPH0.ARC` | 749 of 750 | Interface art by folder: `item/`, `mahjong/`, `idou/` (action map), `bgsam/` (background thumbnails), `sysgraph/`, `kaiwa/` (conversation), `font/` |
| `GRAPH1.ARC` | 1,164 of 1,164 | `bg/NNN_NN.tm2` backgrounds and `epi/eH_NNcN.tm2` event pictures |
| `GRAPH2.ARC` | 1,854 of 1,870 | `stand/<name>x.tm2` character sprites, each with a `stand/<name>z.zb` companion |
| `MUSIC.ARC` | 4 of 4 | `sys_se0.hd`, `sys_se0.bd`, `sys_se1.hd`, `sys_se1.bd` |

Seventeen entries are still unnamed: one small TIM2 in `GRAPH0.ARC` and eight sprite/companion pairs in `GRAPH2.ARC`.

```sh
python tools/hokago.py arc_names extracted build/names
python tools/hokago.py arc extract extracted/GRAPH/GRAPH1.ARC out/graph1 --names build/names/GRAPH1.ARC.txt
```

## Contents

| File | Entries | Contents | Confidence |
|---|---|---|---|
| `SCENARIO.ARC` | 2,485 | All start with the magic `SCF\x1A`; see [scf-format.md](scf-format.md). Sizes from 102 to 63,710 bytes | Confirmed |
| `GRAPH0.ARC` | 750 | 749 TIM2 images and one 24,678-byte entry of another type | Confirmed |
| `GRAPH1.ARC` | 1,164 | All TIM2, every one exactly 287,808 bytes: full-screen pictures such as `bg/000_00.tm2` | Confirmed |
| `GRAPH2.ARC` | 1,870 | 935 TIM2 images and 935 small companion files that start with floating-point values; `stand/…tm2` character sprites and their `.zb` files | Hypothesis for the pairing; counts confirmed |
| `MUSIC.ARC` | 4 | Two Sony sound bank pairs: header (magic `IECS`) and samples | Confirmed |

## Tool

```sh
python tools/hokago.py arc list    extracted/SCENARIO.ARC
python tools/hokago.py arc extract extracted/SCENARIO.ARC out/scenario [--names names.txt]
python tools/hokago.py arc pack    out/scenario rebuilt.ARC
python tools/hokago.py arc verify  extracted/SCENARIO.ARC
```

Extraction writes an `index.tsv` that records the data order, which `pack` needs. Files are named after their hash unless `--names` supplies candidate paths.
