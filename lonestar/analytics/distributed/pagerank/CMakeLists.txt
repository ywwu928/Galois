app_dist(pagerank_push pagerank-push)
app_dist(pagerank_pull pagerank-pull)

if (NOT GALOIS_FULL_MIRRORING)
    app_dist(pagerank_hybrid pagerank-hybrid)
endif()
