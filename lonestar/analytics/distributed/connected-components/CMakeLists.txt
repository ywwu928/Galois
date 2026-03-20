app_dist(cc_push connected-components-push)
app_dist(cc_pull connected-components-pull)

if (NOT GALOIS_FULL_MIRRORING)
    app_dist(cc_hybrid connected-components-hybrid)
endif()
