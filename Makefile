PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=ui
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# The version this extension is stamped with, and the one DuckDB checks before
# loading it. It has to match what the target binary reports for pragma_version
# exactly, or the load is refused.
#
# It cannot be derived here. DuckDB's main branch reports v2.0.0-alpha37626 at
# runtime -- 2.0.0-alpha plus a commit count, synthesised by DuckDB's own
# release tooling -- while `git describe` on that same commit answers
# v1.5.5-<n>-g7e14bd24e0, because the newest tag reachable from main is still
# the 1.5.5 one and there is no v2 tag to describe against. CI makes it worse:
# submodules are cloned shallow, so `git describe` often has no tags at all.
#
# So it is stated, and it travels with the duckdb submodule pointer: this value
# and 7e14bd24e0 are one pair, and moving either without the other produces an
# extension that no binary will load. `?=` so a different nightly can be built
# without editing this file:
#
#   OVERRIDE_GIT_DESCRIBE=v2.0.0-alphaNNNNN make release
OVERRIDE_GIT_DESCRIBE ?= v2.0.0-alpha37626
export OVERRIDE_GIT_DESCRIBE

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# OpenSSL 3.6+ requires Perl core modules (File::Compare, FindBin, ...) at its
# ./Configure step. The linux_amd64 LTS build image ships only perl-IPC-Cmd, so
# building OpenSSL from source fails there with a missing-Perl-module error.
# Install perl-core before the build runs. Guarded so it is a no-op anywhere
# without yum (macOS, Windows) and when OpenSSL is restored from the vcpkg cache.
release debug: | install-build-prereqs

install-build-prereqs:
	@command -v yum >/dev/null 2>&1 && yum install -y perl-core || true
