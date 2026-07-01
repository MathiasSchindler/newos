# EUR-Lex experiment

`eurlex-delegated` answers a focused legal-metadata question: given one or more
EU regulation identifiers, list the delegated acts in a local EUR-Lex metadata
ZIP that are based on, or amend, those regulations. It is an experiment, but it
is built in the same freestanding style as the main tools: the ZIP reader uses
`src/shared/archive_zip.c`, deflate uses the shared compression code, and the
threaded scan uses `RtTaskPool`.

The metadata ZIP contains one `tree_non_inferred.rdf` file per downloaded act. A delegated act can be found by scanning RDF descriptions for:

- `work_has_resource-type` ending in `_DEL`, for example `REG_DEL`.
- `resource_legal_based_on_resource_legal` pointing at the requested CELEX act.
- In practice, delegated acts that amend the requested regulation can also carry `resource_legal_amends_resource_legal`, so the first scanner treats either relation as relevant.

Build and query with:

```sh
make -C experimental/eurlex
experimental/eurlex/build/eurlex-delegated 32019R1021
```

The default target is freestanding. On macOS/aarch64 this produces the
project-linked Mach-O tool at `experimental/eurlex/build/eurlex-delegated`; on
Linux it produces the freestanding Linux-ABI tool for the selected target
architecture. A hosted build is available for bring-up with:

```sh
make -C experimental/eurlex host
```

Targets may be passed as CELEX IDs or as common regulation citations. These are
equivalent:

```sh
experimental/eurlex/build/eurlex-delegated 32022R2554
experimental/eurlex/build/eurlex-delegated '(EU) 2022/2554'
```

Multiple targets can be queried in one scan:

```sh
experimental/eurlex/build/eurlex-delegated 32022R2554 32019R1021
```

Use `-a` to point at a different EUR-Lex metadata ZIP:

```sh
experimental/eurlex/build/eurlex-delegated -a path/to/LEG_MTD.zip 32022R2554
```

Output is TSV written to standard output:

```text
target_celex    delegated_celex resource_type   title   metadata_entry
32022R2554      32024R1774      REG_DEL         ...     uuid/tree_non_inferred.rdf
```

The fields are:

- `target_celex` - the normalized regulation identifier that matched.
- `delegated_celex` - the delegated act's CELEX identifier.
- `resource_type` - the EUR-Lex resource type leaf, such as `REG_DEL`.
- `title` - the English title where available, otherwise an untranslated or entry-level title.
- `metadata_entry` - the RDF entry inside the ZIP that produced the row.

The scanner reads `experimental/eurlex/data/LEG_MTD_20260628_01_00.zip` by default and inflates ZIP entries through `src/shared/archive_zip.c` and the shared deflate code. Targeted scans collect the ZIP central directory serially, then inflate and scan RDF entries through `RtTaskPool` with one archive descriptor per worker. Set `NEWOS_EURLEX_WORKERS=1` to force the serial backend; when unset, the worker count is capped at 8.

The ZIP uses UUID entry names, so a one-off query still has to inflate each RDF entry before it can know whether the requested CELEX appears. For repeated lookups, build a TSV relationship index once and query that file instead. Supplying targets builds a small target-specific index; omitting targets builds a complete index but has to extract every delegated relationship in the corpus.

```sh
experimental/eurlex/build/eurlex-delegated --build-index experimental/eurlex/build/dora-index.tsv 32022R2554
experimental/eurlex/build/eurlex-delegated -i experimental/eurlex/build/dora-index.tsv 32022R2554
```

To build a reusable complete index for all delegated relationships in the ZIP,
omit target arguments:

```sh
experimental/eurlex/build/eurlex-delegated --build-index experimental/eurlex/build/delegated-index.tsv
experimental/eurlex/build/eurlex-delegated -i experimental/eurlex/build/delegated-index.tsv 32022R2554
```

The direct ZIP scan is useful for one-off checks and for validating archive
parsing. The index workflow is the preferred mode for interactive or repeated
queries because lookup becomes a linear scan over a compact TSV file instead of
inflating every RDF entry again.

Performance controls and diagnostics:

- `NEWOS_EURLEX_WORKERS=1` forces serial scanning.
- `NEWOS_EURLEX_WORKERS=N` selects a worker count, capped by `RT_TASK_POOL_MAX_WORKERS`.
- The default worker count uses the platform width capped at 8.
- `make -C experimental/eurlex PROFILE=1 freestanding` builds a function-instrumented tool for the project profiler.

On local macOS/aarch64, the DORA direct lookup (`32022R2554`) measured about 50.9 seconds with `NEWOS_EURLEX_WORKERS=1` and about 7.0 seconds with the default threaded scan, with byte-identical TSV output. After the shared deflate bit-reader optimization, the same threaded direct lookup measured about 6.93 seconds in repeated runs.

Known limits:

- The scanner depends on the RDF relation patterns currently observed in the EUR-Lex metadata dump.
- Direct scans must inflate each RDF file because the ZIP entry names are UUIDs rather than CELEX IDs.
- Title extraction is intentionally simple and favors English titles when they are present.
- This is not a general EUR-Lex search client; it works from a local metadata ZIP or from an index produced from that ZIP.