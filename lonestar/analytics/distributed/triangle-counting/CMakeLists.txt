app_dist(tc triangle-counting)
target_link_libraries(triangle-counting-dist Galois::pangolin)

app_dist(static_tc static-triangle-counting)
target_link_libraries(static-triangle-counting-dist Galois::pangolin)
