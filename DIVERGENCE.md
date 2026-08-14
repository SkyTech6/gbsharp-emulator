# Divergence from upstream binjgb

Everything this fork changes, one section per patch. A patch that cannot be
justified in writing here is a patch to drop.

Upstream is `binji/binjgb`; the fork point is recorded in [NOTICE](NOTICE).
The authoritative list of changes is:

```
git log --oneline upstream-main..main
```

Each patch below says what it touches, why GB# needs it, and whether it is a
candidate to send upstream. "Additive" means the patch only adds files or
symbols and changes no existing behaviour, which is what keeps
`git rebase --onto upstream-main` cheap.

| Patch | Files | Kind | Upstream candidate |
|---|---|---|---|
| [Fork lineage](#fork-lineage) | `NOTICE`, `DIVERGENCE.md` | Additive | No |

## Fork lineage

**Files:** `NOTICE`, `DIVERGENCE.md`

Records what this fork is, what it was forked from, and how its branches are
meant to be used. Nothing else in the tree is touched, and neither file exists
upstream, so this patch can never conflict.

**Upstream candidate:** no. It is about the fork, not about binjgb.
