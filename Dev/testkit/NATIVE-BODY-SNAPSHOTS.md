# Native body observations in the three-client kit

The test-only `Local\KCD2S.BodySnapshot.<pid>` event requests one bounded snapshot
in the engine update thread, after the frame's combat delivery. It exists only
when the kit explicitly enables `KCDS_TEST_HOOKS`. It applies no damage, buffs,
revive, movement or animation changes. Production and unsignalled frames perform
no diagnostic native reads.

Each snapshot has begin/row/end records in the client's normal private log, with
the process, world revision, owner, sample number and Windows GetTickCount64 time.
Rows contain positive native HP and the actual uniquely resolved C_Soul v7
IsUnconscious result through knockout::observe_body. This is different from a
cached KO intent, replica buff presence, or a guessed raw consciousness offset.
The existing validated body reader also records physics profile and native action.
Missing identity/interface or nonpositive/nonfinite HP produces an unavailable
row; it is not converted into an awake/dead claim. At most 16 remote bodies are
sampled; a larger set is explicitly marked truncated.

For an authorized live test, first reidentify the three kit processes and loaded
build, record a GetTickCount64 watermark, then signal each event. Retain each log
and require the corresponding complete snapshot after the watermark. The three
processes must be on the same Windows host for their tick values to be comparable.

```text
python Dev/testkit/check_body_snapshots.py A.log B.log C.log --after-tick <watermark>
```

The checker requires three distinct processes/owners, a common current world,
all nine available body readings, correct owner markers, and a complete latest
snapshot in every log. It rejects a later incomplete snapshot instead of falling
back to older successful evidence. Default observation skew is at most 100 ms and
HP tolerance 0.02. Exit 0 means HP and native KO agree for that sample, exit 1 means
they disagree, and exit 2 means the evidence is incomplete or invalid.

Physics/action differences are reported separately. An agreed native predicate
does not prove a body visibly fell or stood, nor does a single sample prove a
transition stayed synchronized. Capture timestamped unobscured views and repeat
samples around asserted damage, KO, heal and revive. Preserve transient readings;
do not silently relabel them as settled agreement. No live acceptance is implied
by compiling the hook or passing the checker fixture tests.
