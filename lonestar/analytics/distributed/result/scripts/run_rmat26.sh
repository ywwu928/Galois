#!/bin/bash

program="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs/bfs-pull-dist"
graph="/work/08474/ywwu/ls6/graphs/galois"
result="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/result/bfs/iec"

srun -N 1 -n 8 $program "$graph/rmat26.gr" -graphTranspose="$graph/rmat26.tgr" -t=16 --runs=1 -partition=iec --exec=Sync -statFile="$result/rmat26_8procs_stat" > "$result/rmat26_8procs"
srun -N 1 -n 16 $program "$graph/rmat26.gr" -graphTranspose="$graph/rmat26.tgr" -t=8 --runs=1 -partition=iec --exec=Sync -statFile="$result/rmat26_16procs_stat" > "$result/rmat26_16procs"
srun -N 1 -n 32 $program "$graph/rmat26.gr" -graphTranspose="$graph/rmat26.tgr" -t=4 --runs=1 -partition=iec --exec=Sync -statFile="$result/rmat26_32procs_stat" > "$result/rmat26_32procs"
srun -N 1 -n 64 $program "$graph/rmat26.gr" -graphTranspose="$graph/rmat26.tgr" -t=2 --runs=1 -partition=iec --exec=Sync -statFile="$result/rmat26_64procs_stat" > "$result/rmat26_64procs"
