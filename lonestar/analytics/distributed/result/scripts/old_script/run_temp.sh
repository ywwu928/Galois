#!/bin/bash

graph="rmat15 cora citeseer flickr yelp"
process="2 4 8 16 32 64"


for i in $graph
do
    for j in $process
    do
        PRINT_PER_THREAD_STATS=1 srun -N 1 -n ${j} ../bfs/bfs-pull-dist ${i}.gr -graphTranspose=${i}.tgr -t=4 -partition=oec --exec=Sync > bfs/${i}_${j}procs
        PRINT_PER_THREAD_STATS=1 srun -N 1 -n ${j} ../connected-components/connected-components-pull-dist ${i}.sgr -symmetricGraph -t=4 -partition=oec --exec=Sync > connected-components/${i}_${j}procs
        PRINT_PER_THREAD_STATS=1 srun -N 1 -n ${j} ../pagerank/pagerank-pull-dist ${i}.gr -graphTranspose=${i}.tgr -t=4 -partition=oec --exec=Sync > pagerank/${i}_${j}procs
        #PRINT_PER_THREAD_STATS=1 GALOIS_DO_NOT_BIND_THREADS=1 mpirun -n=${j} ../sssp/sssp-pull-dist ${i}.gr -graphTranspose=${i}.tgr -t=4 -num_nodes=1 -partition=oec --exec=Sync > sssp/${i}_${j}procs
    done
done
