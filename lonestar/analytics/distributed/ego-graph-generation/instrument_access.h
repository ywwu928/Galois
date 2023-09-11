#include <map>

static cll::opt<std::string> graphName("graphName", cll::desc("Name of the input graph"), cll::init("temp"));

template <typename Graph>
struct Instrument {
    Graph* graph;
    uint64_t hostID;
    uint64_t numHosts;

    std::atomic<uint64_t> access_count;

    std::vector<uint64_t> node_read;
    std::vector<uint64_t> node_write;
    std::vector<uint64_t> node_access;

    std::map<int, uint64_t> hist_map_read;
    std::map<int, uint64_t> hist_map_write;
    std::map<int, uint64_t> hist_map_access;

    std::vector<uint64_t> distance_vector;
    std::map<int, uint64_t> hist_map_distance;

    std::vector<uint64_t> node_workset;
    std::map<int, uint64_t> hist_map_workset;

    std::shared_ptr<galois::substrate::SimpleLock> vector_lock;
    std::shared_ptr<galois::substrate::SimpleLock> map_lock;
  
    std::ofstream file;

    void init(uint64_t hid, uint64_t numH, std::unique_ptr<Graph>& g) {
        graph    = g.get();
        hostID   = hid;
        numHosts = numH;
  
        vector_lock = std::make_shared<galois::substrate::SimpleLock>();
        map_lock = std::make_shared<galois::substrate::SimpleLock>();
    
        // start instrumentation
        file.open(graphName + "_" + std::to_string(numH) + "procs_id" + std::to_string(hid));
        file << "#####   Stat   #####" << std::endl;
        file << "host " << hostID << " total mirrors: " << graph->numMirrors() << std::endl;
        file << "host " << hostID << " total edges: " << graph->sizeEdges() << std::endl;
    }

    void init_reuse() {
        auto mirrorSize = graph->numMirrors();
        distance_vector.reserve(mirrorSize);
    }

    void record_read_random(typename Graph::GraphNode node) {
        access_count.fetch_add(1, std::memory_order_relaxed);

        auto gid = graph->getGID(node);
        auto owned = graph->isOwned(gid);
        if (!owned) { // mirror
            auto& nodeData = graph->getData(node);
            nodeData.read.fetch_add(1, std::memory_order_relaxed);

            if (nodeData.touched.load()) { // touched before in this run
                distance_vector.push_back(access_count - nodeData.last_access_touched);
            }
            else { // first time touching in this run
                nodeData.workset_touched.fetch_add(1, std::memory_order_relaxed);
            }

            nodeData.touched = true;
            nodeData.last_access_touched.store(access_count);
        }
    }

    void record_write_random(typename Graph::GraphNode node) {
        access_count.fetch_add(1, std::memory_order_relaxed);
        
        auto gid = graph->getGID(node);
        auto owned = graph->isOwned(gid);
        if (!owned) { // mirror
            auto& nodeData = graph->getData(node);
            nodeData.write.fetch_add(1, std::memory_order_relaxed);
            
            if (nodeData.touched.load()) { // touched before in this run
                distance_vector.push_back(access_count - nodeData.last_access_touched);
            }
            else { // first time touching in this run
                nodeData.workset_touched.fetch_add(1, std::memory_order_relaxed);
            }

            nodeData.touched = true;
            nodeData.last_access_touched.store(access_count);
        }
    }
    
    void log_run(uint64_t run) {
        file << "#####   Run " << run << "   #####" << std::endl;
    }

    void load_access () {
        auto mirrorSize = graph->numMirrors();
        node_read.reserve(mirrorSize);
        node_write.reserve(mirrorSize);
        node_access.reserve(mirrorSize);

        const auto& allMirrorNodes = graph->mirrorNodesRange();
        galois::do_all(
            galois::iterate(allMirrorNodes),
            [&](auto node) {
                auto& nodeData = graph->getData(node);

                vector_lock->lock();
                node_read.push_back(nodeData.read);
                node_write.push_back(nodeData.write);
                node_access.push_back(nodeData.read+nodeData.write);
                vector_lock->unlock();
            },
            galois::steal(), galois::no_stats());
    }

    void bin_access () {
        for (int i=0; i<11; i++) {
            hist_map_read.insert({i, 0});
            hist_map_write.insert({i, 0});
            hist_map_access.insert({i, 0});
        }

        for (auto read: node_read) {
            if (read > 10) {
                map_lock->lock();
                hist_map_read[10]++;
                map_lock->unlock();
            }
            else {
                map_lock->lock();
                hist_map_read[read]++;
                map_lock->unlock();
            }
        }
        
        for (auto write: node_write) {
            if (write > 10) {
                map_lock->lock();
                hist_map_write[10]++;
                map_lock->unlock();
            }
            else {
                map_lock->lock();
                hist_map_write[write]++;
                map_lock->unlock();
            }
        }
        
        for (auto access: node_access) {
            if (access > 10) {
                map_lock->lock();
                hist_map_access[10]++;
                map_lock->unlock();
            }
            else {
                map_lock->lock();
                hist_map_access[access]++;
                map_lock->unlock();
            }
        }
    }

    void access_clear() {
        node_read.clear();
        node_write.clear();
        node_access.clear();
        hist_map_read.clear();
        hist_map_write.clear();
        hist_map_access.clear();
    }

    void log_access() {
        load_access();
        bin_access();
        
        for (int i=0; i<11; i++) {
            file << "host " << hostID << " number of mirrors with " << i << " reads: " << hist_map_read[i] << std::endl;
        }
        
        for (int i=0; i<11; i++) {
            file << "host " << hostID << " number of mirrors with " << i << " writes: " << hist_map_write[i] << std::endl;
        }
        
        for (int i=0; i<11; i++) {
            file << "host " << hostID << " number of mirrors with " << i << " accesses: " << hist_map_access[i] << std::endl;
        }
        
        access_clear();
    }
    
    void bin_distance () {
        for (int i=6; i<21; i++) {
            hist_map_distance.insert({i, 0});
        }
        for (auto distance: distance_vector) {
            double exp = std::log2(distance);
            if (exp < 6) {
                map_lock->lock();
                hist_map_distance[6]++;
                map_lock->unlock();
            }
            else if (exp > 20) {
                map_lock->lock();
                hist_map_distance[20]++;
                map_lock->unlock();
            }
            else{
                int key = (int)exp;
                map_lock->lock();
                hist_map_distance[key]++;
                map_lock->unlock();
            }
        }
    }

    void distance_clear() {
        distance_vector.clear();
        hist_map_distance.clear();
    }

    void log_distance() {
        bin_distance();
        
        for (int i=6; i<21; i++) {
            file << "host " << hostID << " number of reuse distances within bin " << i << " : " << hist_map_distance[i] << std::endl;
        }
        
        distance_clear();
    }
    
    void load_workset () {
        auto mirrorSize = graph->numMirrors();
        node_workset.reserve(mirrorSize);

        const auto& allMirrorNodes = graph->mirrorNodesRange();
        galois::do_all(
            galois::iterate(allMirrorNodes),
            [&](auto node) {
                auto& nodeData = graph->getData(node);

                vector_lock->lock();
                node_workset.push_back(nodeData.workset_touched);
                vector_lock->unlock();
            },
            galois::steal(), galois::no_stats());
    }

    void bin_workset () {
        for (int i=0; i<15; i++) {
            hist_map_workset.insert({i, 0});
        }

        for (auto workset: node_workset) {
            if (workset > 15) {
                map_lock->lock();
                hist_map_workset[15]++;
                map_lock->unlock();
            }
            else {
                map_lock->lock();
                hist_map_workset[workset]++;
                map_lock->unlock();
            }
        }
    }

    void workset_clear() {
        node_workset.clear();
        hist_map_workset.clear();
    }

    void log_workset() {
        file << "#####   Workset Summary   #####" << std::endl;
       
        load_workset();
        bin_workset();

        for (int i=0; i<15; i++) {
            file << "host " << hostID << " number of mirrors touched by " << i << " worksets: " << hist_map_workset[i] << std::endl;
        }

        workset_clear();
    }

};
