# Recovery payload status — 2026-11-11

The recovery branch contains the recovery provenance, resume instructions, work log, and test report.

A source-only ZIP was created locally from the recovered 09:57 source snapshot:
`PS2_EE_Recompiler_Source_Code_2026-11-11.zip`.

An attempt to mirror that ZIP as split Base64 text files under `recovery_payload/` did not pass integrity checks: two parts have lengths that differ from the original chunk plan. **Do not concatenate or decode the `.b64` parts as a valid archive.** No binary ZIP blob has been committed.

The verified local ZIP is attached in the ChatGPT conversation. The archive is the recovered 09:57 snapshot, not the inaccessible later temporary worktree described by the previous chat. Its clean build and tests are recorded in `TEST_RESULTS.md`.

To complete the mirror safely, re-upload the attached ZIP as a normal binary asset or use a local Git client to copy its contents into this branch. Do not merge this recovery branch into `main` until source provenance has been reviewed.
