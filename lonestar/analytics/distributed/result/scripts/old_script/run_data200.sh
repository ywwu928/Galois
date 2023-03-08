#!/bin/bash

program="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs/bfs-push-dist"
# program="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/pagerank/pagerank-push-dist"
graph="/work/08474/ywwu/ls6/graphs/galois"
result="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/result/bfs/push"
# result="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/result/pagerank/oec"

# srun -N 1 -n 8 $program "$graph/data_200.gr" -graphTranspose="$graph/data_200.tgr" -startNode=896003 -t=16 --runs=1 -partition=oec --exec=Sync -statFile="$result/data_200_8procs_stat" > "$result/data_200_8procs"
# srun -N 1 -n 16 $program "$graph/data_200.gr" -graphTranspose="$graph/data_200.tgr" -startNode=896003 -t=8 --runs=1 -partition=oec --exec=Sync -statFile="$result/data_200_16procs_stat" > "$result/data_200_16procs"
# srun -N 1 -n 32 $program "$graph/data_200.gr" -graphTranspose="$graph/data_200.tgr" -startNode=896003 -t=4 --runs=1 -partition=oec --exec=Sync -statFile="$result/data_200_32procs_stat" > "$result/data_200_32procs"
srun -N 1 -n 64 $program "$graph/data_200.gr" -graphTranspose="$graph/data_200.tgr" -startNode=896003 -t=2 --runs=1 -partition=oec --exec=Sync -statFile="$result/data_200_64procs_stat" > "$result/data_200_64procs"

# srun -N 1 -n 8 $program "$graph/data_200.gr" -graphTranspose="$graph/data_200.tgr" --maxIterations=100 -t=16 --runs=1 -partition=oec --exec=Sync -statFile="$result/data_200_8procs_stat" > "$result/data_200_8procs"
# srun -N 1 -n 16 $program "$graph/data_200.gr" -graphTranspose="$graph/data_200.tgr" --maxIterations=100 -t=8 --runs=1 -partition=oec --exec=Sync -statFile="$result/data_200_16procs_stat" > "$result/data_200_16procs"
# srun -N 1 -n 32 $program "$graph/data_200.gr" -graphTranspose="$graph/data_200.tgr" --maxIterations=100 -t=4 --runs=1 -partition=oec --exec=Sync -statFile="$result/data_200_32procs_stat" > "$result/data_200_32procs"
# srun -N 2 -n 64 $program "$graph/data_200.gr" -graphTranspose="$graph/data_200.tgr" --maxIterations=100 -t=3 --runs=1 -partition=oec --exec=Sync -statFile="$result/data_200_64procs_stat" > "$result/data_200_64procs"
