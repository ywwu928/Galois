import sys
import os

# target directory
directory = sys.argv[1]

# result containers
hosts = ["_8", "_16", "_32", "_64", "_128"]
graphs = ["flickr", "yelp", "rmat15", "data_200"]
# graphs = ["flickr", "yelp", "rmat15", "rmat26", "rmat28", "data_01", "data_10", "data_200"]

master_access=[]
master_write=[]
mirror_access=[]
mirror_write=[]
for i in range(len(graphs)):
    master_access.append(['-1', '-1', '-1', '-1', '-1', '-1'])
    master_write.append(['-1', '-1', '-1', '-1', '-1', '-1'])
    mirror_access.append(['-1', '-1', '-1', '-1', '-1', '-1'])
    mirror_write.append(['-1', '-1', '-1', '-1', '-1', '-1'])

for filename in os.listdir(directory):
    full_path = os.path.join(directory, filename)
    # checking if it is a file
    if os.path.isfile(full_path):
        f = open(full_path, "r")
        for graph in graphs:
            graph_index = graphs.index(graph)
            if filename.find(graph) != -1 and filename.find("_stat") == -1:
                for host in hosts:
                    if filename.find(host) != -1:
                        host_index = hosts.index(host)
                        #print(filename)
                        lines = f.readlines()
                        for row in lines:
                            if row.find('Total access to master nodes') != -1:
                                row_split = row.strip().split()
                                num = row_split[5]
                                master_access[graph_index][host_index] = num;
                            elif row.find('Write access to master nodes') != -1:
                                row_split = row.strip().split()
                                num = row_split[5]
                                master_write[graph_index][host_index] = num;
                            elif row.find('Total access to mirror nodes') != -1:
                                row_split = row.strip().split()
                                num = row_split[5]
                                mirror_access[graph_index][host_index] = num;
                            elif row.find('Write access to mirror nodes') != -1:
                                row_split = row.strip().split()
                                num = row_split[5]
                                mirror_write[graph_index][host_index] = num;
                                break
                        break
                break
        f.close()

# print
#print(total_access)
#print(mirror_access)
#print(mirror_write)
g = open(sys.argv[1] + "/summary", "w")
for graph in graphs:
    g.write("### " + graph + " ###\n")
    graph_index = graphs.index(graph)
    g.write("Master access: ")
    for i in range(len(hosts)):
        g.write(master_access[graph_index][i] + " ")
    g.write("\n")
    g.write("Master write access: ")
    for i in range(len(hosts)):
        g.write(master_write[graph_index][i] + " ")
    g.write("\n")
    g.write("Mirror access: ")
    for i in range(len(hosts)):
        g.write(mirror_access[graph_index][i] + " ")
    g.write("\n")
    g.write("Mirror write access: ")
    for i in range(len(hosts)):
        g.write(mirror_write[graph_index][i] + " ")
    g.write("\n")
    g.write("\n")
g.close()
