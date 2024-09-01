#include <map>

#include "cache.cpp"

static cll::opt<std::string> graphName("graphName", cll::desc("Name of the input graph"), cll::init("temp"));

constexpr int CACHE_BOUND = 50;
constexpr int CACHE_SAMPLES = 10;
constexpr int CACHE_STEP = CACHE_BOUND / CACHE_SAMPLES;

template <typename Graph>
struct Instrument {
#if GALOIS_INSTRUMENT
  Graph* graph;
  uint64_t hostID;
  uint64_t numHosts;

  // lru cache for different capacities of cache
  std::unique_ptr<LRUCache<uint64_t>[]> lru_cache;
  std::vector<std::shared_ptr<galois::substrate::SimpleLock>> cache_lock;

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

    // set up final result container
    lru_cache =
        std::make_unique<LRUCache<uint64_t>[]>(CACHE_SAMPLES);
    cache_lock.reserve(CACHE_SAMPLES);
    for (auto i=0; i<CACHE_SAMPLES; i++) {
        cache_lock.push_back(std::make_shared<galois::substrate::SimpleLock>());
    }

    local_read_stream = std::make_unique<galois::DGAccumulator<uint64_t>>();
    local_write_stream = std::make_unique<galois::DGAccumulator<uint64_t>>();
    master_read       = std::make_unique<galois::DGAccumulator<uint64_t>>();
    master_write      = std::make_unique<galois::DGAccumulator<uint64_t>>();
    mirror_read =
        std::make_unique<galois::DGAccumulator<uint64_t>[]>(CACHE_SAMPLES);
    mirror_write =
        std::make_unique<galois::DGAccumulator<uint64_t>[]>(CACHE_SAMPLES);
    remote_read =
        std::make_unique<galois::DGAccumulator<uint64_t>[]>(CACHE_SAMPLES);
    remote_write =
        std::make_unique<galois::DGAccumulator<uint64_t>[]>(CACHE_SAMPLES);
    remote_read_to_host =
        std::make_unique<std::unique_ptr<galois::DGAccumulator<uint64_t>[]>[]>(
            numH);
    remote_write_to_host =
        std::make_unique<std::unique_ptr<galois::DGAccumulator<uint64_t>[]>[]>(
            numH);
    remote_comm_to_host =
        std::make_unique<std::unique_ptr<galois::DGAccumulator<uint64_t>[]>[]>(
            numH);
    for (uint32_t i=0; i<numH; i++) {
      remote_read_to_host[i] =
          std::make_unique<galois::DGAccumulator<uint64_t>[]>(CACHE_SAMPLES);
      remote_write_to_host[i] =
          std::make_unique<galois::DGAccumulator<uint64_t>[]>(CACHE_SAMPLES);
      remote_comm_to_host[i] =
          std::make_unique<galois::DGAccumulator<uint64_t>[]>(CACHE_SAMPLES);
    }
    
    auto mirror_num = graph->numMirrors();
    for (auto i=0; i<CACHE_SAMPLES; i++) {
        int cache_size = mirror_num * CACHE_STEP * (i+1) / 100;
        lru_cache[i] = LRUCache<uint64_t>(cache_size);
    }

    local_read_stream->reset();
    local_write_stream->reset();
    master_read->reset();
    master_write->reset();
    for (auto i=0ul; i<CACHE_SAMPLES; i++) {
        mirror_read[i].reset();
        mirror_write[i].reset();
        remote_read[i].reset();
        remote_write[i].reset();
    }
    for (auto i=0ul; i<numHosts; i++) {
        for (auto j=0ul; j<CACHE_SAMPLES; j++) {
            remote_read_to_host[i][j].reset();
            remote_write_to_host[i][j].reset();
            remote_comm_to_host[i][j].reset();
        }
    }

    // start instrumentation
    file.open(graphName + "_" + std::to_string(numH) + "procs_id" + std::to_string(hid), std::ios::out | std::ios::trunc);
    file << "#####   Stat   #####" << std::endl;
    file << "host " << hid << " total mirrors: " << mirror_num << std::endl;
    file << "host " << hid << " total edges: " << graph->sizeEdges() << std::endl;
#else
    (void) hid;
    (void) numH;
    (void) g;
#endif
  }

  void clear() {
#if GALOIS_INSTRUMENT
    for (auto i=0; i<CACHE_SAMPLES; i++) {
        cache_lock[i]->lock();
        lru_cache[i].clear();
        cache_lock[i]->unlock();
    }

    local_read_stream->reset();
    local_write_stream->reset();
    master_read->reset();
    master_write->reset();
    for (auto i=0ul; i<CACHE_SAMPLES; i++) {
        mirror_read[i].reset();
        mirror_write[i].reset();
        remote_read[i].reset();
        remote_write[i].reset();
    }
    for (auto i=0ul; i<numHosts; i++) {
        for (auto j=0ul; j<CACHE_SAMPLES; j++) {
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

    // mirror
    for (int i=0; i<CACHE_SAMPLES; i++) {
        cache_lock[i]->lock();
        access_info<uint64_t> info = lru_cache[i].read(gid);
        cache_lock[i]->unlock();
        
        if (!info.present) { // not in the cache
            if (info.has_victim) { // victim is evicted
                // writeback ro remote
                remote_write[i] += 1;
                remote_write_to_host[graph->getHostID(info.victim)][i] += 1;
            }

            // bring into the cache from remote
            remote_read[i] += 1;
            remote_read_to_host[graph->getHostID(gid)][i] += 1;
        }
        else {
            mirror_read[i] += 1;
        }
    }
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

    // mirror
    for (int i=0; i<CACHE_SAMPLES; i++) {
        cache_lock[i]->lock();
        access_info<uint64_t> info = lru_cache[i].write(gid);
        cache_lock[i]->unlock();
        
        if (!info.present) { // not in the cache
            if (info.has_victim) { // victim is evicted
                // writeback ro remote
                remote_write[i] += 1;
                remote_write_to_host[graph->getHostID(info.victim)][i] += 1;
            }

            // bring into the cache from remote
            remote_read[i] += 1;
            remote_read_to_host[graph->getHostID(gid)][i] += 1;
        }
        else {
            mirror_write[i] += 1;
        }
    }
#else
    (void) node;
#endif
  }

  void scan_cache() {
#if GALOIS_INSTRUMENT
    for (auto i=0; i<CACHE_SAMPLES; i++) {
        cache_lock[i]->lock();
        for (auto it=lru_cache[i].get_lru_begin(); it!=lru_cache[i].get_lru_end(); it++) {
            if (it->dirty) {
                remote_comm_to_host[graph->getHostID(it->tag)][i] += 1;
            }
        }
        cache_lock[i]->unlock();
    }
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

    for (int i=0; i<CACHE_SAMPLES; i++) {
      file << "host " << host_id << " cache " << CACHE_STEP * (i + 1)
           << " mirror reads: " << mirror_read[i].read_local() << std::endl;
      file << "host " << host_id << " cache " << CACHE_STEP * (i + 1)
           << " mirror writes: " << mirror_write[i].read_local() << std::endl;
      file << "host " << host_id << " cache " << CACHE_STEP * (i + 1)
           << " remote reads: " << remote_read[i].read_local() << std::endl;
      file << "host " << host_id << " cache " << CACHE_STEP * (i + 1)
           << " remote writes: " << remote_write[i].read_local() << std::endl;

      for (uint32_t j=0; j<num_hosts; j++) {
        file << "host " << host_id << " cache " << CACHE_STEP * (i + 1) << " remote read to host "
             << j << ": " << remote_read_to_host[j][i].read_local()
             << std::endl;
        file << "host " << host_id << " cache " << CACHE_STEP * (i + 1) << " remote write to host "
             << j << ": " << remote_write_to_host[j][i].read_local()
             << std::endl;
        file << "host " << host_id << " cache " << CACHE_STEP * (i + 1)
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
