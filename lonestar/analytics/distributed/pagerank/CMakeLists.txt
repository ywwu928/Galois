app_dist(pagerank_push pagerank-push)
add_test_dist(pagerank-push-dist rmat15 ${BASEINPUT}/scalefree/rmat15.gr -graphTranspose=${BASEINPUT}/scalefree/transpose/rmat15.tgr -maxIterations=100)

app_dist(full_pagerank_push full-pagerank-push)
