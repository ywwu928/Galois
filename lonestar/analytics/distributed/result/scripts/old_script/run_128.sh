#!/bin/bash

program="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs/bfs-push-dist"
# program="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/pagerank/pagerank-push-dist"
graph="/work/08474/ywwu/ls6/graphs/galois"
result="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/result/bfs/push"
# result="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/result/pagerank/oec"

srun -N 4 -n 128 $program "$graph/yelp.gr" -graphTranspose="$graph/yelp.tgr" -startNode=692666 -t=3 --runs=1 -partition=oec --exec=Sync -statFile="$result/yelp_128procs_stat" > "$result/yelp_128procs"
# srun -N 2 -n 128 $program "$graph/data_10.gr" -graphTranspose="$graph/data_10.tgr" -t=2 --runs=1 -partition=iec --exec=Sync -statFile="$result/data_10_128procs_stat" > "$result/data_10_128procs"
srun -N 4 -n 128 $program "$graph/data_200.gr" -graphTranspose="$graph/data_200.tgr" -startNode=896003 -t=3 --runs=1 -partition=oec --exec=Sync -statFile="$result/data_200_128procs_stat" > "$result/data_200_128procs"
# srun -N 2 -n 128 $program "$graph/rmat26.gr" -graphTranspose="$graph/rmat26.tgr" -t=2 --runs=1 -partition=iec --exec=Sync -statFile="$result/rmat26_128procs_stat" > "$result/rmat26_128procs"
# srun -N 2 -n 128 $program "$graph/rmat28.gr" -graphTranspose="$graph/rmat28.tgr" -t=2 --runs=1 -partition=iec --exec=Sync -statFile="$result/rmat28_128procs_stat" > "$result/rmat28_128procs"

# srun -N 4 -n 128 $program "$graph/yelp.gr" -graphTranspose="$graph/yelp.tgr" --maxIterations=100 -t=3 --runs=1 -partition=oec --exec=Sync -statFile="$result/yelp_128procs_stat" > "$result/yelp_128procs"
# srun -N 4 -n 128 $program "$graph/data_200.gr" -graphTranspose="$graph/data_200.tgr" --maxIterations=100 -t=3 --runs=1 -partition=oec --exec=Sync -statFile="$result/data_200_128procs_stat" > "$result/data_200_128procs"
