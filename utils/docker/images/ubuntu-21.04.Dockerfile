# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2021-2022, Intel Corporation

#
# Dockerfile - a 'recipe' for Docker to build an image of ubuntu-based
#              environment prepared for running pmemstream tests.
#

# Pull base image
FROM ghcr.io/pmem/dev-utils-kit:ubuntu-21.04-latest
MAINTAINER igor.chorazewicz@intel.com

# use 'root' while building the image
USER root

# Misc for our builds/CI (optional)
ARG MISC_DEPS="\
	clang-format-9"

# Install all required packages
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
	${MISC_DEPS} \
&& rm -rf /var/lib/apt/lists/*

# Install all PMDK packages
RUN /opt/install-pmdk.sh /opt/pmdk/

# Install rapidcheck
COPY install-rapidcheck.sh install-rapidcheck.sh
RUN ./install-rapidcheck.sh

# Download scripts required in run-*.sh
COPY download-scripts.sh download-scripts.sh
COPY 0001-fix-generating-gcov-files-and-turn-off-verbose-log.patch 0001-fix-generating-gcov-files-and-turn-off-verbose-log.patch
RUN ./download-scripts.sh

# switch back to regular user
USER ${USER}
