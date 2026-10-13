#!/bin/bash

# Copyright (c) Arduino s.r.l. and/or its affiliated companies
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PATCHES_ROOT="$SCRIPT_DIR/patches"

# Applies every extra/patches/<west-project>/*.patch to the matching west project.
if [ ! -d "$PATCHES_ROOT" ]; then
	# No patch set is configured.
	exit 0
fi

if ! command -v west >/dev/null 2>&1; then
	echo "[ERROR] west is not available; cannot resolve west workspace"
	exit 1
fi

for project_dir in "$PATCHES_ROOT"/*/; do
	[ -d "$project_dir" ] || continue
	project="$(basename "$project_dir")"

	project_path="$(west list -f '{abspath}' "$project" 2>/dev/null || true)"
	if [ -z "$project_path" ]; then
		echo "[ERROR] Unable to locate west project '$project' with 'west list $project'"
		exit 1
	fi

	if [ ! -d "$project_path/.git" ]; then
		echo "[ERROR] Repository for '$project' not found at '$project_path'"
		exit 1
	fi

	applied_any=false
	for patch in "$project_dir"*.patch; do
		[ -e "$patch" ] || continue
		patch_name="$(basename "$patch")"

		if git -C "$project_path" apply --check "$patch" >/dev/null 2>&1; then
			echo "[INFO] Applying $project patch: $patch_name"
			git -C "$project_path" apply "$patch"
			applied_any=true
		elif git -C "$project_path" apply --reverse --check "$patch" >/dev/null 2>&1; then
			echo "[INFO] $project patch already applied: $patch_name"
		else
			echo "[ERROR] Cannot apply $project patch cleanly: $patch_name"
			echo "[ERROR] Patch file: $patch"
			exit 1
		fi
	done

	if [ "$applied_any" = true ]; then
		echo "[INFO] $project patch set applied"
	fi
done
