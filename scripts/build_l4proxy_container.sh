#!/bin/bash

# build the l4proxy container image
docker build -f ../container/Dockerfile --build-arg BESS_DIR=../bess -t l4-proxy --no-cache ..