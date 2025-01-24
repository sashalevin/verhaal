<!---
SPDX-License-Identifier: GPL-2.0-only
Copyright (c) 2024 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
-->
# verhaal

Tool used to build a database up of the Linux kernel commits, including all
stable branches, in order to quickly and simply query it later on to determine
where patches are commited across branches and what commits fix which other
commits.

This is used by the https://git.kernel.org/pub/scm/linux/security/vulns.git/
tools that the Linux kernel CVE team use to manage and track CVEs for Linux.
It can also be used by anyone else who just wants to know "what commit id was
backported where", as that is a common query by many companies / developers
when trying to figure out bugs and backports that might be needed and not
completed.

## Building and installing

verhaal depends on libgit2 and sqlite3, be sure that libraries are properly
installed first.

To work with the "raw" repo, after cloning it just do:

	./autogen.sh

which will build everything and place the binary into the `build/`
subdirectory.

verhaal uses meson to build, so if you wish to just build by hand you can do:

	meson setup build
	cd build/
	meson compile

## Using

To build the initial database, run

	verhaal

by default, the location of the Linux kernel tree will be sourced from the
`CVEKERNELTREE` environment variable and the database output will be written to
a `verhaal.db` file.  To override these, either use a command line option, or
change the environment variable.

After the database has been created, the script

	id_found_in <SHA1>

can be used to print out what commit the specified `SHA1` commit id was
backported to.

Note, the search requires the FULL SHA1 value, substrings are not supported at
this point in time.  Please normalized the git id BEFORE running the
`id_found_in` script.

## Contributing

If you have patches or suggestions, you can submit them either via email
[to the maintainer] or [to the Linux CVE email alias].

Please note that commits must include a `Signed-off-by` trailer, indicating that
you comply with the [Developer Certificate of Origin v1.1].

In addition, when adding new files or contributing to existing ones, ensure
that the SPDX tags `SPDX-FileCopyrightText` and `SPDX-License-Identifier` are
available and are kept up-to date.  You can learn more and do that via
[reuse-tool].

[to the maintainer]: mailto:gregkh@linuxfoundation.org
[to the Linux CVE email alias]: mailto:cve@kernel.org
[pull request]: https://github.com/gregkh/usbutils/pulls
[Developer Certificate of Origin v1.1]: https://developercertificate.org/
[reuse-tool]: https://github.com/fsfe/reuse-tool
