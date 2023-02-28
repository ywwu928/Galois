#!/bin/bash

program="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/pagerank/pagerank-push-dist"
graph="/work/08474/ywwu/ls6/graphs/galois"
result="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/result/pagerank/oec"

# srun -N 1 -n 8 $program "$graph/yelp.gr" -graphTranspose="$graph/yelp.tgr" -startNode=662666 -t=16 --runs=1 -partition=iec --exec=Sync -statFile="$result/yelp_8procs_stat" > "$result/yelp_8procs"
# srun -N 1 -n 16 $program "$graph/yelp.gr" -graphTranspose="$graph/yelp.tgr" -startNode=662666 -t=8 --runs=1 -partition=iec --exec=Sync -statFile="$result/yelp_16procs_stat" > "$result/yelp_16procs"
# srun -N 1 -n 32 $program "$graph/yelp.gr" -graphTranspose="$graph/yelp.tgr" -startNode=662666 -t=4 --runs=1 -partition=iec --exec=Sync -statFile="$result/yelp_32procs_stat" > "$result/yelp_32procs"
# srun -N 1 -n 64 $program "$graph/yelp.gr" -graphTranspose="$graph/yelp.tgr" -startNode=662666 -t=2 --runs=1 -partition=iec --exec=Sync -statFile="$result/yelp_64procs_stat" > "$result/yelp_64procs"

# srun -N 1 -n 8 $program "$graph/yelp.gr" -graphTranspose="$graph/yelp.tgr" --maxIterations=100 -t=16 --runs=1 -partition=oec --exec=Sync -statFile="$result/yelp_8procs_stat" > "$result/yelp_8procs"
# srun -N 1 -n 16 $program "$graph/yelp.gr" -graphTranspose="$graph/yelp.tgr" --maxIterations=100 -t=8 --runs=1 -partition=oec --exec=Sync -statFile="$result/yelp_16procs_stat" > "$result/yelp_16procs"
# srun -N 1 -n 32 $program "$graph/yelp.gr" -graphTranspose="$graph/yelp.tgr" --maxIterations=100 -t=4 --runs=1 -partition=oec --exec=Sync -statFile="$result/yelp_32procs_stat" > "$result/yelp_32procs"
srun -N 2 -n 64 $program "$graph/yelp.gr" -graphTranspose="$graph/yelp.tgr" --maxIterations=100 -t=3 --runs=1 -partition=oec --exec=Sync -statFile="$result/yelp_64procs_stat" > "$result/yelp_64procs"
