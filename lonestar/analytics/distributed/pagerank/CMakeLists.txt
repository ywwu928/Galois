app_dist(pagerank_pull pagerank-pull)
add_test_dist(pagerank-pull-dist rmat15 ${BASEINPUT}/scalefree/rmat15.gr -graphTranspose=${BASEINPUT}/scalefree/transpose/rmat15.tgr -maxIterations=100)

app_dist(pagerank_push pagerank-push)
add_test_dist(pagerank-push-dist rmat15 ${BASEINPUT}/scalefree/rmat15.gr -graphTranspose=${BASEINPUT}/scalefree/transpose/rmat15.tgr -maxIterations=100)

app_dist(static_pagerank_push static-pagerank-push)
app_dist(dynamic_pagerank_push dynamic-pagerank-push)
app_dist(ideal_pagerank_push ideal-pagerank-push)
