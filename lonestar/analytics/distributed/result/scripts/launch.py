import sys
import os

# graphs = ["flickr", "yelp", "rmat16", "rmat20", "data_200"]
graphs = ["data_200", "rmat16", "rmat20"]
# graphs = ["data_200"]
hosts = {
    "flickr": ["8", "16", "32", "64"],
    "yelp": ["8", "16", "32", "64", "128"],
    "rmat16": ["8", "16", "32", "64"],
    "rmat20": ["8", "16", "32", "64", "128"],
    "data_200": ["8", "16", "32", "64", "128"]
}
threads = {
    "flickr": ["16", "8", "4", "4"],
    "yelp": ["16", "8", "4", "4", "4"],
    "rmat16": ["16", "8", "4", "4"],
    "rmat20": ["16", "8", "4", "4", "4"],
    "data_200": ["16", "8", "4", "4", "4"]
}
nodes = {
    "flickr": ["1", "1", "1", "2"],
    "yelp": ["1", "1", "1", "2", "4"],
    "rmat16": ["1", "1", "1", "2"],
    "rmat20": ["1", "1", "1", "2", "4"],
    "data_200": ["1", "1", "1", "2", "4"]
}
bfs_time = {
    "flickr": ["00:05:00", "00:15:00", "00:35:00", "00:45:00"],
    "yelp": ["00:08:00", "00:20:00", "00:50:00", "01:05:00", "01:15:00"],
    #"rmat16": ["00:06:30", "00:18:00", "00:40:00", "00:55:00"],
    #"rmat20": ["00:06:30", "00:18:00", "00:40:00", "01:00:00", "01:00:00"],
    #"data_200": ["00:05:00", "00:13:00", "00:25:00", "00:33:00", "00:45:00"]
    "rmat16": ["00:05:00", "00:08:00", "00:15:00", "00:20:00"],
    "rmat20": ["00:05:00", "00:08:00", "00:15:00", "00:25:00", "00:20:00"],
    "data_200": ["00:05:00", "00:08:00", "00:15:00", "00:18:00", "00:20:00"]
}
pagerank_time = {
    "flickr": ["00:15:00", "00:35:00", "02:00:00", "02:00:00"],
    "yelp": ["00:15:00", "00:30:00", "02:00:00", "02:00:00", "02:00:00"],
    "rmat16": ["00:13:00", "00:32:00", "02:00:00", "02:00:00"],
    "rmat20": ["00:17:00", "00:37:00", "02:00:00", "02:00:00", "02:00:00"],
    "data_200": ["00:25:00", "01:00:00", "02:00:00", "02:00:00", "02:00:00"]
}
start_node = {
    "flickr": "50",
    "yelp": "662666",
    "rmat16": "0",
    "rmat20": "0",
    "data_200": "896003"
}
bfs_mode = {
    "idev": ["8", "16", "32"],
    "sbatch": ["8", "16", "32", "64", "128"]
}
pagerank_mode = {
    "idev": ["8", "16", "32", "64", "128"],
    "sbatch": ["8", "16", "32", "64", "128"]
}

bfs_program="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/bfs/bfs-push-dist"
pagerank_program="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/pagerank/pagerank-push-dist"
graph_path="/work/08474/ywwu/ls6/graphs/galois/"
bfs_result="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/result/bfs/push/"
pagerank_result="/work/08474/ywwu/ls6/Galois/lonestar/analytics/distributed/result/pagerank/push/"

if len(sys.argv) != 2:
    print("usage: python3 launch.py (algorithm)")
    sys.exit(0)

algorithm = sys.argv[1]
if algorithm == "bfs":
    time = bfs_time
    program = bfs_program
    result = bfs_result
    mode = bfs_mode
elif algorithm == "pagerank":
    time = pagerank_time
    program = pagerank_program
    result = pagerank_result
    mode=pagerank_mode

for graph in graphs:
    idev_script = open("idev_script_" + graph, "w")
    idev_script.write("#!/bin/bash\n")
    idev_script.write("\n")
    for host in hosts[graph]:
        host_index = hosts[graph].index(host)
        if host in mode["idev"]:
            idev_script.write("echo \"" + graph + " " + host + " hosts start time:\"\n")
            idev_script.write("date\n")
            if algorithm == "bfs":
                idev_script.write("srun -N " + nodes[graph][host_index] + " -n " + host +" " +  program + " \"" + graph_path + graph + ".gr\" -graphTranspose=\"" + graph_path + graph + ".tgr\" -startNode=" + start_node[graph] + " -t=" + threads[graph][host_index] + " --runs=1 -partition=oec --exec=Sync -statFile=\"" + result + graph + "_" + host + "procs_stat\" > \"" + result + graph + "_" + host + "procs\"\n")
            elif algorithm == "pagerank":
                idev_script.write("srun -N " + nodes[graph][host_index] + " -n " + host +" " +  program + " \"" + graph_path + graph + ".gr\" -graphTranspose=\"" + graph_path + graph + ".tgr\" --maxIterations=10 -t=" + threads[graph][host_index] + " --runs=1 -partition=oec --exec=Sync -statFile=\"" + result + graph + "_" + host + "procs_stat\" > \"" + result + graph + "_" + host + "procs\"\n")
            idev_script.write("echo \"" + graph + " " + host + " hosts end time:\"\n")
            idev_script.write("date\n")
        if host in mode["sbatch"]:
            sbatch_script = open("sbatch_script_" + graph + "_" + host, "w")

            sbatch_script.write("#!/bin/bash\n")
            sbatch_script.write("\n")
            sbatch_script.write("#SBATCH -J galois                 # Job name\n")
            sbatch_script.write("#SBATCH -o galois.o%j             # Name of stdout output file\n")
            sbatch_script.write("#SBATCH -e galois.e%j             # Name of stderr error file\n")
            sbatch_script.write("#SBATCH -p normal                 # Queue (partition) name\n")
            sbatch_script.write("#SBATCH -N " + nodes[graph][host_index] + "                      # Total number of nodes\n")
            sbatch_script.write("#SBATCH -n " + str(128*int(nodes[graph][host_index])) + "                    # Total number of mpi tasks\n")
            sbatch_script.write("#SBATCH -t " + time[graph][host_index] + "               # Run time (hh:mm:ss)\n")
            sbatch_script.write("#SBATCH --mail-type=all           # Send email at begin and end of job\n")
            sbatch_script.write("#SBATCH -A CCR22006               # Project/Allocation name\n")
            sbatch_script.write("#SBATCH --mail-user=ywwu928@utexas.edu\n")
            sbatch_script.write("\n")
            sbatch_script.write("module list\n")
            sbatch_script.write("pwd\n")
            sbatch_script.write("date\n")
            sbatch_script.write("\n")
            if algorithm == "bfs":
                sbatch_script.write("srun -N " + nodes[graph][host_index] + " -n " + host +" " +  program + " \"" + graph_path + graph + ".gr\" -graphTranspose=\"" + graph_path + graph + ".tgr\" -startNode=" + start_node[graph] + " -t=" + threads[graph][host_index] + " --runs=1 -partition=oec --exec=Sync -statFile=\"" + result + graph + "_" + host + "procs_stat\" > \"" + result + graph + "_" + host + "procs\"\n")
            elif algorithm == "pagerank":
                sbatch_script.write("srun -N " + nodes[graph][host_index] + " -n " + host +" " +  program + " \"" + graph_path + graph + ".gr\" -graphTranspose=\"" + graph_path + graph + ".tgr\" --maxIterations=10 -t=" + threads[graph][host_index] + " --runs=1 -partition=oec --exec=Sync -statFile=\"" + result + graph + "_" + host + "procs_stat\" > \"" + result + graph + "_" + host + "procs\"\n")
            
            sbatch_script.close()


for graph in graphs:
    for host in hosts[graph]:
        os.system("sbatch sbatch_script_" + graph + "_" + host)

