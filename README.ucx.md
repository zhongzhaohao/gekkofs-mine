We modify gekkofs to adapt to TH-express network through UCX support
To compile:
1. cd scripts/
2. run clean_up_compiled_deps.sh to clean up previously compilation files
3. run ./compile_dep.sh -p arm:latest ../deps-arm ../deps-arm-install/ to compile dependencies
4. cd .. && source env.sh
5. ./build.sh && cd build && make -j install

# Redis backend usage:
gkfs_daemon -d redisdb --server xxx/deps-arm-install/redis-server

# Memcached backend usage:
gkfs_daemon -d memcacheddb --server xxx/deps-arm-install/memcached

# gkfs_stage usage:
details:
    source env.sh
    gkfs_stage --help
examples:
    gkfs_stage xxx/fileA  /gkfs_mount/fileB 
    gkfs_stage /gkfs_mount/fileB  xxx/fileA --o_direct_size 4096 --n_buffers 3 --threads 8 -a 4MB
    gkfs_stage xxx/fileA  /gkfs_mount/fileB -b 32MB --threads 6 --nodes 3 --force 
    gkfs_stage xxx/fileA  /gkfs_mount/fileB --nodelists [cn1005-1009,cn1002,cn1023] -f -b 128mb -t 2