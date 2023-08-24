#include <map>

static cll::opt<std::string> graphName("graphName", cll::desc("Name of the input graph"), cll::init("temp"));
static cll::opt<int> cacheSize("cacheSize", cll::desc("Size of the cache (in percentage of mirrors)"), cll::init(50));
static cll::opt<int> windowSize("windowSize", cll::desc("Size of the sliding window (in multiples of cache size)"), cll::init(10));
static cll::opt<int> decrementSize("decrementSize", cll::desc("Number of bits to truncate after each window"), cll::init(2));

bool sortAccess (const std::vector<uint64_t>& v1, const std::vector<uint64_t>& v2) { // descending order
    return v1[3] > v2[3]; // index 3 is # of accesses
}

template <typename Graph>
struct Instrument {
  Graph* graph;
  uint64_t hostID;
  uint64_t numHosts;

  // size of total mirror nodes; read / write accesses for each mirror
  std::vector<std::vector<uint64_t>> cache;
  std::shared_ptr<galois::substrate::SimpleLock> vector_lock;

  std::unique_ptr<galois::DGAccumulator<uint64_t>> local_read_stream;
  std::unique_ptr<galois::DGAccumulator<uint64_t>> master_read;
  std::unique_ptr<galois::DGAccumulator<uint64_t>> master_write;
  std::unique_ptr<galois::DGAccumulator<uint64_t>> mirror_read;
  std::unique_ptr<galois::DGAccumulator<uint64_t>> mirror_write;
  std::unique_ptr<galois::DGAccumulator<uint64_t>> remote_read;
  std::unique_ptr<galois::DGAccumulator<uint64_t>> remote_write;
  std::unique_ptr<std::unique_ptr<galois::DGAccumulator<uint64_t>[]>>
      remote_read_to_host;
  std::unique_ptr<std::unique_ptr<galois::DGAccumulator<uint64_t>[]>>
      remote_write_to_host;
  std::unique_ptr<std::unique_ptr<galois::DGAccumulator<uint64_t>[]>>
      remote_comm_to_host;
  std::ofstream file;

  void init(uint64_t hid, uint64_t numH, std::unique_ptr<Graph>& g) {
    /**
     * Counts cache level for each mirror node.
     */
    graph    = g.get();
    hostID   = hid;
    numHosts = numH;
  
    vector_lock = std::make_shared<galois::substrate::SimpleLock>();

    local_read_stream = std::make_unique<galois::DGAccumulator<uint64_t>>();
    master_read       = std::make_unique<galois::DGAccumulator<uint64_t>>();
    master_write      = std::make_unique<galois::DGAccumulator<uint64_t>>();
    mirror_read       = std::make_unique<galois::DGAccumulator<uint64_t>>();
    mirror_write      = std::make_unique<galois::DGAccumulator<uint64_t>>();
    remote_read       = std::make_unique<galois::DGAccumulator<uint64_t>>();
    remote_write      = std::make_unique<galois::DGAccumulator<uint64_t>>();
    remote_read_to_host =
        std::make_unique<galois::DGAccumulator<uint64_t>[]>(numH);
    remote_write_to_host =
        std::make_unique<galois::DGAccumulator<uint64_t>[]>(numH);
    remote_comm_to_host =
        std::make_unique<galois::DGAccumulator<uint64_t>[]>(numH);
    clear();
      
    auto mirrorSize = graph->numMirrors();
    cache.reserve(mirrorSize * cacheSize / 100);
    
    // start instrumentation
    file.open(graphName + "_" + std::to_string(numH) + "procs_cache" + std::to_string(cacheSize) + "_id" + std::to_string(hid));
    file << "#####   Stat   #####" << std::endl;
    file << "host " << hid << " total number of mirrors: " << graph->numMirrors() << std::endl;
  }

  void clear() {
    local_read_stream->reset();
    master_read->reset();
    master_write->reset();
    mirror_read->reset();
    mirror_write->reset();
    remote_read->reset();
    remote_write->reset();
    for (auto i = 0ul; i < numHosts; i++) {
        remote_read_to_host[i].reset();
        remote_write_to_host[i].reset();
        remote_comm_to_host[i].reset();
    }
  }

  void record_local_read_stream() { *local_read_stream += 1; }

  void record_read_random(typename Graph::GraphNode node) {
    auto gid = graph->getGID(node);
    if (graph->isOwned(gid)) { // master
      *master_read += 1;
      return;
    }
    
    auto& nodeData = graph->getData(node);
    nodeData.read.fetch_add(1, std::memory_order_relaxed);
  }

  void record_write_random(typename Graph::GraphNode node) {
    auto gid = graph->getGID(node);
    if (graph->isOwned(gid)) { // master
      *master_write += 1;
      return;
    }
    
    auto& nodeData = graph->getData(node);
    nodeData.write.fetch_add(1, std::memory_order_relaxed);
  }

  void update() {
      form_vector();
      count_access();
  }

  void form_vector () {
      auto mirrorSize = graph->numMirrors();
      node_access.reserve(mirrorSize);

      const auto& allMirrorNodes = graph->mirrorNodesRange();
      galois::do_all(
          galois::iterate(allMirrorNodes),
          [&](auto node) {
              auto node_GID = graph->getGID(node);
              auto& nodeData = graph->getData(node);
              std::vector<uint64_t> temp {graph->getHostID(node_GID), nodeData.read, nodeData.write, nodeData.read+nodeData.write};

              vector_lock->lock();
              node_access.push_back(temp);
              vector_lock->unlock();
          },
          galois::steal(), galois::no_stats());
  }

  void count_access() {
      // sort the access vector first
      std::sort(node_access.begin(), node_access.end(), sortAccess);

      // count # of reads and writes
      int vector_size = node_access.size();
      unsigned chunk_size = vector_size / CACHE_LEVELS;
      unsigned remainder = vector_size % CACHE_LEVELS;
	
      for (unsigned i=0; i<CACHE_LEVELS; i++) {
		  for (unsigned j=0; j<chunk_size; j++) {
			  int index = i * chunk_size + j;
			  uint64_t owner_host = node_access[index][0];
			  uint64_t read = node_access[index][1];
			  uint64_t write = node_access[index][2];

              for (unsigned k=0; k<i+1; k++) {
                  remote_read[k] += read;
                  remote_read_to_host[owner_host][k] += read;

                  remote_write[k] += write;
                  remote_write_to_host[owner_host][k] += write;
              }

              for (unsigned k=i+1; k<CACHE_LEVELS+1; k++) {
                  mirror_read[k] += read;

                  mirror_write[k] += write;
                
                  if (write > 0) {
                      remote_comm_to_host[owner_host][k] += 1;
                  }
              }
		  }
	  }

      for (unsigned i=0; i<remainder; i++) {
          int index = CACHE_LEVELS * chunk_size + i;
          uint64_t owner_host = node_access[index][0];
          uint64_t read = node_access[index][1];
          uint64_t write = node_access[index][2];
            
          for (unsigned k=0; k<CACHE_LEVELS; k++) {
              remote_read[k] += read;
              remote_read_to_host[owner_host][k] += read;

              remote_write[k] += write;
              remote_write_to_host[owner_host][k] += write;
          }

          mirror_read[CACHE_LEVELS] += read;

          mirror_write[CACHE_LEVELS] += write;
        
          if (write > 0) {
              remote_comm_to_host[owner_host][10] += 1;
          }
      }

      node_access.clear();
  }

  void log_run(uint64_t run) {
    file << "#####   Run " << run << "   #####" << std::endl;
  }

  void log_round(uint64_t num_iterations) {
    auto host_id   = hostID;
    auto num_hosts = numHosts;
    file << "#####   Round " << num_iterations << "   #####" << std::endl;
    file << "host " << host_id
         << " local read (stream): " << local_read_stream->read_local()
         << std::endl;
    file << "host " << host_id << " master reads: " << master_read->read_local()
         << std::endl;
    file << "host " << host_id
         << " master writes: " << master_write->read_local() << std::endl;

    for (int i = 0; i < 11; i++) {
      file << "host " << host_id << " cache " << i
           << " mirror reads: " << mirror_read[i].read_local() << std::endl;
      file << "host " << host_id << " cache " << i
           << " mirror writes: " << mirror_write[i].read_local() << std::endl;
      file << "host " << host_id << " cache " << i
           << " remote reads: " << remote_read[i].read_local() << std::endl;
      file << "host " << host_id << " cache " << i
           << " remote writes: " << remote_write[i].read_local() << std::endl;

      for (uint32_t j = 0; j < num_hosts; j++) {
        file << "host " << host_id << " cache " << i << " remote read to host "
             << j << ": " << remote_read_to_host[j][i].read_local()
             << std::endl;
        file << "host " << host_id << " cache " << i << " remote write to host "
             << j << ": " << remote_write_to_host[j][i].read_local()
             << std::endl;
        file << "host " << host_id << " cache " << i
             << " remote communication for host " << j << ": "
             << remote_comm_to_host[j][i].read_local() << std::endl;
      }
    }
  }
};
