/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

/**
 * @file DistributedGraph.h
 *
 * Contains the implementation for DistGraph. Command line argument definitions
 * are found in DistributedGraph.cpp.
 */

#ifndef _GALOIS_DIST_HGRAPH_H_
#define _GALOIS_DIST_HGRAPH_H_

#include <unordered_map>
#include <fstream>

#include "galois/graphs/LC_CSR_Graph.h"
#include "galois/graphs/BufferedGraph.h"
#include "galois/runtime/DistStats.h"
#include "galois/graphs/OfflineGraph.h"
#include "galois/DynamicBitset.h"

/*
 * Headers for boost serialization
 */

namespace galois {
namespace graphs {
/**
 * Enums specifying how masters are to be distributed among hosts.
 */
enum MASTERS_DISTRIBUTION {
  //! balance nodes
  BALANCED_MASTERS,
  //! balance edges
  BALANCED_EDGES_OF_MASTERS,
  //! balance nodes and edges
  BALANCED_MASTERS_AND_EDGES
};

/**
 * Base DistGraph class that all distributed graphs extend from.
 *
 * @tparam NodeTy type of node data for the graph
 * @tparam EdgeTy type of edge data for the graph
 */
template <typename NodeTy, typename EdgeTy>
class DistGraph {
private:
  //! Graph name used for printing things
  constexpr static const char* const GRNAME = "dGraph";

  using GraphTy = galois::graphs::LC_CSR_Graph<NodeTy, EdgeTy, true>;

  // vector for determining range objects for master nodes + nodes
  // with edges (which includes masters)
  //! represents split of all nodes among threads to balance edges
  std::vector<uint32_t> allNodesRanges;
  std::vector<uint32_t> allNodesRangesReserved;
  //! represents split of present nodes (master + mirror) among threads
  std::vector<uint32_t> presentNodesRanges;
  std::vector<uint32_t> presentNodesRangesReserved;
  //! represents split of master nodes among threads
  std::vector<uint32_t> masterRanges;
  std::vector<uint32_t> masterRangesReserved;
  //! represents split of mirror nodes among threads
  std::vector<uint32_t> mirrorRanges;
  std::vector<uint32_t> mirrorRangesReserved;
  //! represents split of ghost nodes among threads
  std::vector<uint32_t> ghostRanges;
  std::vector<uint32_t> ghostRangesReserved;

  using NodeRangeType =
      galois::runtime::SpecificRange<boost::counting_iterator<size_t>>;

  //! Vector of ranges that stores the 10 different range objects that a user is
  //! able to access
  std::vector<NodeRangeType> specificRanges;
  //! Like specificRanges, but for in edges
  std::vector<NodeRangeType> specificRangesIn;

protected:
  //! The internal graph used by DistGraph to represent the graph
  GraphTy graph;

  //! Marks if the graph is transposed or not.
  bool transposed;

  // global graph variables
  uint64_t numGlobalNodes; //!< Total nodes in the global unpartitioned graph.
  uint64_t numGlobalEdges; //!< Total edges in the global unpartitioned graph.
  uint32_t numNodes;       //!< Num nodes in this graph in total
  uint32_t numActualNodes; //!< Num actual existing nodes in this graph in total
  uint32_t numGhostNodes;  //!< Num ghost nodes in this graph in total
  uint64_t numEdges;       //!< Num edges in this graph in total

  const unsigned id;       //!< ID of the machine.
  const uint32_t numHosts; //!< Total number of machines

  // local graph
  // size() = Number of nodes created on this host (masters + mirrors)
  uint32_t numOwned;    //!< Number of nodes owned (masters) by this host.
                        //!< size() - numOwned = mirrors on this host
  uint32_t beginMaster; //!< Local id of the beginning of master nodes.
                        //!< beginMaster + numOwned = local id of the end of
                        //!< master nodes
  uint32_t numNodesWithEdges; //!< Number of nodes (masters + mirrors) that have
                              //!< outgoing edges

  //! Information that converts host to range of nodes that host reads
  std::vector<std::pair<uint64_t, uint64_t>> gid2host;
  //! Mirror nodes from different hosts. For reduce
  std::vector<std::vector<size_t>> mirrorNodes;
  //! ghost nodes from different hosts
  std::vector<std::vector<size_t>> ghostNodes;

  //! GID = localToGlobalVector[LID]
  std::vector<uint64_t> localToGlobalVector;
  //! LID = globalToLocalMap[GID]
  std::unordered_map<uint64_t, uint32_t> globalToLocalMap;
  
  //! Host ID = localHostVector[LID]
  std::vector<uint32_t> localHostVector;

  //! Increments evilPhase, a phase counter used by communication.
  void inline increment_evilPhase() {
    ++galois::runtime::evilPhase;
    if (galois::runtime::evilPhase >=
        static_cast<uint32_t>(
            std::numeric_limits<int16_t>::max())) { // limit defined by MPI or
                                                    // LCI
      galois::runtime::evilPhase = 1;
    }
  }

  //! Returns evilPhase + 1, handling loop around as necessary
  unsigned inline evilPhasePlus1() {
    unsigned result = galois::runtime::evilPhase + 1;

    // limit defined by MPI or LCI
    if (result >= uint32_t{std::numeric_limits<int16_t>::max()}) {
      return 1;
    }
    return result;
  }

  //! used to sort edges in the sort edges function
  template <typename GraphNode, typename ET>
  struct IdLess {
    bool
    operator()(const galois::graphs::EdgeSortValue<GraphNode, ET>& e1,
               const galois::graphs::EdgeSortValue<GraphNode, ET>& e2) const {
      return e1.dst < e2.dst;
    }
  };

private:
  /**
   * Given an OfflineGraph, compute the masters for each node by
   * evenly (or unevenly as specified by scale factor)
   * blocking the nodes off to assign to each host. Considers
   * ONLY nodes and not edges.
   *
   * @param g The offline graph which has loaded the graph you want
   * to get the masters for
   * @param scalefactor A vector that specifies if a particular host
   * should have more or less than other hosts
   * @param DecomposeFactor Specifies how decomposed the blocking
   * of nodes should be. For example, a factor of 2 will make 2 blocks
   * out of 1 block had the decompose factor been set to 1.
   */
  void computeMastersBlockedNodes(galois::graphs::OfflineGraph& g,
                                  const std::vector<unsigned>& scalefactor,
                                  unsigned DecomposeFactor = 1) {
    uint64_t numNodes_to_divide = g.size();
    if (scalefactor.empty() || (numHosts * DecomposeFactor == 1)) {
      for (unsigned i = 0; i < numHosts * DecomposeFactor; ++i)
        gid2host.push_back(galois::block_range(uint64_t{0}, numNodes_to_divide,
                                               i, numHosts * DecomposeFactor));
      return;
    }

    // TODO: not compatible with DecomposeFactor.
    assert(scalefactor.size() == numHosts);

    unsigned numBlocks = 0;

    for (unsigned i = 0; i < numHosts; ++i) {
      numBlocks += scalefactor[i];
    }

    std::vector<std::pair<uint64_t, uint64_t>> blocks;
    for (unsigned i = 0; i < numBlocks; ++i) {
      blocks.push_back(
          galois::block_range(uint64_t{0}, numNodes_to_divide, i, numBlocks));
    }

    std::vector<unsigned> prefixSums;
    prefixSums.push_back(0);

    for (unsigned i = 1; i < numHosts; ++i) {
      prefixSums.push_back(prefixSums[i - 1] + scalefactor[i - 1]);
    }

    for (unsigned i = 0; i < numHosts; ++i) {
      unsigned firstBlock = prefixSums[i];
      unsigned lastBlock  = prefixSums[i] + scalefactor[i] - 1;
      gid2host.push_back(
          std::make_pair(blocks[firstBlock].first, blocks[lastBlock].second));
    }
  }

  /**
   * Given an OfflineGraph, compute the masters for each node by
   * evenly (or unevenly as specified by scale factor)
   * blocking the nodes off to assign to each host while taking
   * into consideration the only edges of the node to get
   * even blocks.
   *
   * @param g The offline graph which has loaded the graph you want
   * to get the masters for
   * @param scalefactor A vector that specifies if a particular host
   * should have more or less than other hosts
   * @param DecomposeFactor Specifies how decomposed the blocking
   * of nodes should be. For example, a factor of 2 will make 2 blocks
   * out of 1 block had the decompose factor been set to 1.
   */
  void computeMastersBalancedEdges(galois::graphs::OfflineGraph& g,
                                   const std::vector<unsigned>& scalefactor,
                                   uint32_t edgeWeight,
                                   unsigned DecomposeFactor = 1) {
    if (edgeWeight == 0) {
      edgeWeight = 1;
    }

    auto& net = galois::runtime::getSystemNetworkInterface();

    gid2host.resize(numHosts * DecomposeFactor);
    for (unsigned d = 0; d < DecomposeFactor; ++d) {
      auto r = g.divideByNode(0, edgeWeight, (id + d * numHosts),
                              numHosts * DecomposeFactor, scalefactor);
      gid2host[id + d * numHosts].first  = *(r.first.first);
      gid2host[id + d * numHosts].second = *(r.first.second);
    }

    for (unsigned h = 0; h < numHosts; ++h) {
      if (h == id) {
        continue;
      }
      galois::runtime::SendBuffer b;
      for (unsigned d = 0; d < DecomposeFactor; ++d) {
        galois::runtime::gSerialize(b, gid2host[id + d * numHosts]);
      }
      net.sendTagged(h, galois::runtime::evilPhase, b);
    }
    net.flush();
    unsigned received = 1;
    while (received < numHosts) {
      decltype(net.receiveTagged(galois::runtime::evilPhase)) p;
      do {
        p = net.receiveTagged(galois::runtime::evilPhase);
      } while (!p);
      assert(p->first != id);
      auto& b = p->second;
      for (unsigned d = 0; d < DecomposeFactor; ++d) {
        galois::runtime::gDeserialize(b, gid2host[p->first + d * numHosts]);
      }
      ++received;
    }
    increment_evilPhase();

#ifndef NDEBUG
    for (unsigned h = 0; h < numHosts; h++) {
      if (h == 0) {
        assert(gid2host[h].first == 0);
      } else if (h == numHosts - 1) {
        assert(gid2host[h].first == gid2host[h - 1].second);
        assert(gid2host[h].second == g.size());
      } else {
        assert(gid2host[h].first == gid2host[h - 1].second);
        assert(gid2host[h].second == gid2host[h + 1].first);
      }
    }
#endif
  }

  /**
   * Given an OfflineGraph, compute the masters for each node by
   * evenly (or unevenly as specified by scale factor)
   * blocking the nodes off to assign to each host while taking
   * into consideration the edges of the node AND the node itself.
   *
   * @param g The offline graph which has loaded the graph you want
   * to get the masters for
   * @param scalefactor A vector that specifies if a particular host
   * should have more or less than other hosts
   * @param DecomposeFactor Specifies how decomposed the blocking
   * of nodes should be. For example, a factor of 2 will make 2 blocks
   * out of 1 block had the decompose factor been set to 1. Ignored
   * in this function currently.
   *
   * @todo make this function work with decompose factor
   */
  void computeMastersBalancedNodesAndEdges(
      galois::graphs::OfflineGraph& g, const std::vector<unsigned>& scalefactor,
      uint32_t nodeWeight, uint32_t edgeWeight, unsigned) {
    if (nodeWeight == 0) {
      nodeWeight = g.sizeEdges() / g.size(); // average degree
    }
    if (edgeWeight == 0) {
      edgeWeight = 1;
    }

    auto& net = galois::runtime::getSystemNetworkInterface();
    gid2host.resize(numHosts);
    auto r = g.divideByNode(nodeWeight, edgeWeight, id, numHosts, scalefactor);
    gid2host[id].first  = *r.first.first;
    gid2host[id].second = *r.first.second;
    for (unsigned h = 0; h < numHosts; ++h) {
      if (h == id)
        continue;
      galois::runtime::SendBuffer b;
      galois::runtime::gSerialize(b, gid2host[id]);
      net.sendTagged(h, galois::runtime::evilPhase, b);
    }
    net.flush();
    unsigned received = 1;
    while (received < numHosts) {
      decltype(net.receiveTagged(galois::runtime::evilPhase)) p;
      do {
        p = net.receiveTagged(galois::runtime::evilPhase);
      } while (!p);
      assert(p->first != id);
      auto& b = p->second;
      galois::runtime::gDeserialize(b, gid2host[p->first]);
      ++received;
    }
    increment_evilPhase();
  }

protected:
  /**
   * Wrapper call that will call into more specific compute masters
   * functions that compute masters based on nodes, edges, or both.
   *
   * @param masters_distribution method of masters distribution to use
   * @param g The offline graph which has loaded the graph you want
   * to get the masters for
   * @param scalefactor A vector that specifies if a particular host
   * should have more or less than other hosts
   * @param nodeWeight weight to give nodes when computing balance
   * @param edgeWeight weight to give edges when computing balance
   * @param DecomposeFactor Specifies how decomposed the blocking
   * of nodes should be. For example, a factor of 2 will make 2 blocks
   * out of 1 block had the decompose factor been set to 1.
   */
  uint64_t computeMasters(MASTERS_DISTRIBUTION masters_distribution,
                          galois::graphs::OfflineGraph& g,
                          const std::vector<unsigned>& scalefactor,
                          uint32_t nodeWeight = 0, uint32_t edgeWeight = 0,
                          unsigned DecomposeFactor = 1) {
    galois::Timer timer;
    timer.start();
    g.reset_seek_counters();

    uint64_t numNodes_to_divide = g.size();

    // compute masters for all nodes
    switch (masters_distribution) {
    case BALANCED_MASTERS:
      computeMastersBlockedNodes(g, scalefactor, DecomposeFactor);
      break;
    case BALANCED_MASTERS_AND_EDGES:
      computeMastersBalancedNodesAndEdges(g, scalefactor, nodeWeight,
                                          edgeWeight, DecomposeFactor);
      break;
    case BALANCED_EDGES_OF_MASTERS:
    default:
      computeMastersBalancedEdges(g, scalefactor, edgeWeight, DecomposeFactor);
      break;
    }

    timer.stop();

    galois::runtime::reportStatCond_Tmax<MORE_DIST_STATS>(
        GRNAME, "MasterDistTime", timer.get());

    galois::gPrint(
        "[", id, "] Master distribution time : ", timer.get_usec() / 1000000.0f,
        " seconds to read ", g.num_bytes_read(), " bytes in ", g.num_seeks(),
        " seeks (", g.num_bytes_read() / (float)timer.get_usec(), " MBPS)\n");
    return numNodes_to_divide;
  }

  //! reader assignment from a file
  //! corresponds to master assignment if using an edge cut
  void readersFromFile(galois::graphs::OfflineGraph& g, std::string filename) {
    // read file lines
    std::ifstream mappings(filename);
    std::string curLine;

    unsigned timesToRead = id + 1;

    for (unsigned i = 0; i < timesToRead; i++) {
      std::getline(mappings, curLine);
    }

    std::vector<char> modifyLine(curLine.begin(), curLine.end());
    char* tokenizedString = modifyLine.data();
    char* token;
    token = strtok(tokenizedString, " ");

    // loop 6 more times
    for (unsigned i = 0; i < 6; i++) {
      token = strtok(NULL, " ");
    }
    std::string left(token);

    // 3 more times for right
    for (unsigned i = 0; i < 3; i++) {
      token = strtok(NULL, " ");
    }
    std::string right(token);

    gid2host.resize(numHosts);
    gid2host[id].first  = std::stoul(left);
    gid2host[id].second = std::stoul(right) + 1;
    galois::gPrint("[", id, "] Left: ", gid2host[id].first,
                   ", Right: ", gid2host[id].second, "\n");

    /////////////////////////
    // send/recv from other hosts
    /////////////////////////
    auto& net = galois::runtime::getSystemNetworkInterface();

    for (unsigned h = 0; h < numHosts; ++h) {
      if (h == id)
        continue;
      galois::runtime::SendBuffer b;
      galois::runtime::gSerialize(b, gid2host[id]);
      net.sendTagged(h, galois::runtime::evilPhase, b);
    }
    net.flush();
    unsigned received = 1;
    while (received < numHosts) {
      decltype(net.receiveTagged(galois::runtime::evilPhase)) p;
      do {
        p = net.receiveTagged(galois::runtime::evilPhase);
      } while (!p);
      assert(p->first != id);
      auto& b = p->second;
      galois::runtime::gDeserialize(b, gid2host[p->first]);
      ++received;
    }
    increment_evilPhase();

    // sanity checking assignment
    for (unsigned h = 0; h < numHosts; h++) {
      if (h == 0) {
        GALOIS_ASSERT(gid2host[h].first == 0);
      } else if (h == numHosts - 1) {
        GALOIS_ASSERT(gid2host[h].first == gid2host[h - 1].second,
                      gid2host[h].first, " ", gid2host[h - 1].second);
        GALOIS_ASSERT(gid2host[h].second == g.size(), gid2host[h].second, " ",
                      g.size());
      } else {
        GALOIS_ASSERT(gid2host[h].first == gid2host[h - 1].second,
                      gid2host[h].first, " ", gid2host[h - 1].second);
        GALOIS_ASSERT(gid2host[h].second == gid2host[h + 1].first,
                      gid2host[h].second, " ", gid2host[h + 1].first);
      }
    }
  }

  uint32_t G2L(uint64_t gid) const {
    assert(isLocal(gid));
    return globalToLocalMap.at(gid);
  }

  uint64_t L2G(uint32_t lid) const { return localToGlobalVector[lid]; }

public:
  //! Type representing a node in this graph
  using GraphNode = typename GraphTy::GraphNode;
  //! Expose EdgeTy to other classes
  using EdgeType = EdgeTy;
  //! iterator type over nodes
  using iterator = typename GraphTy::iterator;
  //! constant iterator type over nodes
  using const_iterator = typename GraphTy::const_iterator;
  //! iterator type over edges
  using edge_iterator = typename GraphTy::edge_iterator;

  /**
   * Constructor for DistGraph. Initializes metadata fields.
   *
   * @param host host number that this graph resides on
   * @param numHosts total number of hosts in the currently executing program
   */
  DistGraph(unsigned host, unsigned numHosts)
      : transposed(false), id(host), numHosts(numHosts) {
    mirrorNodes.resize(numHosts);
    ghostNodes.resize(numHosts);
    numGlobalNodes = 0;
    numGlobalEdges = 0;
  }

  /**
   * Return a vector of pairs denoting mirror node ranges.
   *
   * Assumes all mirror nodes occur after the masters: this invariant should be
   * held by CuSP.
   */
  std::vector<std::pair<uint32_t, uint32_t>> getMirrorRanges() const {
    std::vector<std::pair<uint32_t, uint32_t>> mirrorRangesVector;
    // order of nodes locally is masters, outgoing mirrors, incoming mirrors,
    // so just get from numOwned to end
    if (numOwned != numActualNodes) {
      assert(numOwned < numActualNodes);
      mirrorRangesVector.push_back(std::make_pair(numOwned, numActualNodes));
    }

    return mirrorRangesVector;
  }
  
  std::vector<std::pair<uint32_t, uint32_t>> getGhostRanges() const {
    std::vector<std::pair<uint32_t, uint32_t>> ghostRangesVector;
    if (numActualNodes != numNodes) {
      assert(numActualNodes < numNodes);
      ghostRangesVector.push_back(std::make_pair(numActualNodes, numNodes));
    }
    return ghostRangesVector;
  }

  std::vector<std::vector<size_t>>& getMirrorNodes() { return mirrorNodes; }
  std::vector<std::vector<size_t>>& getGhostNodes() { return ghostNodes; }

private:
  virtual unsigned getHostIDImpl(uint64_t) const = 0;
  virtual bool isOwnedImpl(uint64_t) const       = 0;
  virtual bool isLocalImpl(uint64_t) const       = 0;
  virtual bool isPresentImpl(uint32_t) const       = 0;
  virtual bool isGhostImpl(uint32_t) const       = 0;
  virtual bool isVertexCutImpl() const           = 0;
  virtual std::pair<unsigned, unsigned> cartesianGridImpl() const {
    return std::make_pair(0u, 0u);
  }

public:
  virtual ~DistGraph() {}
  
  //! Determines which host has the master for a particular node
  //! @returns Host id of node in question
  inline unsigned getHostID(uint64_t gid) const { return getHostIDImpl(gid); }
  //! Determines which host has the master for a particular local node
  //! @returns Host id of local node in question
  inline unsigned getHostIDForLocal(uint32_t lid) const {
      return localHostVector[lid];
  }
  //! Determine if a node has a master on this host.
  //! @returns True if passed in global id has a master on this host
  inline bool isOwned(uint64_t gid) const { return isOwnedImpl(gid); }
  //! Determine if a node can be on this host
  //! @returns True if passed in global id can be on this host
  inline bool isLocal(uint64_t gid) const { return isLocalImpl(gid); }
  //! Determine if a node has a proxy on this host
  //! @returns True if passed in global id has a proxy on this host
  inline bool isPresent(uint32_t lid) const { return isPresentImpl(lid); }
  //! Determine if a node is a ghost on this host
  inline bool isGhost(uint32_t lid) const { return isGhostImpl(lid); }
  /**
   * Returns true if current partition is a vertex cut
   * @returns true if partition being stored in this graph is a vertex cut
   */
  inline bool is_vertex_cut() const { return isVertexCutImpl(); }
  /**
   * Returns Cartesian split (if it exists, else returns pair of 0s
   */
  inline std::pair<unsigned, unsigned> cartesianGrid() const {
    return cartesianGridImpl();
  }

  bool isTransposed() { return transposed; }
  
  uint32_t getNumHosts() { return numHosts; }

  /**
   * Converts a local node id into a global node id
   *
   * @param nodeID local node id
   * @returns global node id corresponding to the local one
   */
  inline uint64_t getGID(const uint32_t nodeID) const { return L2G(nodeID); }

  /**
   * Converts a global node id into a local node id
   *
   * @param nodeID global node id
   * @returns local node id corresponding to the global one
   */
  inline uint32_t getLID(const uint64_t nodeID) const { return G2L(nodeID); }

  /**
   * Get data of a node.
   *
   * @param N node to get the data of
   * @param mflag access flag for node data
   * @returns A node data object
   */
  inline typename GraphTy::node_data_reference
  getData(GraphNode N,
          galois::MethodFlag mflag = galois::MethodFlag::UNPROTECTED) {
    auto& r = graph.getData(N, mflag);
    return r;
  }

  /**
   * Get the edge data for a particular edge in the graph.
   *
   * @param ni edge to get the data of
   * @param mflag access flag for edge data
   * @returns The edge data for the requested edge
   */
  inline typename GraphTy::edge_data_reference
  getEdgeData(edge_iterator ni,
              galois::MethodFlag mflag = galois::MethodFlag::UNPROTECTED) {
    auto& r = graph.getEdgeData(ni, mflag);
    return r;
  }

  /**
   * Gets edge destination of edge ni.
   *
   * @param ni edge id to get destination of
   * @returns Local ID of destination of edge ni
   */
  GraphNode getEdgeDst(edge_iterator ni) { return graph.getEdgeDst(ni); }

  /**
   * Gets the first edge of some node.
   *
   * @param N node to get the edge of
   * @returns iterator to first edge of N
   */
  inline edge_iterator edge_begin(GraphNode N) {
    return graph.edge_begin(N, galois::MethodFlag::UNPROTECTED);
  }

  /**
   * Gets the end edge boundary of some node.
   *
   * @param N node to get the edge of
   * @returns iterator to the end of the edges of node N, i.e. the first edge
   * of the next node (or an "end" iterator if there is no next node)
   */
  inline edge_iterator edge_end(GraphNode N) {
    return graph.edge_end(N, galois::MethodFlag::UNPROTECTED);
  }
  
  /**
   * Return the degree of the edge in the local graph
   */
  inline uint64_t localDegree(GraphNode N) {
    return graph.getDegree(N);
  }

  /**
   * Returns an iterable object over the edges of a particular node in the
   * graph.
   *
   * @param N node to get edges iterator over
   */
  inline galois::runtime::iterable<galois::NoDerefIterator<edge_iterator>>
  edges(GraphNode N) {
    return galois::graphs::internal::make_no_deref_range(edge_begin(N),
                                                         edge_end(N));
  }

  uint64_t getDegree(GraphNode N) const { return graph.getDegree(N); }

  /**
   * Gets number of nodes on this (local) graph.
   *
   * @returns number of nodes present in this (local) graph
   */
  inline size_t size() const { return graph.size(); }
  inline size_t actualSize() const { return graph.actualSize(); }

  /**
   * Gets number of edges on this (local) graph.
   *
   * @returns number of edges present in this (local) graph
   */
  inline size_t sizeEdges() const { return graph.sizeEdges(); }

  /**
   * Gets number of nodes on this (local) graph.
   *
   * @returns number of nodes present in this (local) graph
   */
  inline size_t numMasters() const { return numOwned; }
  
  inline size_t numMirrors() const { return numActualNodes - numOwned; }
  
  inline size_t numGhosts() const { return numNodes - numActualNodes; }

  /**
   * Gets number of nodes with edges (may include nodes without edges)
   * on this (local) graph.
   *
   * @returns number of nodes with edges (may include nodes without edges
   * as it measures a contiguous range)
   */
  inline size_t getNumNodesWithEdges() const { return numNodesWithEdges; }

  /**
   * Gets number of nodes on the global unpartitioned graph.
   *
   * @returns number of nodes present in the global unpartitioned graph
   */
  inline size_t globalSize() const { return numGlobalNodes; }

  /**
   * Gets number of edges on the global unpartitioned graph.
   *
   * @returns number of edges present in the global unpartitioned graph
   */
  inline size_t globalSizeEdges() const { return numGlobalEdges; }

  /**
   * Returns a range object that encapsulates all nodes of the graph.
   *
   * @returns A range object that contains all the nodes in this graph
   */
  inline const NodeRangeType& allNodesRange() const {
    assert(specificRanges.size() == 10);
    return specificRanges[0];
  }

  /**
   * Returns a range object that encapsulates both master and mirror nodes in this
   * graph.
   *
   * @returns A range object that contains both the master and mirror nodes in this graph
   */
  inline const NodeRangeType& presentNodesRange() const {
    assert(specificRanges.size() == 10);
    return specificRanges[1];
  }
  
  /**
   * Returns a range object that encapsulates only master nodes in this
   * graph.
   *
   * @returns A range object that contains the master nodes in this graph
   */
  inline const NodeRangeType& masterNodesRange() const {
    assert(specificRanges.size() == 10);
    return specificRanges[2];
  }
  
  /**
   * Returns a range object that encapsulates only mirror nodes in this
   * graph.
   *
   * @returns A range object that contains the mirror nodes in this graph
   */
  inline const NodeRangeType& mirrorNodesRange() const {
    assert(specificRanges.size() == 10);
    return specificRanges[3];
  }
  
  /**
   * Returns a range object that encapsulates only ghost nodes in this
   * graph.
   *
   * @returns A range object that contains the ghost nodes in this graph
   */
  inline const NodeRangeType& ghostNodesRange() const {
    assert(specificRanges.size() == 10);
    return specificRanges[4];
  }
  
  /**
   * Returns a range object that encapsulates all nodes of the graph.
   *
   * @returns A range object that contains all the nodes in this graph
   */
  inline const NodeRangeType& allNodesRangeReserved() const {
    assert(specificRanges.size() == 10);
    return specificRanges[5];
  }

  /**
   * Returns a range object that encapsulates both master and mirror nodes in this
   * graph.
   *
   * @returns A range object that contains both the master and mirror nodes in this graph
   */
  inline const NodeRangeType& presentNodesRangeReserved() const {
    assert(specificRanges.size() == 10);
    return specificRanges[6];
  }
  
  /**
   * Returns a range object that encapsulates only master nodes in this
   * graph.
   *
   * @returns A range object that contains the master nodes in this graph
   */
  inline const NodeRangeType& masterNodesRangeReserved() const {
    assert(specificRanges.size() == 10);
    return specificRanges[7];
  }
  
  /**
   * Returns a range object that encapsulates only mirror nodes in this
   * graph.
   *
   * @returns A range object that contains the mirror nodes in this graph
   */
  inline const NodeRangeType& mirrorNodesRangeReserved() const {
    assert(specificRanges.size() == 10);
    return specificRanges[8];
  }
  
  /**
   * Returns a range object that encapsulates only ghost nodes in this
   * graph.
   *
   * @returns A range object that contains the ghost nodes in this graph
   */
  inline const NodeRangeType& ghostNodesRangeReserved() const {
    assert(specificRanges.size() == 10);
    return specificRanges[9];
  }

  /**
   * Returns a vector object that contains the global IDs (in order) of
   * the master nodes in this graph.
   *
   * @returns A vector object that contains the global IDs (in order) of
   * the master nodes in this graph
   */
  std::vector<uint64_t> getMasterGlobalIDs() {
    std::vector<uint64_t> IDs;

    IDs.reserve(numMasters());
    for (auto node : masterNodesRange()) {
      IDs.push_back(getGID(node));
    }

    return IDs;
  }

protected:
  /**
   * Uses a pre-computed prefix sum to determine division of nodes among
   * threads.
   *
   * The call uses binary search to determine the ranges.
   */
  inline void determineThreadRanges() {
    allNodesRanges = galois::graphs::determineUnitRangesFromPrefixSum(
        galois::getActiveThreads(), graph.getEdgePrefixSum());
  }
  inline void determineThreadRangesReserved(uint32_t reserved) {
    assert(allNodesRanges.size() != 0);

    if (reserved == 0) {
        allNodesRangesReserved = allNodesRanges;
    }
    else {
        allNodesRangesReserved = galois::graphs::determineUnitRangesFromPrefixSum(galois::getActiveThreads() - reserved, graph.getEdgePrefixSum());
    }
  }

  /**
   * Determines the thread ranges for present nodes only and saves them to
   * the object.
   *
   * Only call after graph is constructed + only call once
   */
  inline void determineThreadRangesPresent() {
    // make sure this hasn't been called before
    assert(presentNodesRanges.size() == 0);

    galois::gDebug("Manually det. master thread ranges");
    presentNodesRanges = galois::graphs::determineUnitRangesFromGraph(
        graph, galois::getActiveThreads(), beginMaster,
        numActualNodes, 0);
  }
  inline void determineThreadRangesPresentReserved(uint32_t reserved) {
    assert(presentNodesRanges.size() != 0);

    if (reserved == 0) {
        presentNodesRangesReserved = presentNodesRanges;
    }
    else {
        presentNodesRangesReserved = galois::graphs::determineUnitRangesFromGraph(graph, galois::getActiveThreads() - reserved, beginMaster, numActualNodes, 0);
    }
  }

  /**
   * Determines the thread ranges for master nodes only and saves them to
   * the object.
   *
   * Only call after graph is constructed + only call once
   */
  inline void determineThreadRangesMaster() {
    // make sure this hasn't been called before
    assert(masterRanges.size() == 0);

    galois::gDebug("Manually det. master thread ranges");
    masterRanges = galois::graphs::determineUnitRangesFromGraph(
        graph, galois::getActiveThreads(), beginMaster,
        beginMaster + numOwned, 0);
  }
  inline void determineThreadRangesMasterReserved(uint32_t reserved) {
    assert(masterRanges.size() != 0);

    if (reserved == 0) {
        masterRangesReserved = masterRanges;
    }
    else {
        masterRangesReserved = galois::graphs::determineUnitRangesFromGraph(graph, galois::getActiveThreads() - reserved, beginMaster, beginMaster + numOwned, 0);
    }
  }
  
  /**
   * Determines the thread ranges for mirror nodes only and saves them to
   * the object.
   *
   * Only call after graph is constructed + only call once
   */
  inline void determineThreadRangesMirror() {
    // make sure this hasn't been called before
    assert(mirrorRanges.size() == 0);

    // first check if we even need to do any work; if already calculated,
    // use already calculated vector
    galois::gDebug("Manually det. mirror thread ranges");
    mirrorRanges = galois::graphs::determineUnitRangesFromGraph(graph, galois::getActiveThreads(), numOwned, numActualNodes, 0);
  }
  inline void determineThreadRangesMirrorReserved(uint32_t reserved) {
    assert(mirrorRanges.size() != 0);

    if (reserved == 0) {
        mirrorRangesReserved = mirrorRanges;
    }
    else {
        mirrorRanges = galois::graphs::determineUnitRangesFromGraph(graph, galois::getActiveThreads() - reserved, numOwned, numActualNodes, 0);
    }
  }
  
  /**
   * Determines the thread ranges for ghost nodes only and saves them to
   * the object.
   *
   * Only call after graph is constructed + only call once
   */
  inline void determineThreadRangesGhost() {
    // make sure this hasn't been called before
    assert(ghostRanges.size() == 0);

    // first check if we even need to do any work; if already calculated,
    // use already calculated vector
    galois::gDebug("Manually det. ghost thread ranges");
    ghostRanges = galois::graphs::determineUnitRangesFromGraph(graph, galois::getActiveThreads(), numActualNodes, numNodes, 0);
  }
  inline void determineThreadRangesGhostReserved(uint32_t reserved) {
    assert(ghostRanges.size() != 0);

    if (reserved == 0) {
        ghostRangesReserved = ghostRanges;
    }
    else {
        ghostRangesReserved = galois::graphs::determineUnitRangesFromGraph(graph, galois::getActiveThreads() - reserved, numActualNodes, numNodes, 0);
    }
  }

  /**
   * Initializes the 5 range objects that a user can access to iterate
   * over the graph in different ways.
   */
  void initializeSpecificRanges() {
    assert(specificRanges.size() == 0);

    // TODO/FIXME assertion likely not safe if a host gets no nodes
    // make sure the thread ranges have already been calculated
    // for the 5 ranges
    assert(allNodesRanges.size() != 0);
    assert(presentNodesRanges.size() != 0);
    assert(masterRanges.size() != 0);
    assert(mirrorRanges.size() != 0);
    assert(ghostRanges.size() != 0);
    assert(allNodesRangesReserved.size() != 0);
    assert(presentNodesRangesReserved.size() != 0);
    assert(masterRangesReserved.size() != 0);
    assert(mirrorRangesReserved.size() != 0);
    assert(ghostRangesReserved.size() != 0);

    // 0 is all nodes
    specificRanges.push_back(galois::runtime::makeSpecificRange(
        boost::counting_iterator<size_t>(0),
        boost::counting_iterator<size_t>(size()), allNodesRanges.data()));

    // 1 is master nodes
    specificRanges.push_back(galois::runtime::makeSpecificRange(
        boost::counting_iterator<size_t>(beginMaster),
        boost::counting_iterator<size_t>(beginMaster + numActualNodes),
        presentNodesRanges.data()));

    // 2 is master nodes
    specificRanges.push_back(galois::runtime::makeSpecificRange(
        boost::counting_iterator<size_t>(beginMaster),
        boost::counting_iterator<size_t>(beginMaster + numOwned),
        masterRanges.data()));

	// 3 is mirror nodes
    specificRanges.push_back(galois::runtime::makeSpecificRange(
        boost::counting_iterator<size_t>(numOwned),
        boost::counting_iterator<size_t>(numActualNodes),
        mirrorRanges.data()));
	
    // 4 is ghost nodes
    specificRanges.push_back(galois::runtime::makeSpecificRange(
        boost::counting_iterator<size_t>(numActualNodes),
        boost::counting_iterator<size_t>(numNodes),
        mirrorRanges.data()));
    
    // 5 is all nodes reserved
    specificRanges.push_back(galois::runtime::makeSpecificRange(
        boost::counting_iterator<size_t>(0),
        boost::counting_iterator<size_t>(size()), allNodesRangesReserved.data()));

    // 6 is master nodes reserved
    specificRanges.push_back(galois::runtime::makeSpecificRange(
        boost::counting_iterator<size_t>(beginMaster),
        boost::counting_iterator<size_t>(beginMaster + numActualNodes),
        presentNodesRangesReserved.data()));

    // 7 is master nodes reserved
    specificRanges.push_back(galois::runtime::makeSpecificRange(
        boost::counting_iterator<size_t>(beginMaster),
        boost::counting_iterator<size_t>(beginMaster + numOwned),
        masterRangesReserved.data()));

	// 8 is mirror nodes reserved
    specificRanges.push_back(galois::runtime::makeSpecificRange(
        boost::counting_iterator<size_t>(numOwned),
        boost::counting_iterator<size_t>(numActualNodes),
        mirrorRangesReserved.data()));
	
    // 9 is ghost nodes reserved
    specificRanges.push_back(galois::runtime::makeSpecificRange(
        boost::counting_iterator<size_t>(numActualNodes),
        boost::counting_iterator<size_t>(numNodes),
        mirrorRangesReserved.data()));
    
    assert(specificRanges.size() == 10);
  }

public:
  /**
   * Write the local LC_CSR graph to the file on a disk.
   *
   * @todo revive this
   */
  void save_local_graph_to_file(std::string) { GALOIS_DIE("not implemented"); }

  /**
   * Read the local LC_CSR graph from the file on a disk.
   *
   * @todo revive this
   */
  void read_local_graph_from_file(std::string) {
    GALOIS_DIE("not implemented");
  }

  /**
   * Deallocates underlying LC CSR Graph
   */
  void deallocate() {
    galois::gDebug("Deallocating CSR in DistGraph");
    graph.deallocate();
  }

  /**
   * Sort the underlying LC_CSR_Graph by ID (destinations)
   * It sorts edges of the nodes by destination.
   */
  void sortEdgesByDestination() {
    using GN = typename GraphTy::GraphNode;
    galois::do_all(
        galois::iterate(graph),
        [&](GN n) { graph.sortEdges(n, IdLess<GN, EdgeTy>()); },
        galois::no_stats(), galois::loopname("CSREdgeSort"), galois::steal());
  }
};

template <typename NodeTy, typename EdgeTy>
constexpr const char* const galois::graphs::DistGraph<NodeTy, EdgeTy>::GRNAME;
} // end namespace graphs
} // end namespace galois

#endif //_GALOIS_DIST_HGRAPH_H
