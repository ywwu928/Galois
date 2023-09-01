#include <unordered_map>
#include <vector>
#include <random>

#include "bloom_filter.hpp"

static cll::opt<std::string> graphName("graphName", cll::desc("Name of the input graph"), cll::init("temp"));
static cll::list<int> cacheSize("cacheSize", cll::desc("Size of the cache (in percentage of mirrors)"), cll::OneOrMore);
// static cll::opt<int> windowSize("windowSize", cll::desc("Size of the sliding window (in multiples of cache size)"), cll::init(20));
static cll::opt<int> threshold("threshold", cll::desc("Threshold to get an entry in the cache"), cll::init(10));
  
// std::random_device rd;  // a seed source for the random number engine
// std::mt19937 gen(rd()); // mersenne_twister_engine seeded with rd()
std::mt19937 gen(0); // mersenne_twister_engine seeded with rd()

template <typename Graph>
struct Instrument {
  Graph* graph;
  uint64_t hostID;
  uint64_t numHosts;
  
  int numSize;
  uint64_t mirrorSize;
  
  std::shared_ptr<galois::substrate::SimpleLock> map_lock;
  std::shared_ptr<galois::substrate::SimpleLock> bloom_lock;

  std::vector<uint64_t> entrySize;
  std::vector<std::unordered_map<uint64_t, bool>> map_vector;
  
  // std::unique_ptr<uint64_t[]> entrySize;
  // std::unique_ptr<std::unordered_map<uint64_t, bool>[]> map_vector;
  
  std::unique_ptr<bloom_filter[]> bloom_counter;
  
  std::vector<std::pair<uint64_t, bool>> victim_sample;

  std::unique_ptr<galois::DGAccumulator<uint64_t>> local_read_stream;
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

  void init(uint64_t hid, uint64_t numH, std::unique_ptr<Graph>& g) {
    /**
     * Counts cache level for each mirror node.
     */
    graph    = g.get();
    hostID   = hid;
    numHosts = numH;

    numSize = cacheSize.size();
    mirrorSize = graph->numMirrors();
    
    map_lock = std::make_shared<galois::substrate::SimpleLock>();
    bloom_lock = std::make_shared<galois::substrate::SimpleLock>();

    entrySize.resize(numSize);
    map_vector.resize(numSize);
    // linked_list_vector.resize(numSize);
    
    // entrySize = std::make_unique<uint64_t[]>(numSize);
    // map_vector = std::make_unique<std::unordered_map<uint64_t, bool>[]>(numSize);
    
    bloom_counter = std::make_unique<bloom_filter[]>(numSize);
    
    victim_sample.reserve(10);

    local_read_stream = std::make_unique<galois::DGAccumulator<uint64_t>>();
    master_read       = std::make_unique<galois::DGAccumulator<uint64_t>>();
    master_write      = std::make_unique<galois::DGAccumulator<uint64_t>>();
    mirror_read =
        std::make_unique<galois::DGAccumulator<uint64_t>[]>(numSize);
    mirror_write =
        std::make_unique<galois::DGAccumulator<uint64_t>[]>(numSize);
    remote_read =
        std::make_unique<galois::DGAccumulator<uint64_t>[]>(numSize);
    remote_write =
        std::make_unique<galois::DGAccumulator<uint64_t>[]>(numSize);
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
          std::make_unique<galois::DGAccumulator<uint64_t>[]>(numSize);
      remote_write_to_host[i] =
          std::make_unique<galois::DGAccumulator<uint64_t>[]>(numSize);
      remote_comm_to_host[i] =
          std::make_unique<galois::DGAccumulator<uint64_t>[]>(numSize);
    }

    for (auto i = 0; i < numSize; i++) {
        entrySize[i] = mirrorSize * cacheSize[i] / 100;

        map_vector[i].reserve(entrySize[i]);
        // linked_list_vector[i].setMax(entrySize[i]);

        // std::unordered_map<uint64_t, bool> map_temp;
        // map_temp.reserve(entrySize[i]);
        // map_vector.push_back(map_temp);

        bloom_parameters parameters;
        parameters.projected_element_count = mirrorSize;
        parameters.false_positive_probability = 0.01;
        parameters.threshold = threshold;
        if (!parameters) {
            std::cout << "Error: invalid set of bloom filter parameters!" << std::endl;
            return;
        }
        parameters.compute_optimal_parameters();
        bloom_counter[i].set_parameters(parameters);
    }
    counter_clear();

    // start instrumentation
    file.open(graphName + "_" + std::to_string(numH) + "procs_id" + std::to_string(hid));
    file << "#####   Stat   #####" << std::endl;
    file << "host " << hid << " total mirror nodes: " << graph->numMirrors()
         << std::endl;
  }

  void counter_clear() {
    local_read_stream->reset();
    master_read->reset();
    master_write->reset();
    for (auto i = 0; i < numSize; i++) {
      mirror_read[i].reset();
      mirror_write[i].reset();
      remote_read[i].reset();
      remote_write[i].reset();
    }
    for (auto i = 0ul; i < numHosts; i++) {
      for (auto j = 0; j < numSize; j++) {
        remote_read_to_host[i][j].reset();
        remote_write_to_host[i][j].reset();
        remote_comm_to_host[i][j].reset();
      }
    }
  }
  
  void bloom_clear() {
      for (auto i = 0; i < numSize; i++) {
        bloom_counter[i].clear();
      }
  }
  
  void bloom_decrement() {
      for (auto i = 0; i < numSize; i++) {
        bloom_counter[i].decrement();
      }
  }
  
  void cache_clear() {
      // std::cout << "Host " << hostID << " Cache Clear Breakpoint 1!" << std::endl;
      // map_vector[0].clear();
      for (auto i = 0; i < numSize; i++) {
        map_lock->lock();
        map_vector[i].clear();
        map_lock->unlock();
      }
      // std::cout << "Host " << hostID << " Cache Clear Breakpoint 2!" << std::endl;
  }

  void clear() {
      // std::cout << "Host " << hostID << " Clear Start!" << std::endl;
      counter_clear();
      // std::cout << "Host " << hostID << " Counter Cleared!" << std::endl;
      bloom_clear();
      // std::cout << "Host " << hostID << " Bloom Cleared!" << std::endl;
      cache_clear();
      // std::cout << "Host " << hostID << " Cache Cleared!" << std::endl;
      
      // for (auto i = 0; i < numSize; i++) {
      //   map_vector[i].reserve(entrySize[i]);
      // }
  }

/*
  void bloom_clear(int index) {
      bloom_counter[index].clear();
  }

  void increment_access() {
      for (auto i = 0; i < numSize; i++) {
          access_count[i] += 1;

          auto limit = windowSize * entrySize[i];
          if (access_count[i] > limit) {
              bloom_clear(i);
              access_count[i] = 0;
          }
      }
  }
*/
  void record_local_read_stream() {
      // std::cout << "Called!" << std::endl;
      *local_read_stream += 1;
      // increment_access();
  }

  void record_read_random(typename Graph::GraphNode node) {
    auto gid = graph->getGID(node);

    if (graph->isOwned(gid)) { // master
      *master_read += 1;
    }
    else { // mirror
        for (auto i = 0; i < numSize; i++) {
            auto it = map_vector[i].find(gid);

            if (it == map_vector[i].end()) { // not found in cache
                remote_read[i] += 1;
                remote_read_to_host[graph->getHostID(gid)][i] += 1;
                
                bloom_lock->lock();
                bool exceed = bloom_counter[i].insert(gid);
                bloom_lock->unlock();
                
                if (map_vector[i].size() < entrySize[i]) { // there is empty entry in cache
                    map_lock->lock();
                    map_vector[i].insert({gid, true});
                    map_lock->unlock();
                }
                else {
                    if (exceed) { // cache replacement
                        map_lock->lock();
                        std::sample(map_vector[i].begin(), map_vector[i].end(), std::back_inserter(victim_sample), 10, gen);
                        auto victim_pair = std::min_element(victim_sample.begin(), victim_sample.end());
                        auto victim_gid = victim_pair->first;
                        // std::cout << "Victim global id is " << victim_gid << std::endl;
                        map_vector[i].erase(victim_gid);
                        map_vector[i].insert({gid, true});
                        victim_sample.clear();
                        map_lock->unlock();
                    }
                }
            }
            else { // found in cache
                // std::cout << "Cache Hit!" << std::endl;
                mirror_read[i] += 1;
            }
            // bloom_counter[i].insert(gid);
            // mirror_read[i] += 1;
        }
    }

    // increment_access();
  }

  void record_write_random(typename Graph::GraphNode node, bool comm) {
    auto gid = graph->getGID(node);

    if (graph->isOwned(gid)) { // master
      *master_write += 1;
    }
    else { // mirror
        for (auto i = 0; i < numSize; i++) {
            auto it = map_vector[i].find(gid);

            if (it == map_vector[i].end()) { // not found in cache
                remote_write[i] += 1;
                remote_write_to_host[graph->getHostID(gid)][i] += 1;
                
                bloom_lock->lock();
                bool exceed = bloom_counter[i].insert(gid);
                bloom_lock->unlock();
                
                if (map_vector[i].size() < entrySize[i]) { // there is empty entry in cache
                    map_lock->lock();
                    map_vector[i].insert({gid, true});
                    map_lock->unlock();
                }
                else {
                    if (exceed) { // cache replacement
                        map_lock->lock();
                        std::sample(map_vector[i].begin(), map_vector[i].end(), std::back_inserter(victim_sample), 10, gen);
                        auto victim_pair = std::min_element(victim_sample.begin(), victim_sample.end());
                        auto victim_gid = victim_pair->first;
                        // std::cout << "Victim global id is " << victim_gid << std::endl;
                        map_vector[i].erase(victim_gid);
                        map_vector[i].insert({gid, true});
                        victim_sample.clear();
                        map_lock->unlock();
                    }
                }
            }
            else { // found in cache
                // std::cout << "Cache Hit!" << std::endl;
                mirror_write[i] += 1;
                
                if (comm) {
                    remote_comm_to_host[graph->getHostID(gid)][i] += 1;
                }
            }
        }
    }

    // increment_access();
  }

  void log_run(uint64_t run) {
    file << "#####   Run " << run << "   #####" << std::endl;
  }

  void log_round(uint64_t num_iterations) {
    file << "#####   Round " << num_iterations << "   #####" << std::endl;
    file << "host " << hostID
         << " local read (stream): " << local_read_stream->read_local()
         << std::endl;
    file << "host " << hostID << " master reads: " << master_read->read_local()
         << std::endl;
    file << "host " << hostID
         << " master writes: " << master_write->read_local() << std::endl;

    for (int i = 0; i < numSize; i++) {
      file << "host " << hostID << " cache " << cacheSize[i]
           << " mirror reads: " << mirror_read[i].read_local() << std::endl;
      file << "host " << hostID << " cache " << cacheSize[i]
           << " mirror writes: " << mirror_write[i].read_local() << std::endl;
      file << "host " << hostID << " cache " << cacheSize[i]
           << " remote reads: " << remote_read[i].read_local() << std::endl;
      file << "host " << hostID << " cache " << cacheSize[i]
           << " remote writes: " << remote_write[i].read_local() << std::endl;

      for (uint32_t j = 0; j < numHosts; j++) {
        file << "host " << hostID << " cache " << cacheSize[i] << " remote read to host "
             << j << ": " << remote_read_to_host[j][i].read_local()
             << std::endl;
        file << "host " << hostID << " cache " << cacheSize[i] << " remote write to host "
             << j << ": " << remote_write_to_host[j][i].read_local()
             << std::endl;
        file << "host " << hostID << " cache " << cacheSize[i]
             << " dirty mirrors for host " << j << ": "
             << remote_comm_to_host[j][i].read_local() << std::endl;
      }
    }
  }
};
