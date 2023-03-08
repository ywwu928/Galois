import sys
import os
import csv
from statistics import median
from statistics import mean
import numpy as np

if len(sys.argv) != 3:
    print("usage: python3 model.py (path to directory of results) (graph name)")
    sys.exit(0)

# target directory
directory = sys.argv[1]
graph = sys.argv[2]

# result containers
graph_hosts = {
    "flickr": [8, 16, 32, 64],
    "yelp": [8, 16, 32, 64, 128],
    "rmat16": [8, 16, 32, 64],
    "rmat20": [8, 16, 32, 64, 128],
    "data_200": [8, 16, 32, 64, 128]
}
hosts = graph_hosts[graph]

master = []
master_write = []
mirror = []
mirror_write = []
master_summary = []
master_write_summary = []
mirror_summary = []
mirror_write_summary = []
mirror_send_update = []
master_recv_update = []

rounds = []

total_master_nodes = []
total_mirror_nodes = []
replication_factor = []

partition_master_nodes = []
partition_mirror_nodes = []

for host in hosts:
    # master.append([[] for i in range(host)])
    # master_write.append([[] for i in range(host)])
    # mirror.append([[] for i in range(host)])
    # mirror_write.append([[] for i in range(host)])
    # mirror_send_update.append([[] for i in range(host)])
    # master_recv_update.append([[] for i in range(host)])
    
    master.append([])
    master_write.append([])
    mirror.append([])
    mirror_write.append([])
    mirror_send_update.append([])
    master_recv_update.append([])
    
    master_summary.append([-1 for i in range(host)])
    master_write_summary.append([-1 for i in range(host)])
    mirror_summary.append([-1 for i in range(host)])
    mirror_write_summary.append([-1 for i in range(host)])
    
    rounds.append(0)

    total_master_nodes.append(0)
    total_mirror_nodes.append(0)
    replication_factor.append(0)

    partition_master_nodes.append([0 for i in range(host)])
    partition_mirror_nodes.append([0 for i in range(host)])

for filename in os.listdir(directory):
    full_path = os.path.join(directory, filename)
    # checking if it is a file
    if os.path.isfile(full_path):
        f = open(full_path, "r")
        if filename.find(graph) != -1:
            if filename.find("_stat") == -1:
                for host in hosts:
                    if filename.find("_" + str(host)) != -1:
                        host_index = hosts.index(host)
                        round_prev = -1
                        lines = f.readlines()
                        for row in lines:
                            if row.find('#####   Round') != -1:
                                row_split = row.strip().split()
                                num = row_split[2]
                                if num != round_prev:
                                    rounds[host_index] += 1
                                    master[host_index].append([-1 for i in range(host)])
                                    master_write[host_index].append([-1 for i in range(host)])
                                    mirror[host_index].append([-1 for i in range(host)])
                                    mirror_write[host_index].append([-1 for i in range(host)])
                                    mirror_send_update[host_index].append([-1 for i in range(host)])
                                    master_recv_update[host_index].append([-1 for i in range(host)])
                                    round_prev = num
                            elif row.find('round master accesses') != -1:
                                row_split = row.strip().split()
                                host_id = int(row_split[1])
                                num = row_split[5]
                                master[host_index][rounds[host_index]-1][host_id] = int(num)
                            elif row.find('round master writes') != -1:
                                row_split = row.strip().split()
                                host_id = int(row_split[1])
                                num = row_split[5]
                                master_write[host_index][rounds[host_index]-1][host_id] = int(num)
                            elif row.find('round mirror accesses') != -1:
                                row_split = row.strip().split()
                                host_id = int(row_split[1])
                                num = row_split[5]
                                mirror[host_index][rounds[host_index]-1][host_id] = int(num)
                            elif row.find('round mirror writes') != -1:
                                row_split = row.strip().split()
                                host_id = int(row_split[1])
                                num = row_split[5]
                                mirror_write[host_index][rounds[host_index]-1][host_id] = int(num)
                            elif row.find('number of mirrors sending updates') != -1:
                                row_split = row.strip().split()
                                host_id = int(row_split[1])
                                num = row_split[7]
                                mirror_send_update[host_index][rounds[host_index]-1][host_id] = int(num)
                            elif row.find('receiving updates') != -1:
                                row_split = row.strip().split()
                                host_id = int(row_split[1])
                                num = row_split[4]
                                master_recv_update[host_index][rounds[host_index]-1][host_id] = int(num)
                            elif row.find('total master accesses') != -1:
                                row_split = row.strip().split()
                                host_id = int(row_split[1])
                                num = row_split[5]
                                master_summary[host_index][host_id] = int(num)
                            elif row.find('total master writes') != -1:
                                row_split = row.strip().split()
                                host_id = int(row_split[1])
                                num = row_split[5]
                                master_write_summary[host_index][host_id] = int(num)
                            elif row.find('total mirror accesses') != -1:
                                row_split = row.strip().split()
                                host_id = int(row_split[1])
                                num = row_split[5]
                                mirror_summary[host_index][host_id] = int(num)
                            elif row.find('total mirror writes') != -1:
                                row_split = row.strip().split()
                                host_id = int(row_split[1])
                                num = row_split[5]
                                mirror_write_summary[host_index][host_id] = int(num)
            elif filename.find("_stat") != -1:
                for host in hosts:
                    if filename.find("_" + str(host)) != -1:
                        host_index = hosts.index(host)
                        lines = f.readlines()
                        for row in lines:
                            if row.find('TotalNodes') != -1:
                                row_split = row.strip().split()
                                num = row_split[5]
                                total_master_nodes[host_index] = int(num)
                            elif row.find('TotalGlobalMirrorNodes') != -1:
                                row_split = row.strip().split()
                                num = row_split[5]
                                total_mirror_nodes[host_index] = int(num)
                            elif row.find('NumMasterNodesOf') != -1:
                                row_split = row.strip().split(", ")
                                string = row_split[3]
                                string_split = string.split("_")
                                host_id = int(string_split[1])
                                num = int(row_split[5])
                                partition_master_nodes[host_index][host_id] = int(num)
                            elif row.find('MirrorNodesFrom') != -1:
                                row_split = row.strip().split(", ")
                                string = row_split[3]
                                string_split = string.split("_")
                                host_id = int(string_split[1])
                                num = int(row_split[5])
                                partition_mirror_nodes[host_index][host_id] += int(num)
                            elif row.find('ReplicationFactor') != -1:
                                row_split = row.strip().split()
                                num = row_split[5]
                                replication_factor[host_index] = float(num)

master_nodes_stats = []
mirror_nodes_stats = []
master_stats = []
master_write_stats = []
mirror_stats = []
mirror_write_stats = []
total_stats = []
mirror_send_update_stats = []
master_recv_update_stats = []

for host in hosts:
    master_nodes_stats.append([])
    mirror_nodes_stats.append([])
    master_stats.append([])
    master_write_stats.append([])
    mirror_stats.append([])
    mirror_write_stats.append([])
    total_stats.append([])
    mirror_send_update_stats.append([])
    master_recv_update_stats.append([])

    host_index = hosts.index(host)

    # order: median, average, max
    # index 0 is median
    # index 1 is average
    # index 2 is max
    # index 3 is index for max
    master_nodes_stats[host_index].append(median(partition_master_nodes[host_index]))
    master_nodes_stats[host_index].append(mean(partition_master_nodes[host_index]))
    master_nodes_stats[host_index].append(max(partition_master_nodes[host_index]))
    master_nodes_stats[host_index].append(np.argmax(partition_master_nodes[host_index]))
    mirror_nodes_stats[host_index].append(median(partition_mirror_nodes[host_index]))
    mirror_nodes_stats[host_index].append(mean(partition_mirror_nodes[host_index]))
    mirror_nodes_stats[host_index].append(max(partition_mirror_nodes[host_index]))
    mirror_nodes_stats[host_index].append(np.argmax(partition_mirror_nodes[host_index]))

    for round_num in range(rounds[host_index]):
        master_stats[host_index].append([])
        master_write_stats[host_index].append([])
        mirror_stats[host_index].append([])
        mirror_write_stats[host_index].append([])
        total_stats[host_index].append([])
        mirror_send_update_stats[host_index].append([])
        master_recv_update_stats[host_index].append([])

        master_stats[host_index][round_num].append(median(master[host_index][round_num]))
        master_stats[host_index][round_num].append(mean(master[host_index][round_num]))
        master_stats[host_index][round_num].append(max(master[host_index][round_num]))
        master_stats[host_index][round_num].append(np.argmax(master[host_index][round_num]))
        master_write_stats[host_index][round_num].append(median(master_write[host_index][round_num]))
        master_write_stats[host_index][round_num].append(mean(master_write[host_index][round_num]))
        master_write_stats[host_index][round_num].append(max(master_write[host_index][round_num]))
        master_write_stats[host_index][round_num].append(np.argmax(master_write[host_index][round_num]))
        mirror_stats[host_index][round_num].append(median(mirror[host_index][round_num]))
        mirror_stats[host_index][round_num].append(mean(mirror[host_index][round_num]))
        mirror_stats[host_index][round_num].append(max(mirror[host_index][round_num]))
        mirror_stats[host_index][round_num].append(np.argmax(mirror[host_index][round_num]))
        mirror_write_stats[host_index][round_num].append(median(mirror_write[host_index][round_num]))
        mirror_write_stats[host_index][round_num].append(mean(mirror_write[host_index][round_num]))
        mirror_write_stats[host_index][round_num].append(max(mirror_write[host_index][round_num]))
        mirror_write_stats[host_index][round_num].append(np.argmax(mirror_write[host_index][round_num]))
        total_stats[host_index][round_num].append(median(np.add(master[host_index][round_num], mirror[host_index][round_num])))
        total_stats[host_index][round_num].append(mean(np.add(master[host_index][round_num], mirror[host_index][round_num])))
        total_stats[host_index][round_num].append(max(np.add(master[host_index][round_num], mirror[host_index][round_num])))
        total_stats[host_index][round_num].append(np.argmax(np.add(master[host_index][round_num], mirror[host_index][round_num])))
        mirror_send_update_stats[host_index][round_num].append(median(mirror_send_update[host_index][round_num]))
        mirror_send_update_stats[host_index][round_num].append(mean(mirror_send_update[host_index][round_num]))
        mirror_send_update_stats[host_index][round_num].append(max(mirror_send_update[host_index][round_num]))
        mirror_send_update_stats[host_index][round_num].append(np.argmax(mirror_send_update[host_index][round_num]))
        master_recv_update_stats[host_index][round_num].append(median(master_recv_update[host_index][round_num]))
        master_recv_update_stats[host_index][round_num].append(mean(master_recv_update[host_index][round_num]))
        master_recv_update_stats[host_index][round_num].append(max(master_recv_update[host_index][round_num]))
        master_recv_update_stats[host_index][round_num].append(np.argmax(master_recv_update[host_index][round_num]))

# print
# print(master_stats)
# print(mirror_stats)
# print(total_stats)
# print(rounds)
# print(mirror_send_update)
# print(master_recv_update)
# print(master_summary)

data_size = 8
id_size = 8
addr_size = 8
cache_line_size = 64

edge_vertex_ratio = {
        "flickr": 10.08,
        "yelp": 19.47,
        "rmat16": 16,
        "rmat20": 16,
        "data_200": 3.76
}

local_bw = 4e12
remote_bw = 512e9

baseline_time = []
gluon_time = []
gluon_speedup = []

for i in range(len(hosts)):
    baseline_time.append(0)
    gluon_time.append(0)
    gluon_speedup.append(0)

round_baseline_local_time = []
round_baseline_remote_time = []
round_baseline_time = []
round_gluon_compute_time = []
round_gluon_communication_time = []
round_gluon_time = []
round_gluon_speedup = []

for host in hosts:
    round_baseline_local_time.append([])
    round_baseline_remote_time.append([])
    round_baseline_time.append([])
    round_gluon_compute_time.append([])
    round_gluon_communication_time.append([])
    round_gluon_time.append([])
    round_gluon_speedup.append([])

for host in hosts:
    host_index = hosts.index(host)
    for round_num in range(rounds[host_index]):
        if mirror_send_update_stats[host_index][round_num][2] == 0:
            remote_access_ratio = 0;
        else:
            remote_access_ratio = mirror_stats[host_index][round_num][2] / mirror_send_update_stats[host_index][round_num][2]
        baseline_local_node_time = ((master_nodes_stats[host_index][2] * data_size + edge_vertex_ratio[graph] * master_nodes_stats[host_index][2] * id_size) / cache_line_size) / local_bw
        baseline_local_edge_time = (master_stats[host_index][round_num][2] * cache_line_size) / local_bw
        baseline_local_fetch_respond_time = (remote_access_ratio * master_recv_update_stats[host_index][round_num][2] * cache_line_size) / local_bw
        baseline_local_time = baseline_local_node_time + baseline_local_edge_time + baseline_local_fetch_respond_time
        round_baseline_local_time[host_index].append(baseline_local_time)

        baseline_remote_send_request_time_1 = (mirror_stats[host_index][round_num][2] * addr_size) / remote_bw
        baseline_remote_send_respond_time_1 = (remote_access_ratio * master_recv_update[host_index][round_num][mirror_stats[host_index][round_num][3]] * data_size) / remote_bw
        baseline_remote_send_request_time_2 = (mirror[host_index][round_num][master_recv_update_stats[host_index][round_num][3]] * addr_size) / remote_bw
        baseline_remote_send_respond_time_2 = (remote_access_ratio * master_recv_update_stats[host_index][round_num][2] * data_size) / remote_bw
        baseline_remote_send_time = max(baseline_remote_send_request_time_1+baseline_remote_send_respond_time_1, baseline_remote_send_request_time_2+baseline_remote_send_respond_time_2)
        baseline_remote_receive_request_time_1 = (remote_access_ratio * master_recv_update_stats[host_index][round_num][2] * addr_size) / remote_bw
        baseline_remote_receive_respond_time_1 = (mirror[host_index][round_num][master_recv_update_stats[host_index][round_num][3]] * data_size) / remote_bw
        baseline_remote_receive_request_time_2 = (remote_access_ratio * master_recv_update[host_index][round_num][mirror_stats[host_index][round_num][3]] * addr_size) / remote_bw
        baseline_remote_receive_respond_time_2 = (mirror_stats[host_index][round_num][2] * data_size) / remote_bw
        baseline_remote_receive_time = max(baseline_remote_receive_request_time_1+baseline_remote_receive_respond_time_1, baseline_remote_receive_request_time_2+baseline_remote_receive_respond_time_2)
        baseline_remote_time = max(baseline_remote_send_time, baseline_remote_receive_time)
        # print(baseline_remote_time)
        round_baseline_remote_time[host_index].append(baseline_remote_time)

        baseline_round_time = max(baseline_local_time, baseline_remote_time)
        round_baseline_time[host_index].append(baseline_round_time)
        baseline_time[host_index] += baseline_round_time

        gluon_compute_node_time = ((master_nodes_stats[host_index][2] * data_size + edge_vertex_ratio[graph] * master_nodes_stats[host_index][2] * id_size) / cache_line_size) / local_bw
        gluon_compute_edge_time = (total_stats[host_index][round_num][2] * cache_line_size) / local_bw
        gluon_compute_time = gluon_compute_node_time + gluon_compute_edge_time
        round_gluon_compute_time[host_index].append(gluon_compute_time)
        
        gluon_communication_send_data_time = (mirror_send_update_stats[host_index][round_num][2] * data_size) / remote_bw
        gluon_communication_receive_data_time = (master_recv_update_stats[host_index][round_num][2] * (replication_factor[host_index] - 1) * data_size) / remote_bw
        gluon_communication_mirror_collect_time = max(gluon_communication_send_data_time, gluon_communication_receive_data_time)
        # print(gluon_communication_master_data_collect_time)
        # print(gluon_communication_mirror_send_time)
        # print(gluon_communication_mirror_collect_time)

        # master_update_ratio = (mirror_write[host_index][round_num] / (replication_factor[host_index] - 1)) / total_master_nodes[host_index]
        # gluon_communication_mirror_data_collect_time = (master_update_ratio * (mirror_nodes_max[host_index] / (replication_factor[host_index] - 1)) * data_size) / remote_bw
        # gluon_communication_master_send_time = (master_update_ratio * master_nodes_max[host_index] * data_size) / remote_bw
        # gluon_communication_mirror_update_time = max(gluon_communication_mirror_data_collect_time, gluon_communication_master_send_time)

        # gluon_communication_time = gluon_communication_mirror_collect_time + gluon_communication_mirror_update_time
        gluon_communication_time = gluon_communication_mirror_collect_time
        round_gluon_communication_time[host_index].append(gluon_communication_time)

        gluon_round_time = gluon_compute_time + gluon_communication_time
        #print(gluon_round_time)
        round_gluon_time[host_index].append(gluon_round_time)
        gluon_time[host_index] += gluon_round_time

        gluon_round_speedup = baseline_round_time / gluon_round_time
        round_gluon_speedup[host_index].append(gluon_round_speedup)

    # print(gluon_time[host_index])
    gluon_speedup[host_index] = baseline_time[host_index] / gluon_time[host_index]

# print(baseline_time)
# print(gluon_time)
# print(gluon_speedup)

stats = ["(median)", "(average)", "(max)"]

with open(directory + "/summary_" + graph + ".csv", "w", newline='') as summary:
    writer = csv.writer(summary)
    writer.writerow([graph])

    row_host = ["hosts"]
    row_total_master_nodes = ["total master nodes"]
    row_total_mirror_nodes = ["total mirror nodes"]
    row_replication_factor = ["replication factor"]
    
    rows_master_nodes = []
    rows_mirror_nodes = []
    for stat in stats:
        rows_master_nodes.append(["master nodes " + stat])
        rows_mirror_nodes.append(["mirror nodes " + stat])

    row_rounds = ["rounds"]
    # row_total_master_access = ["total master access"]
    # row_total_mirror_access = ["total mirror access"]
    # row_total_mirror_write = ["total mirror write"]
    row_baseline_time = ["baseline time"] 
    row_gluon_time = ["gluon time"] 
    row_gluon_speedup = ["gluon speedup"] 
    
    for host in hosts:
        host_index = hosts.index(host)
        row_host.append(host)
        row_total_master_nodes.append(total_master_nodes[host_index])
        row_total_mirror_nodes.append(total_mirror_nodes[host_index])
        row_replication_factor.append(replication_factor[host_index])

        for stat in stats:
            stat_index = stats.index(stat)
            rows_master_nodes[stat_index].append(master_nodes_stats[host_index][stat_index])
            rows_mirror_nodes[stat_index].append(mirror_nodes_stats[host_index][stat_index])

        row_rounds.append(rounds[host_index])
        # row_total_master_access.append(master_summary[host_index])
        # row_total_mirror_access.append(mirror_summary[host_index])
        # row_total_mirror_write.append(mirror_write_summary[host_index])
        row_baseline_time.append(baseline_time[host_index])
        row_gluon_time.append(gluon_time[host_index]) 
        row_gluon_speedup.append(gluon_speedup[host_index]) 
    
    writer.writerow(row_host)
    writer.writerow(row_total_master_nodes)
    writer.writerow(row_total_mirror_nodes)
    writer.writerow(row_replication_factor)
    writer.writerow([])

    writer.writerows(rows_master_nodes)
    writer.writerows(rows_mirror_nodes)
    writer.writerow([])

    writer.writerow(row_rounds)
    # writer.writerow(row_total_master_access)
    # writer.writerow(row_total_mirror_access)
    # writer.writerow(row_total_mirror_write)
    # writer.writerow([])
    writer.writerow(row_baseline_time) 
    writer.writerow(row_gluon_time)
    writer.writerow(row_gluon_speedup)
    writer.writerow([])
    
    max_round = max(rounds)
    for round_num in range(max_round):
        row_round = ["Round " + str(round_num)]

        rows_round_master_access = []
        rows_round_mirror_access = []
        rows_round_mirror_write = []
        rows_round_mirror_send_update = []
        rows_round_master_recv_update = []
        for stat in stats:
            rows_round_master_access.append(["master access " + stat])
            rows_round_mirror_access.append(["mirror access " + stat])
            rows_round_mirror_write.append(["mirror write " + stat])
            rows_round_mirror_send_update.append(["mirror nodes sending updates " + stat])
            rows_round_master_recv_update.append(["master nodes receiving updates " + stat])

        row_round_baseline_local_time = ["baseline local time"] 
        row_round_baseline_remote_time = ["baseline remote time"] 
        row_round_baseline_time = ["baseline time"] 
        row_round_gluon_compute_time = ["gluon compute time"] 
        row_round_gluon_communication_time = ["gluon communication time"] 
        row_round_gluon_time = ["gluon time"] 
        row_round_gluon_speedup = ["gluon speedup"] 
        
        for host in hosts:
            host_index = hosts.index(host)
            if round_num < rounds[host_index]:
                for stat in stats:
                    stat_index = stats.index(stat)
                    rows_round_master_access[stat_index].append(master_stats[host_index][round_num][stat_index])
                    rows_round_mirror_access[stat_index].append(mirror_stats[host_index][round_num][stat_index])
                    rows_round_mirror_write[stat_index].append(mirror_write_stats[host_index][round_num][stat_index])
                    rows_round_mirror_send_update[stat_index].append(mirror_send_update_stats[host_index][round_num][stat_index])
                    rows_round_master_recv_update[stat_index].append(master_recv_update_stats[host_index][round_num][stat_index])

                row_round_baseline_local_time.append(round_baseline_local_time[host_index][round_num])
                row_round_baseline_remote_time.append(round_baseline_remote_time[host_index][round_num])
                row_round_baseline_time.append(round_baseline_time[host_index][round_num])
                row_round_gluon_compute_time.append(round_gluon_compute_time[host_index][round_num]) 
                row_round_gluon_communication_time.append(round_gluon_communication_time[host_index][round_num]) 
                row_round_gluon_time.append(round_gluon_time[host_index][round_num])
                row_round_gluon_speedup.append(round_gluon_speedup[host_index][round_num])
            else:
                for stat in stats:
                    stat_index = stats.index(stat)
                    rows_round_master_access[stat_index].append("x")
                    rows_round_mirror_access[stat_index].append("x")
                    rows_round_mirror_write[stat_index].append("x")
                    rows_round_mirror_send_update[stat_index].append("x")
                    rows_round_master_recv_update[stat_index].append("x")
                
                row_round_baseline_local_time.append("x")
                row_round_baseline_remote_time.append("x")
                row_round_baseline_time.append("x")
                row_round_gluon_compute_time.append("x") 
                row_round_gluon_communication_time.append("x") 
                row_round_gluon_time.append("x")
                row_round_gluon_speedup.append("x")

        writer.writerow(row_round)
        writer.writerows(rows_round_master_access)
        writer.writerows(rows_round_mirror_access)
        writer.writerows(rows_round_mirror_write)
        writer.writerows(rows_round_mirror_send_update)
        writer.writerows(rows_round_master_recv_update)
        writer.writerow([])

        writer.writerow(row_round_baseline_local_time) 
        writer.writerow(row_round_baseline_remote_time) 
        writer.writerow(row_round_baseline_time) 
        writer.writerow(row_round_gluon_compute_time)
        writer.writerow(row_round_gluon_communication_time)
        writer.writerow(row_round_gluon_time)
        writer.writerow(row_round_gluon_speedup)
        writer.writerow([])

