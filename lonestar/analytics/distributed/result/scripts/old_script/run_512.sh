#!/bin/bash

program="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs/bfs-pull-dist"
graph="/work/08474/ywwu/ls6/graphs/galois"
result="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/result/bfs/oec"

srun -N 16 -n 512 $program "$graph/data_200.gr" -graphTranspose="$graph/data_200.tgr" -t=3 --runs=1 -partition=oec --exec=Sync -statFile="$result/data_200_512procs_stat" > "$result/data_200_512procs"
# srun -N 2 -n 128 $program "$graph/rmat26.gr" -graphTranspose="$graph/rmat26.tgr" -t=2 --runs=1 -partition=iec --exec=Sync -statFile="$result/rmat26_128procs_stat" > "$result/rmat26_128procs"
# srun -N 2 -n 128 $program "$graph/rmat28.gr" -graphTranspose="$graph/rmat28.tgr" -t=2 --runs=1 -partition=iec --exec=Sync -statFile="$result/rmat28_128procs_stat" > "$result/rmat28_128procs"
