#!/bin/bash

program="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs/bfs-push-dist"
# program="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/pagerank/pagerank-push-dist"
graph="/work/08474/ywwu/ls6/graphs/galois"
result="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/result/bfs/push"
# result="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/result/pagerank/oec"

srun -N 1 -n 8 $program "$graph/flickr.gr" -graphTranspose="$graph/flickr.tgr" -startNode=50 -t=16 --runs=1 -partition=oec --exec=Sync -statFile="$result/flickr_8procs_stat" > "$result/flickr_8procs"
srun -N 1 -n 16 $program "$graph/flickr.gr" -graphTranspose="$graph/flickr.tgr" -startNode=50 -t=8 --runs=1 -partition=oec --exec=Sync -statFile="$result/flickr_16procs_stat" > "$result/flickr_16procs"
srun -N 1 -n 32 $program "$graph/flickr.gr" -graphTranspose="$graph/flickr.tgr" -startNode=50 -t=4 --runs=1 -partition=oec --exec=Sync -statFile="$result/flickr_32procs_stat" > "$result/flickr_32procs"
srun -N 1 -n 64 $program "$graph/flickr.gr" -graphTranspose="$graph/flickr.tgr" -startNode=50 -t=2 --runs=1 -partition=oec --exec=Sync -statFile="$result/flickr_64procs_stat" > "$result/flickr_64procs"

# srun -N 1 -n 8 $program "$graph/flickr.gr" -graphTranspose="$graph/flickr.tgr" --maxIterations=100 -t=16 --runs=1 -partition=oec --exec=Sync -statFile="$result/flickr_8procs_stat" > "$result/flickr_8procs"
# srun -N 1 -n 16 $program "$graph/flickr.gr" -graphTranspose="$graph/flickr.tgr" --maxIterations=100 -t=8 --runs=1 -partition=oec --exec=Sync -statFile="$result/flickr_16procs_stat" > "$result/flickr_16procs"
# srun -N 1 -n 32 $program "$graph/flickr.gr" -graphTranspose="$graph/flickr.tgr" --maxIterations=100 -t=4 --runs=1 -partition=oec --exec=Sync -statFile="$result/flickr_32procs_stat" > "$result/flickr_32procs"
# srun -N 2 -n 64 $program "$graph/flickr.gr" -graphTranspose="$graph/flickr.tgr" --maxIterations=100 -t=3 --runs=1 -partition=oec --exec=Sync -statFile="$result/flickr_64procs_stat" > "$result/flickr_64procs"
