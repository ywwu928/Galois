app_dist(cc_push connected-components-push)
add_test_dist(connected-components-push-dist rmat15 ${BASEINPUT}/scalefree/symmetric/rmat15.sgr -symmetricGraph)

app_dist(cc_pull connected-components-pull)
add_test_dist(connected-components-pull-dist rmat15 ${BASEINPUT}/scalefree/symmetric/rmat15.sgr -symmetricGraph)

app_dist(static_cc_push static-connected-components-push)
app_dist(dynamic_cc_push dynamic-connected-components-push)
app_dist(ideal_cc_push ideal-connected-components-push)
app_dist(normal_cc_push normal-connected-components-push)
