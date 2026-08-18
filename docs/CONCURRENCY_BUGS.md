# Concurrency Bug Postmortem: B+ Tree insert/remove races

A debugging case study from Phase 4 validation. While stress-testing WAL recovery on CI,
a genuine concurrency bug turned up in the Phase 3 B+ tree — concurrent `insert()` and
`remove()` against an already-multi-level tree could corrupt it. What looked like one bug
turned out to be **five distinct races in the same family**, found one at a time by
repeatedly reproducing the failure on CI and tracing a single node's lifecycle back through
instrumented logs.

This doc exists for two reasons: (1) a written record of what broke and why, and (2) this
is genuinely good interview material — concurrent data structure bugs, systematic debugging
under a stress workload, and the tradeoffs in fixing them are exactly what a "tell me about
a hard bug" question is fishing for.

---

## The setup

- B+ tree with fine-grained latch crabbing + a B-link design (Lehman & Yao): every node has
  its own reader/writer spinlock, and a split links its new right sibling in immediately
  (via `next_leaf`/`right_link` + `high_key`) while the *parent* link gets fixed up
  separately, via `propagateSplit()`.
- A single `structure_latch` (shared for traversals, exclusive for `propagateSplit()` and
  `remove()`) serializes all structural changes against each other and against readers —
  see `include/db_engine/bplustree.hpp` for the full rationale.
- The repro: `tests/test.cpp`'s `test_concurrent_insert_remove()` — 3 threads removing a disjoint
  key range concurrently with 3 threads inserting a disjoint (higher) key range, against a
  tree that already has ~300 keys and several levels. Run in a loop (300 iterations) on
  GitHub Actions CI, not locally (see "How this was debugged" below).

The five bugs all stem from one theme: **a concurrent `remove()` and a `propagateSplit()`
call that's still queued behind the exclusive lock can disagree about the tree's current
shape**, because `propagateSplit()`'s parent fix-up is deliberately lazy (that's the whole
point of the B-link design) and a `remove()` can slip in and change things in the meantime.

---

## Bug 1: TOCTOU race on `root` in `remove()`

**Symptom:** the entire tree would occasionally vanish mid-test — a key that was definitely
inserted became unfindable, with no exception, no assertion failure, just a smaller tree.

**Root cause:** `remove()` used to snapshot `root` *before* acquiring `structure_latch`
exclusively:

```cpp
auto root_snapshot = snapshotRoot();          // <-- before the lock
ExclusiveLatchGuard structure_lock(structure_latch);
```

If a concurrent `insert()`'s `propagateSplit()` promoted a *new* root in the gap between
those two lines, `root_snapshot` was left pointing at what was now just the tree's leftmost
leaf, not the actual root. `remove()`'s "am I deleting the tree's last leaf?" special case
(`path.size() == 1 && leaf->keys.empty() → swapRoot(nullptr)`) used that stale snapshot as
its signal, and nulled out the *real*, freshly-promoted root — discarding the whole tree
built under it.

**Fix:** take the snapshot *after* the exclusive lock is held. Every writer of `root` needs
at least `structure_latch` shared (insert's bootstrap) or exclusive (everything else), so
once `remove()` holds it exclusively, `root` is provably stable for the rest of the call —
closing the window entirely, not just narrowing it.

**File:** `src/bplustree.cpp`, `remove()`.

---

## Bug 2: `remove()` nullifying `root` while it still had live content

**Symptom:** same as Bug 1 (a key vanishing with no error) — this was actually the bug that
kept the tree failing even after Bug 1 was fixed, because it's a second, independent way to
hit the same "root gets nulled with content still hanging off it" outcome.

**Root cause:** the B-link design lets a leaf split *before* its parent link is fixed up —
that's normal, temporary, and safe. But it means a bare-leaf `root` (i.e. the tree hasn't
been promoted to a real multi-level structure yet) can have a `next_leaf` pointing at real,
live content that just hasn't been linked into a proper parent yet. `remove()`'s root-empty
special case didn't check for this: if `root` (a bare leaf) had its last key removed, the
code nulled `root` unconditionally — discarding the entire chain still reachable via
`next_leaf`. The very next `insert()`, seeing `root == nullptr`, silently bootstrapped a
brand-new, disconnected tree from scratch.

**Fix:** only nullify `root` when `leaf->next_leaf` is *also* null — i.e. the tree is
genuinely, completely empty. Otherwise just leave `root` as the (temporarily empty) leaf;
its `high_key`/`next_leaf` still route searches correctly via `moveRight`, and it gets
cleaned up normally by whatever future split or merge touches it next.

**File:** `src/bplustree.cpp`, `remove()`, the root special case.

---

## Bug 3: `propagateSplit()` resurrecting a node a concurrent merge already killed

**Symptom:** a freshly-inserted key would be present when scanned via the leaf chain
directly (`next_leaf` walk) but unreachable via `search()` — or, worse, entirely missing —
depending on timing.

**Root cause:** when `remove()`'s rebalancing merges an underflowing leaf into a sibling
(`handleLeafUnderflow`/`handleInternalUnderflow`), the absorbed node's own `keys`/`rids`/
`children` are **never cleared** — only copied into the survivor. The object stays alive
(and looks completely normal) as long as anything still references it, which is exactly
what happens when that same node had *just* split moments earlier and a `propagateSplit()`
call is still queued behind the exclusive lock, holding a `shared_ptr` to it as `left_child`.

`propagateSplit()`'s self-healing logic — "if `left_child` isn't in the resolved parent's
children, insert it" — had no way to distinguish "not yet linked" (needs inserting) from
"already correctly unlinked by a merge" (must *not* be reinserted). It would happily
resurrect the dead node, with its stale, frozen content, back into the live tree —
corrupting the routing exactly where the split's genuinely-new sibling should have ended up.

**Fix:** a new `is_merged_away` flag on `BPlusTreeNode`, set the instant a node is absorbed
(all 4 merge call sites: leaf/internal × merge-left/merge-right). `propagateSplit()` checks
it before ever reinserting a "not found" `left_child`. `getParent()` also rejects a resolved
parent that's since been merged away (its own weak_ptr chain can point at one), and the
`ancestor_hints` fallback skips dead entries the same way — both needed the same guard,
since either could hand back a dead-but-still-resolvable node.

**Files:** `include/db_engine/node.hpp` (the flag), `src/bplustree.cpp` (`handleLeafUnderflow`,
`handleInternalUnderflow`, `getParent`, `propagateSplit`).

---

## Bug 4: `propagateSplit()` resurrecting a *live* node into the *wrong* parent

**Symptom:** same signature as Bug 3 (key present via raw scan, missing/misrouted via
`search()`) — this is what remained *after* Bug 3's fix, on the next several CI runs. Traced
by following one specific node's full lifecycle through the log by its (deliberately added,
debug-only) instance ID, since heap addresses get reused constantly under this workload.

**Root cause:** "not found in this parent" doesn't only mean "dead" (Bug 3) — it can also
mean the node is alive and *already correctly linked under a different parent*, because
`parent_hint` for this particular `propagateSplit()` call resolved to the wrong (but
perfectly valid, live) ancestor. Bug 3's fix correctly stopped reinserting `left_child` in
that case — but then let execution fall through and link `right_child` (its sibling, always
created by splitting `left_child`, so it *always* belongs under the same parent) into that
wrong parent anyway.

**Fix:** when `left_child` turns out to be linked under a different, live parent, redirect
the rest of the iteration — release the wrong parent's latch, acquire the correct one
(read directly off `left_child->parent`, not via `getParent()` — see the deadlock note
below), re-run the same `moveRight` corrections against it, and let `right_child`'s
insertion and the fullness/cascade check run against the *corrected* parent instead.

**Deadlock note:** the first version of this fix called `getParent(left_child)` to find the
"real" parent — but by that point the function is already holding the *originally resolved*
parent's latch exclusively, and `left_child`'s cached parent pointer can legitimately equal
that exact node (the ordinary case). `getParent()` would then try to re-acquire a latch this
same thread already held — a guaranteed self-deadlock (`RWSpinLatch` has no reentrant
support). This actually happened once on CI: a run hung for 13+ minutes instead of failing
in the usual ~2.5, which is what gave the deadlock away before it shipped. Fixed by reading
`left_child->parent` directly and only latching the *resolved* parent separately (never
alongside the one already held).

**File:** `src/bplustree.cpp`, `propagateSplit()`.

---

## Bug 5: `findAncestorForRelink()`'s level-BFS assumed uniform tree depth

**Symptom:** `terminate called ... what(): propagateSplit: guided descent from root could
not locate left_child's level -- tree structure corrupted`.

**Root cause:** this function is the fallback used when neither `ancestor_hints` nor
`getParent()` can find a parent at all (e.g. several generations of not-yet-linked splits
deep). The original implementation did a level-order BFS from `root`, fanning out across
*every* sibling at each level to find the one matching `left_child`'s depth. That assumes
every branch of the tree sits at the same depth at the same moment — which is **not** true
under real concurrent load: with several inserter threads racing ahead of their own
`propagateSplit()` fix-ups (each queued behind the same exclusive lock), one branch's
unlinked chain can run several splits deeper than an unrelated, already-linked branch
elsewhere. Fanning out across siblings could then mix leaves and internal nodes in the same
"level" — something a well-formed B+ tree never has, and indistinguishable from genuine
corruption to the old code, hence the throw.

**Fix:** replaced the BFS with a rightmost-spine descent — always follow `children.back()`,
never look sideways at siblings. This works because of a structural invariant: every split,
at every depth, extends the tree's *current rightmost branch* (new inserts always
`moveRight` to the frontier). So the rightmost spine from `root` is guaranteed to reach
`left_child`'s level regardless of what depth any unrelated branch happens to be at — it
never visits "elsewhere," so non-uniform depth elsewhere simply can't confuse it.

**File:** `src/bplustree.cpp`, `findAncestorForRelink()`.

---

## How this was debugged

Worth writing down since the technique mattered as much as the bugs:

1. **Reproduced on CI, not locally.** This machine is resource-constrained; a 300-iteration,
   6-thread stress loop is exactly the kind of thing that should run on GitHub Actions
   compute instead of tanking a laptop. The workflow triggers on plain branch push, so no PR
   was needed for the fast iterate-push-watch loop.
2. **fprintf instrumentation, not a debugger,** because the bug only reproduced under real
   thread contention — a debugger breakpoint changes the timing enough to hide it. Tagged
   every structural mutation site (`[SPLIT]`, `[MERGE-LEFT]`, `[MERGE-RIGHT]`, `[BORROW-*]`,
   `[PSPLIT-*]`) with `fflush(stderr)` after every line.
3. **Per-node instance IDs, separate from the persisted `node_id`.** Heap addresses get
   reused constantly (a freed node's memory is immediately claimed by the next allocation of
   the same size), which makes naive address-based log correlation actively misleading — two
   log lines with the same address can be two completely unrelated objects. A monotonic
   counter assigned at construction disambiguates them and was what let several of these
   bugs get pinned down precisely instead of guessed at.
4. **Trace one node's full lifecycle, not the whole log.** Once a specific missing/misrouted
   key was identified, `grep`-ing every mention of that exact node (address *and* instance
   ID) from creation to the point of failure reliably surfaced the exact operation
   responsible — every one of these five bugs was found this way, not by theorizing first.
5. **One fix at a time, re-verify on CI, don't stack guesses.** Several early attempts (a
   first version of Bug 5's fix, an earlier narrower version of Bug 3's fix) were
   individually real improvements but incomplete — each was caught by pushing again and
   letting the same stress loop find the next failure, rather than assuming a fix that
   *looked* right was actually complete.

---

## Known follow-up: not yet fixed

After all five fixes above, the stress test went from failing on essentially the first
iteration (100% reproduction) to passing **~900 consecutive iterations** across 3 clean
300-iteration CI runs. One more, much rarer failure in the *same* exception family
(`findAncestorForRelink`'s "could not locate left_child's level") turned up once, at
iteration 298 of a 300-iteration run — roughly 1 in 900.

What's known: in that trace, `left_child`'s recorded parent (call it P) appeared to be
alive and reachable moments before this call needed it — nothing in the surrounding log
shows P being merged away, and `structure_latch` exclusivity should rule out anything else
mutating it mid-call. But `getParent(left_child)` apparently still returned null, forcing
the fallback path, which then *also* failed to find P via the rightmost spine. That's
inconsistent with every mechanism fixed above and wasn't root-caused before this work was
shipped — deliberately punted rather than chased further, since going from "fails always"
to "fails 1 in 900" is a large, real improvement and further narrowing this needs another
round of the same trace-and-reproduce cycle on CI.

**Next steps if picked back up:** add tracing to whatever code path could cause `P` to
become unreachable from `root` without going through a merge (candidates worth checking
first: a second `propagateSplit()` cascade interacting with `P` in a way not yet covered by
Bug 4's redirect fix, or something in the internal-split cascade — `splitInternalNode()` —
that doesn't have the same protections as the leaf-split path).
