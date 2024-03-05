#include <map>

static cll::opt<std::string> graphName("graphName", cll::desc("Name of the input graph"), cll::init("temp"));

constexpr int CACHE_BOUND = 50;
constexpr int CACHE_SAMPLES = 10;
constexpr int CACHE_STEP = CACHE_BOUND / CACHE_SAMPLES;

bool sortAccess (const std::vector<uint64_t>& v1, const std::vector<uint64_t>& v2) { // descending order
    return v1[3] > v2[3]; // index 3 is # of accesses
}

template <typename Graph>
struct Instrument {
#if GALOIS_INSTRUMENT
  Graph* graph;
  uint64_t hostID;
  uint64_t numHosts;

  // size of total mirror nodes; read / write accesses for each mirror
  std::vector<std::vector<uint64_t>> node_access;
  std::shared_ptr<galois::substrate::SimpleLock> vector_lock;

  std::unique_ptr<galois::DGAccumulator<uint64_t>> local_read_stream;
  std::unique_ptr<galois::DGAccumulator<uint64_t>> local_write_stream;
  std::unique_ptr<galois::DGAccumulator<uint64_t>> master_read;
  std::unique_ptr<galois::DGAccumulator<uint64_t>> master_write;
  std::unique_ptr<galois::DGAccumulator<uint64_t>[]> mirror_read;
  std::unique_ptr<galois::DGAccumulator<uint64_t>[]> mirror_write;
  std::unique_ptr<galois::DGAccumulator<uint64_t>[]> remote_read;
  std::unique_ptr<galois::DGAccumulator<uint64_t>[]> remote_write;
  std::unique_ptr<std::unique_ptr<galois::DGAccumulator<uint64_t>[]>[]>
      remote_read_to_host;
  std::unique_ptr<std::unique_ptr<galois::DGAccumulator<uint64_t>[]>[]>
      remote_write_to_host;
  std::unique_ptr<std::unique_ptr<galois::DGAccumulator<uint64_t>[]>[]>
      remote_comm_to_host;
  std::ofstream file;
#endif

  void init(uint64_t hid, uint64_t numH, std::unique_ptr<Graph>& g) {
#if GALOIS_INSTRUMENT
    /**
     * Counts cache level for each mirror node.
     */
    graph    = g.get();
    hostID   = hid;
    numHosts = numH;
  
    vector_lock = std::make_shared<galois::substrate::SimpleLock>();

    local_read_stream = std::make_unique<galois::DGAccumulator<uint64_t>>();
    local_write_stream = std::make_unique<galois::DGAccumulator<uint64_t>>();
    master_read       = std::make_unique<galois::DGAccumulator<uint64_t>>();
    master_write      = std::make_unique<galois::DGAccumulator<uint64_t>>();
    mirror_read =
        std::make_unique<galois::DGAccumulator<uint64_t>[]>(CACHE_SAMPLES + 1);
    mirror_write =
        std::make_unique<galois::DGAccumulator<uint64_t>[]>(CACHE_SAMPLES + 1);
    remote_read =
        std::make_unique<galois::DGAccumulator<uint64_t>[]>(CACHE_SAMPLES + 1);
    remote_write =
        std::make_unique<galois::DGAccumulator<uint64_t>[]>(CACHE_SAMPLES + 1);
    remote_read_to_host =
        std::make_unique<std::unique_ptr<galois::DGAccumulator<uint64_t>[]>[]>(
            numH);
    remote_write_to_host =
        std::make_unique<std::unique_ptr<galois::DGAccumulator<uint64_t>[]>[]>(
            numH);
    remote_comm_to_host =
        std::make_unique<std::unique_ptr<galois::DGAccumulator<uint64_t>[]>[]>(
            numH);
    for (uint32_t i = 0; i < numH; i++) {
      remote_read_to_host[i] =
          std::make_unique<galois::DGAccumulator<uint64_t>[]>(CACHE_SAMPLES + 1);
      remote_write_to_host[i] =
          std::make_unique<galois::DGAccumulator<uint64_t>[]>(CACHE_SAMPLES + 1);
      remote_comm_to_host[i] =
          std::make_unique<galois::DGAccumulator<uint64_t>[]>(CACHE_SAMPLES + 1);
    }
    clear();
    
    // start instrumentation
    file.open(graphName + "_" + std::to_string(numH) + "procs_id" + std::to_string(hid), std::ios::out | std::ios::trunc);
    file << "#####   Stat   #####" << std::endl;
    file << "host " << hid << " total mirrors: " << graph->numMirrors() << std::endl;
    file << "host " << hid << " total edges: " << graph->sizeEdges() << std::endl;
#else
    (void) hid;
    (void) numH;
    (void) g;
#endif
  }

  void clear() {
#if GALOIS_INSTRUMENT
    local_read_stream->reset();
    local_write_stream->reset();
    master_read->reset();
    master_write->reset();
    for (auto i = 0ul; i < CACHE_SAMPLES + 1; i++) {
      mirror_read[i].reset();
      mirror_write[i].reset();
      remote_read[i].reset();
      remote_write[i].reset();
    }
    for (auto i = 0ul; i < numHosts; i++) {
      for (auto j = 0ul; j < CACHE_SAMPLES + 1; j++) {
        remote_read_to_host[i][j].reset();
        remote_write_to_host[i][j].reset();
        remote_comm_to_host[i][j].reset();
      }
    }
#endif
  }

  void record_local_read_stream() {
#if GALOIS_INSTRUMENT
      *local_read_stream += 1;
#endif
  }
  
  void record_local_write_stream() {
#if GALOIS_INSTRUMENT
      *local_write_stream += 1;
#endif
  }

  void record_read_random(typename Graph::GraphNode node) {
#if GALOIS_INSTRUMENT
    auto gid = graph->getGID(node);
    if (graph->isOwned(gid)) { // master
      *master_read += 1;
      return;
    }
    
    auto& nodeData = graph->getData(node);
    nodeData.read.fetch_add(1, std::memory_order_relaxed);
#else
    (void) node;
#endif
  }

  void record_write_random(typename Graph::GraphNode node) {
#if GALOIS_INSTRUMENT
    auto gid = graph->getGID(node);
    if (graph->isOwned(gid)) { // master
      *master_write += 1;
      return;
    }
    
    auto& nodeData = graph->getData(node);
    nodeData.write.fetch_add(1, std::memory_order_relaxed);
#else
    (void) node;
#endif
  }

  void update() {
#if GALOIS_INSTRUMENT
      form_vector();
      count_access();
#endif
  }

  void form_vector () {
#if GALOIS_INSTRUMENT
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
#endif
  }

  void count_access() {
#if GALOIS_INSTRUMENT
      // sort the access vector first
      std::sort(node_access.begin(), node_access.end(), sortAccess);

      // count # of reads and writes
      int vector_size = node_access.size();
      int vector_bound = vector_size * CACHE_BOUND / 100;
      unsigned chunk_size = vector_bound / CACHE_SAMPLES;
      unsigned remainder = vector_bound % CACHE_SAMPLES;
	
      for (unsigned i=0; i<CACHE_SAMPLES; i++) {
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

              for (unsigned k=i+1; k<CACHE_SAMPLES+1; k++) {

                  mirror_read[k] += read;
                  mirror_write[k] += write;
/*                  
                  if (read > 0) {
                      mirror_read[k] += (read-1);
                      remote_read[k] += 1;

                      mirror_write[k] += write;
                  } else {
                      if (write > 0) {
                          mirror_write[k] += (write-1);
                          remote_write[k] += 1;
                      }
                  }
*/                
                  if (write > 0) {
                      remote_comm_to_host[owner_host][k] += 1;
                  }
              }
		  }
	  }

      for (unsigned i=0; i<remainder; i++) {
          int index = CACHE_SAMPLES * chunk_size + i;
          uint64_t owner_host = node_access[index][0];
          uint64_t read = node_access[index][1];
          uint64_t write = node_access[index][2];
            
          for (unsigned k=0; k<CACHE_SAMPLES; k++) {
              remote_read[k] += read;
              remote_read_to_host[owner_host][k] += read;

              remote_write[k] += write;
              remote_write_to_host[owner_host][k] += write;
          }

          mirror_read[CACHE_SAMPLES] += read;
          mirror_write[CACHE_SAMPLES] += write;
/*
          if (read > 0) {
              mirror_read[CACHE_SAMPLES] += (read-1);
              remote_read[CACHE_SAMPLES] += 1;

              mirror_write[CACHE_SAMPLES] += write;
          } else {
              if (write > 0) {
                  mirror_write[CACHE_SAMPLES] += (write-1);
                  remote_write[CACHE_SAMPLES] += 1;
              }
          }
*/        
          if (write > 0) {
              remote_comm_to_host[owner_host][CACHE_SAMPLES] += 1;
          }
      }
      
      for (auto index=vector_bound; index<vector_size; index++) {
          uint64_t owner_host = node_access[index][0];
          uint64_t read = node_access[index][1];
          uint64_t write = node_access[index][2];
          
          for (unsigned k=0; k<CACHE_SAMPLES+1; k++) {
              remote_read[k] += read;
              remote_read_to_host[owner_host][k] += read;

              remote_write[k] += write;
              remote_write_to_host[owner_host][k] += write;
          }
      }

      node_access.clear();
#endif
  }

  void log_run(uint64_t run) {
#if GALOIS_INSTRUMENT
    file << "#####   Run " << run << "   #####" << std::endl;
#else
    (void) run;
#endif
  }

  void log_round(uint64_t num_iterations) {
#if GALOIS_INSTRUMENT
    auto host_id   = hostID;
    auto num_hosts = numHosts;
    file << "#####   Round " << num_iterations << "   #####" << std::endl;
    file << "host " << host_id
         << " local read (stream): " << local_read_stream->read_local()
         << std::endl;
    file << "host " << host_id
         << " local write (stream): " << local_write_stream->read_local()
         << std::endl;
    file << "host " << host_id << " master reads: " << master_read->read_local()
         << std::endl;
    file << "host " << host_id
         << " master writes: " << master_write->read_local() << std::endl;

    for (int i = 0; i < CACHE_SAMPLES+1; i++) {
      file << "host " << host_id << " cache " << CACHE_STEP*i
           << " mirror reads: " << mirror_read[i].read_local() << std::endl;
      file << "host " << host_id << " cache " << CACHE_STEP*i
           << " mirror writes: " << mirror_write[i].read_local() << std::endl;
      file << "host " << host_id << " cache " << CACHE_STEP*i
           << " remote reads: " << remote_read[i].read_local() << std::endl;
      file << "host " << host_id << " cache " << CACHE_STEP*i
           << " remote writes: " << remote_write[i].read_local() << std::endl;

      for (uint32_t j = 0; j < num_hosts; j++) {
        file << "host " << host_id << " cache " << CACHE_STEP*i << " remote read to host "
             << j << ": " << remote_read_to_host[j][i].read_local()
             << std::endl;
        file << "host " << host_id << " cache " << CACHE_STEP*i << " remote write to host "
             << j << ": " << remote_write_to_host[j][i].read_local()
             << std::endl;
        file << "host " << host_id << " cache " << CACHE_STEP*i
             << " dirty mirrors for host " << j << ": "
             << remote_comm_to_host[j][i].read_local() << std::endl;
      }
    }

    file.flush();
#else
    (void) num_iterations;
#endif
  }
};
