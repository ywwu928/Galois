app_dist(kcore_push k-core-push)
app_dist(kcore_pull k-core-pull)

if (NOT GALOIS_FULL_MIRRORING)
    app_dist(kcore_hybrid k-core-hybrid)
endif()
