# Third-party license corpus

Each directory under `licenses/` is one third-party component bundled by the
seekdb Python wheel:

```text
licenses/<component>/
  METADATA.json
  <upstream license text>
```

`METADATA.json` records the component name, exact version, upstream source,
packaging form, license, modification state, license files, redistribution
requirements, and evidence for the recorded version.

`scripts/stage-license-bundle.py` walks this directory, validates the metadata,
and renders the bundle that is installed into the wheel:

- `LICENSE` and `NOTICE` for the seekdb-bindings project;
- `THIRD_PARTY_COMPONENTS.md` and `.json` inventory;
- `REDISTRIBUTION.md`;
- `MANIFEST.sha256`;
- the component directories with the original license texts;
- `seekdb/`, the engine bundle generated from the seekdb source tree when it is
  available at build time.

The license texts must remain verbatim. Update the version and evidence fields
whenever the corresponding dependency or build image changes.
