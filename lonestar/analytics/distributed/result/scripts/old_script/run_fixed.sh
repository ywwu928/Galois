#!/bin/bash

program="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs/bfs-pull-dist"
graph="/work/08474/ywwu/ls6/graphs/galois"
result="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/result/bfs/iec"

srun -N 1 -n 4 $program "$graph/rmat14.gr" -graphTranspose="$graph/rmat14.tgr" -t=32 --runs=1 -partition=iec --exec=Sync -statFile="$result/rmat14_4procs_stat" > "$result/rmat14_4procs"
srun -N 1 -n 16 $program "$graph/rmat16.gr" -graphTranspose="$graph/rmat16.tgr" -t=8 --runs=1 -partition=iec --exec=Sync -statFile="$result/rmat16_16procs_stat" > "$result/rmat16_16procs"
srun -N 1 -n 32 $program "$graph/rmat17.gr" -graphTranspose="$graph/rmat17.tgr" -t=4 --runs=1 -partition=iec --exec=Sync -statFile="$result/rmat17_32procs_stat" > "$result/rmat17_32procs"
srun -N 1 -n 64 $program "$graph/rmat18.gr" -graphTranspose="$graph/rmat18.tgr" -t=2 --runs=1 -partition=iec --exec=Sync -statFile="$result/rmat18_64procs_stat" > "$result/rmat18_64procs"
