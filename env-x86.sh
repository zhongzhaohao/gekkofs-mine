#!/bin/bash

GKFS_HOME=$PWD
export LD_LIBRARY_PATH=${GKFS_HOME}/deps-install/lib64:${GKFS_HOME}/deps-install/lib:${GKFS_HOME}/install/lib:${LD_LIBRARY_PATH}
export LIBRARY_PATH=${LD_LIBRARY_PATH}

