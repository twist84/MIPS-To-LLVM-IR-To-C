# SDBZ USA spawn-boundary investigation

> **Record status (2026-11-25):** Historical spawn-boundary investigation with its original evidence limits. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Any branch, PR, pending replay or feature-status claims below refer to the documented stage.


Original ELF SHA-256 verified:
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.
No retail executable or data table is committed.

- `0x0023af30` reads stage fields +8/+0x24, resolves a table entry at
  0x00488400, and calls a virtual method at vtable +0x28.
- `0x0023b0ac..0x0023b160` computes a difference and midpoint, then
  invokes the xyz normalization routine at 0x00106950.
- `0x0010697c..0x00106990` squares xyz, adds y and z into x,
  computes Q = VF0.w / sqrt(sum), waits for Q, then scales xyz.
  W is preserved. The exact six-word sequence is covered in the
  VU0 FMAC regression test with both directions of a 3-4-5 vector.
- `0x0023b194..0x0023b230` scales the direction by 15 and forms
  opposing position candidates. `0x001cbb40` returns a field at
  player->+0x534->+8. The original branch selects one candidate
  and passes it to virtual method +0x40 at 0x0023b264 or 0x0023b280.
- One paired caller invokes the routine at 0x0029820c and 0x00298218
  for different objects, then calls 0x0030e300 on both.
  That function calls virtual position getter +0x4c, sets Y=250,
  calls setter +0x40, then performs further position-related work.
  These observations concern position, not proof of a velocity defect.
- `0x00109c64..0x00109cf0` subtracts 128 from reply bytes 6,7,4,5;
  values in [-36,36] become zero. Button-only reports cannot establish
  analog neutrality.

The new report field sdbz_spawn_boundaries records the first 128
matching dispatch-boundary observations, including original position
setter arguments and input/output of the normalization call. The
signature guard checks six known code words, not a full ELF hash.
Invalid RAM reads are marked invalid; the observer never writes guest
state. Analog observations are candidate reply samples, not a claim
that the original caller's mode test or input branch was taken.

The prior 40M replay report records 2,364 candidate pad replies and
169 Cross-bit-low candidates while SIO2 reports zero scripted Cross
polls. The archive has no RAM or instruction-level vector trace,
so it cannot establish the first incorrect position or velocity write.
A matched new retail replay must verify both position observations and
analog input before attributing the symptom to either path.
