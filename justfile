#!/usr/bin/env -S just --justfile
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2024 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
#
# justfile to help remember how to do some maintenance stuff when maintaining
# the files in this tree
#

# show the list of options
_help:
	@just --list


# Update the verhaal.spdx file
@spdx:
	reuse --root . spdx --creator-organization="The Linux Foundation" --creator-person="Greg Kroah-Hartman <gregkh@linuxfoundation.org>" > verhaal.spdx


# Run the "reuse lint" tool
@lint:
	reuse --root . lint


# Build the source, setting up things if not present
@make:
	./autogen.sh


# Build the database, using the defaults
@build_db:
	./build/verhaal
