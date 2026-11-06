# Three-client HP observations

`observe_player_vitals.py` opens only read/query process handles. It never writes process memory,
executes native functions, injects code, sends game inputs, changes a process or reads game code.
Use current identified client PIDs, their actual engine ports and their current runtime logs.

Example from a worktree, with current PIDs substituted:

```powershell
python Dev/testkit/observe_player_vitals.py `
  --client A <A-PID> 64101 ../live-gate/client-<A-PID>.log `
  --client B <B-PID> 64102 ../live-gate/client-<B-PID>.log `
  --client C <C-PID> 64103 ../live-gate/client-<C-PID>.log `
  --runtime-dll ../live-gate/kcd2s-game-client-v4.dll `
  --output ../health-audit/combat.csv --seconds 45
```

Start observation before the separately operated combat, parry, KO, heal or revive scenario.
The observer validates each game executable/port and loaded runtime path, derives network owners
and local/replica entity IDs from current logs, and checks each actor/entity identity before reads.
It does not assume a fixed local engine ID. See the module docstring for layout provenance.

The output has four files:

- `.csv`: time-stamped native HP/stamina/exhaust/hunger for each owner and its two replicas,
  combat state, immortality/permanent-unconsciousness buff presence and logged KO transitions.
- `.agreement.csv`: each replica's difference from its owner, how long the owner's rounded HP
  has stayed unchanged, and agreement/transient-lag/settled-mismatch classifications.
- `.summary.json`: settled HP acceptance, transient mismatch counts, largest difference, missing
  views, read errors and sampling throughput. All three owners need settled comparison evidence.
- `.identity.json`: the identified process image and command line for each requested PID.

The default settled window is 300 ms and HP tolerance is 0.01 native units. The raw timeline keeps
earlier disagreement visible. A zero-HP replica with a positive-HP owner fails even inside that
settling window. Missing views and memory-read errors also prevent HP acceptance.

Every owner's final HP must also finish the settling window. Initial agreement does not let a
capture pass while dense damage at its end is still changing or its final replicas disagree.
The summary separately compares observed owner KO intent with the replicas' actual permanent
unconsciousness buffs after settling. It exposes stale KO buffs after a logged successful revive;
positive HP alone never means the owner woke. Unknown owner intent or failed buff reads stay
unknown, and the native lifecycle verdict remains unproven.

The raw `tick_ms`/`read_completed_ms` bound each view's read; comparison rows retain separate
owner and replica start ticks. Reads across three clients are sequential, not atomic. The kit's
`burst_t.py` and `burst_fast.py` screenshot `times.txt` use the same GetTickCount64 clock, so join
those timestamps to the nearest rows for that client and report the distance/read interval.
PrintWindow timestamps mark capture start, so screenshots alone do not prove exact contact timing.

Native consciousness/dead/unconscious fields are explicitly unknown: consciousness is computed by
the game's derived-stat getter, not the float after the writable soul states. Buff presence and
logged KO intent are supporting observations, never substitutes for those native predicates or
for three-view screenshots. `native_lifecycle_proven` therefore remains false. Complete native
lifecycle acceptance needs corresponding native evidence and a functional check in the game.

Portable parser/acceptance checks:

```powershell
python -m unittest discover -s Dev/testkit -p test_observe_player_vitals.py -v
```
