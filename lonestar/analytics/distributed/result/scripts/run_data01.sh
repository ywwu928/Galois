#!/bin/bash

program="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs/bfs-pull-dist"
graph="/work/08474/ywwu/ls6/graphs/galois"
result="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/result/bfs/iec"
process="8 16 32 64"


for i in $process
do
    srun -N 1 -n ${i} $program "$graph/data_01.gr" -graphTranspose="$graph/data_01.tgr" -startNode=26 -t=4 --runs=1 -partition=iec --exec=Sync -statFile="$result/data_01_${i}procs_stat" > "$result/data_01_${i}procs"
done
