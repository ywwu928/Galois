#include <map>

static cll::opt<std::string> graphName("graphName", cll::desc("Name of the input graph"), cll::init("temp"));

constexpr int CACHE_BOUND = 25;
constexpr int CACHE_SAMPLES = 5;

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

    // size of total nodes; cache level for each mirror (lower level higher in-degree)
    std::vector<int> node_cache_level;

    std::vector<uint64_t> node_workset;
    std::vector<int> node_workset_level;
    std::map<int, std::vector<uint64_t>> hist_map_workset;
  
    std::shared_ptr<galois::substrate::SimpleLock> vector_lock;
    std::shared_ptr<galois::substrate::SimpleLock> map_lock;
  
    std::ofstream file;

    void init(uint64_t hid, uint64_t numH, std::unique_ptr<Graph>& g) {
        graph    = g.get();
        hostID   = hid;
        numHosts = numH;
  
        vector_lock = std::make_shared<galois::substrate::SimpleLock>();
        map_lock = std::make_shared<galois::substrate::SimpleLock>();
        
        node_cache_level.resize(graph->size());
        // load transposed graph (to count incoming degrees)
        std::vector<unsigned> _;
        auto tgr = loadDistGraph<typename Graph::GraphNode, typename Graph::EdgeType, /*iterateOutEdges*/ false>(_);

        // in-degree counting (using transposed graph)
        galois::InsertBag<std::pair<uint64_t, typename Graph::GraphNode>> indeg_nodes;
        const auto& allNodes = tgr->allNodesRange();
        galois::do_all(
            galois::iterate(allNodes),
            [&](auto node) {
            // ignore master nodes
            if (tgr->isOwned(tgr->getGID(node))) {
                return;
            }
            indeg_nodes.emplace(std::make_pair(std::distance(tgr->edge_begin(node), tgr->edge_end(node)), node));
            },
            galois::steal(), galois::no_stats());

        // descending sort
        std::multimap<uint64_t, typename Graph::GraphNode, std::greater<int>> sorted_indeg_nodes(indeg_nodes.begin(), indeg_nodes.end());
        // cut into levels
        int64_t sorted_indeg_nodes_bound = sorted_indeg_nodes.size() * CACHE_BOUND / 100;
        auto [level_size, surplus] = std::div(sorted_indeg_nodes_bound, (int64_t)CACHE_SAMPLES);
        auto it = sorted_indeg_nodes.begin();
        auto end = std::next(it, sorted_indeg_nodes_bound);
        for (int cache_level = 1; cache_level <= CACHE_SAMPLES; cache_level++) {
          if (surplus) {
            end = std::next(it, level_size + 1);
            surplus--;
          } else {
            end = std::next(it, level_size);
          }
          galois::do_all(
              galois::iterate(it, end),
              [&](auto& deg_node) {
                auto& [deg, node]      = deg_node;
                node_cache_level[node] = cache_level;
              },
              galois::no_stats());
          it = end;
        }
        
        it = std::next(sorted_indeg_nodes.begin(), sorted_indeg_nodes_bound);
        end = sorted_indeg_nodes.end();
        galois::do_all(
            galois::iterate(it, end),
            [&](auto& deg_node) {
                auto& [deg, node]      = deg_node;
                node_cache_level[node] = -1;
            },
            galois::no_stats());
    
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
        node_workset_level.reserve(mirrorSize);

        const auto& allMirrorNodes = graph->mirrorNodesRange();
        galois::do_all(
            galois::iterate(allMirrorNodes),
            [&](auto node) {
                auto& nodeData = graph->getData(node);

                vector_lock->lock();
                node_workset.push_back(nodeData.workset_touched);
                node_workset_level.push_back(node_cache_level[node]);
                vector_lock->unlock();
            },
            galois::steal(), galois::no_stats());
    }

    void bin_workset () {
        for (int i=0; i<22; i++) {
            std::vector<uint64_t> empty_vector (CACHE_SAMPLES+1, 0);
            hist_map_workset.insert({i, empty_vector});
        }

        for (auto i=0ul; i<node_workset.size(); i++) {
            uint64_t workset = node_workset[i];
            int workset_level = node_workset_level[i];

            if (workset <= 15) {
                map_lock->lock();
                hist_map_workset[workset].at(0)++;
                if (workset_level > 0) {
                    for (int j=workset_level; j<=CACHE_SAMPLES; j++) {
                        hist_map_workset[workset].at(j)++;
                    }
                }
                map_lock->unlock();
            }
            else {
                double exp = std::log2(workset);
                int key = (int)exp;
                if (exp > 10) {
                    map_lock->lock();
                    hist_map_workset[21].at(0)++;
                    if (workset_level > 0) {
                        for (int j=workset_level; j<=CACHE_SAMPLES; j++) {
                            hist_map_workset[21].at(j)++;
                        }
                    }
                    map_lock->unlock();
                }
                else{
                    map_lock->lock();
                    hist_map_workset[key+12].at(0)++;
                    if (workset_level > 0) {
                        for (int j=workset_level; j<=CACHE_SAMPLES; j++) {
                            hist_map_workset[key+12].at(j)++;
                        }
                    }
                    map_lock->unlock();
                }
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

        for (int i=0; i<16; i++) {
            file << "host " << hostID << " number of mirrors touched by " << i << " worksets: " << hist_map_workset[i].at(0) << std::endl;
            for (int j=1; j<=CACHE_SAMPLES; j++) {
                file << "host " << hostID << " bin " << i << " worksets within top " << (CACHE_BOUND/CACHE_SAMPLES)*j << " percent: " << hist_map_workset[i].at(j) << std::endl;
            }
        }
        
        for (int i=16; i<22; i++) {
            file << "host " << hostID << " number of mirrors touched within bin " << i << " worksets: " << hist_map_workset[i].at(0) << std::endl;
            for (int j=1; j<=CACHE_SAMPLES; j++) {
                file << "host " << hostID << " bin " << i << " worksets within top " << (CACHE_BOUND/CACHE_SAMPLES)*j << " percent: " << hist_map_workset[i].at(j) << std::endl;
            }
        }

        workset_clear();
    }

};
