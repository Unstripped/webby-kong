# Contributing

## Branches

- `development` — daily work and the target of pull requests.
- `main` — releases only. It is updated by merging `development`; every push to it runs CI and publishes a
  GitHub Release.

## Commit messages

`area: lower-case summary`, e.g. `patches: race reward flag endianness`. Areas in use include `patches`, `gfx`,
`web`, `widescreen`, `port`, `gen_data`, `debug`, `arcade`, `tests`, `saves`, `prepare`, `docs`, `ci`, `repo`.

Put `#minor` or `#major` anywhere in a commit message (subject or body) to make the next release bump that part of
the version instead of the patch version (rule: `tools/next_version.py`).

## Changing game source

Never edit `decomp/`. Game source changes are commits in `build/tree`, exported as patches:

```sh
tools/prepare.py tree      # fresh build/tree with the current patches applied
# edit and commit in build/tree, one commit per concern
tools/prepare.py export    # rewrite patches/ from build/tree commits base..HEAD
```

Commit the resulting `patches/*.patch` changes here.

`mise run sync` / `mise run serve` compile your `build/tree` commits and edits as they are; they recreate
`build/tree` only when `decomp/`, `patches/` or `tools/prepare.py` change, and stop instead if it holds work
`patches/` does not.

## No ROMs, ever

Never commit a ROM, game asset, save file or build output: they are Nintendo's data or derived from it. CI rejects
them (`tools/check_no_roms.py`); install the same check as a pre-commit hook in your clone:

```sh
git config core.hooksPath .githooks
```

## Before opening a pull request

Run the CI commands and the headless regression scenarios in [docs/testing.md](docs/testing.md). CI has no ROM,
so the scenarios are yours to run.

## Releasing

1. Confirm the latest CI run on `development` is green.
2. Merge into `main` and push:

   ```sh
   git checkout main && git merge --ff-only development && git push
   ```

   If the fast-forward fails, use `git merge development` instead.
3. The Release workflow runs CI, picks the next version, publishes the GitHub Release with its tag, then pushes the
   Docker image `ghcr.io/unstripped/webby-kong:<tag>` (`.github/workflows/docker.yml`).

Never create `v*` tags by hand: the version rule reads them.
