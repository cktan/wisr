# Benchmark log, Google Cloud VM

**Not comparable with `../results/`.** These runs are on a Google
Cloud VM (AMD EPYC 7B12, 2 cores and 4 threads, 15 GB, ext4 on a
network disk). Those in `../results/` are on other machines, under
WSL. Compare a run here only with another run here.

New logs are not zero-filled here, as they are on WSL
(`../../sm/sm1/WALPREALLOC.md` §2), which alone moves a commit's
sync and a load.

`run.sh` writes to `../results/`: a report from this machine is
moved here, and its row with it, by hand.

| Date | Commit | What | Machine | Report |
| --- | --- | --- | --- | --- |
| 2026-10-31 16:33 | caba5b41 | before merging main | AMD EPYC 7B12 | [20261031-1633](20261031-1633.md) |
| 2026-10-31 17:24 | d94c42d5 | main merged | AMD EPYC 7B12 | [20261031-1724](20261031-1724.md) |
| 2026-10-31 17:28 | d94c42d5 | `btree.sh`, all three kinds | AMD EPYC 7B12 | [btree-20261031](btree-20261031.md) |
