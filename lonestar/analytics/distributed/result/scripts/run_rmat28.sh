#!/bin/bash

program="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs/bfs-pull-dist"
graph="/work/08474/ywwu/ls6/graphs/galois"
result="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/result/bfs/mirror_read"

srun -N 1 -n 8 $program "$graph/rmat28.gr" -graphTranspose="$graph/rmat28.tgr" -t=16 --runs=1 -partition=oec --exec=Sync -statFile="$result/rmat28_8procs_stat" > "$result/rmat28_8procs"
srun -N 1 -n 16 $program "$graph/rmat28.gr" -graphTranspose="$graph/rmat28.tgr" -t=8 --runs=1 -partition=oec --exec=Sync -statFile="$result/rmat28_16procs_stat" > "$result/rmat28_16procs"
srun -N 1 -n 32 $program "$graph/rmat28.gr" -graphTranspose="$graph/rmat28.tgr" -t=4 --runs=1 -partition=oec --exec=Sync -statFile="$result/rmat28_32procs_stat" > "$result/rmat28_32procs"
srun -N 1 -n 64 $program "$graph/rmat28.gr" -graphTranspose="$graph/rmat28.tgr" -t=2 --runs=1 -partition=oec --exec=Sync -statFile="$result/rmat28_64procs_stat" > "$result/rmat28_64procs"
