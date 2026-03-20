app_dist(bfs_push bfs-push)
app_dist(bfs_pull bfs-pull)

if (NOT GALOIS_FULL_MIRRORING)
    app_dist(bfs_hybrid bfs-hybrid)
endif()
