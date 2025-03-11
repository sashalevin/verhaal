// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2024-2025 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
//
// 'verhaal' - builds a database of kernel commits so that we can search them later on
//
// This is a "replacement" for the old-school "abuse the filesystem as a database" tool that we use
// to store all Linux kernel mainline and stable kernel commits in so that we can "quickly" search
// them.
//
// Instead of using the database and grep we do it all in a sql database and then we can search it
// with some simple sql statements.  Cuts time to search from about .6 seconds to .01 seconds on my
// semi-slow-storage device.
//
// This is the "create the database" program.  It does it all in ram and then writes it out to disk
// when finished as that's much faster.  It recreates the world, all at once.  Ideally we should
// "just" add the new records when they are found, but that's for another day...
//

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <getopt.h>
#include <git2.h>
#include "ccan/list/list.h"

#include "verhaal.h"
#include "terminal.h"

/* A specific commit */
struct commit {
	struct list_node node;
	char *sha;
	char *release;
	char *mainline_id;
	char *reverts;
	char *fixes;
	int mainline;
};

git_repository *git_repo;
static char *git_repo_location;
static const char *database_name_default = DATABASE_NAME;
static int num_commits;

static bool fixes_print = false;

// Dumb "only print stuff when greg is debugging the code" function
static bool debug = false;
__attribute__((__format__(printf, 1, 2))) int dbg(const char *fmt, ...)
{
	va_list args;
	int ret;

	if (!debug)
		return 0;

	va_start(args, fmt);
	ret = vprintf(fmt, args);
	va_end(args);

	return ret;
}

static int git_init(void)
{
	int ret;

	git_libgit2_init();

	ret = git_repository_open(&git_repo, git_repo_location);
	if (ret) {
		fprintf(stderr, "Error opening git repo at %s", git_repo_location);
		return ret;
	}

	return 0;
}

static void git_shutdown(void)
{
	git_repository_free(git_repo);
	git_libgit2_shutdown();
}

static char *find_sha1_full(const char *string)
{
	return search_string(string, "[a-f0-9]{40,}");
}

static char *find_sha1_short(const char *string)
{
	// At least 7 characters long, we might miss some odd ones, but
	// it's a good start as they _should_ all be at least 12 long.
	return search_string(string, "[a-f0-9]{7,}");
}

static char *find_upstream(const char *message)
{
	char *upstream;
	char *sha1;

	upstream = search_string(message, ".*upstream.*\n?");
	if (!upstream)
		return NULL;

	sha1 = find_sha1_full(upstream);
	free(upstream);
	return sha1;
}

static char *find_reverts(const char *message)
{
	char *reverts;
	char *sha1;

	reverts = search_string(message, ".*reverts.*\n?");
	if (!reverts)
		return NULL;
	sha1 = find_sha1_full(reverts);
	free(reverts);
	return sha1;
}

// Handle a single "Fixes:" line
static char *find_fix(const char *line)
{
	char *fix;
	char *sha1;

	fix = search_string(line, "fixes:.*\n?");
	if (!fix)
		return NULL;
	sha1 = find_sha1_short(fix);
	free(fix);
	return sha1;
}

// Turn a "short" Fixes SHA1 into a "full" SHA1 if it is present in the git tree
static char *fixes_expand(char *fix, const char *line)
{
	git_revspec revspec;
	const git_oid *oid;
	char *sha;

	int ret = git_revparse(&revspec, git_repo, fix);

	if (ret) {
		// Fix sha was not in the git tree, see if it is in our table of "fixup" sha values:
		char *translate_sha = fix_translate(fix);
		if (translate_sha != NULL)
			return translate_sha;

		// Fix was not in the translate list, so just return the string given to us after
		// potentially logging it to stderr if that option was enabled by the user

		//fprintf(stderr, "Invalid fix line: '%s' '%s'", fix, line);
		//fprintf(stderr, "%s is NOT a valid fix in the kernel tree, please fix...\n", fix);

		if (fixes_print) {
			// Dig out the SHA1: ("foo") type info so we can look them up elsewhere
			char *temp = search_string(line, "[a-f0-9]{10,}.*");
			fprintf(stderr, "%s\n", temp);
			free(temp);
		}
		return fix;
	}

	// Turn the git oid into a full sha1
	oid = git_object_id(revspec.from);
	sha = malloc(100);
	git_oid_tostr(sha, 100, oid);
	dbg("		short fixes: %s, expanded sha = %s\n", fix, sha);

	git_object_free(revspec.from);
	git_object_free(revspec.to);
	free(fix);
	return sha;
}

#if 0
// Test message to check for multiple Fixes: lines
static const char *test_message =
"    Reported-by: Juri Lelli <juri.lelli@redhat.com> # original bug\n"
"    Reported-by: Manu Bretelle <chantra@meta.com> # bugs in masking fix\n"
"    Fixes: 3f00c5239344 (\"bpf: Allow trusted pointers to be passed to KF_TRUSTED_ARGS kfuncs\")\n"
"    Fixes: cb4158ce8ec8 (\"bpf: Mark raw_tp arguments with PTR_MAYBE_NULL\")\n"
"    Reviewed-by: Eduard Zingerman <eddyz87@gmail.com>\n"
"    Co-developed-by: Jiri Olsa <jolsa@kernel.org>";
#endif

// Handle the message one line at a time, as that's simpler than attempting a recursive search of a
// large buffer
static char *find_fixes(const char *message)
{
	char *final, *temp;

	final = malloc(1024);
	memset(final, 0x00, 1024);
	temp = malloc(1024);

	// Split the message up into one-per-line in a destructive way
	// and then feed that to find_fix and build up a string of sha1
	// values as a "fixes" line
	char *local_message = strdup(message);
	char *line = strtok(local_message, "\n");
	while (line) {
		//fprintf(stdout, "line: '%s'\n", line);
		char *f = find_fix(line);
		if (f) {
			// turn this into a fully-formed SHA1, not just an abbreviated one
			f = fixes_expand(f, line);
			if (strlen(final) == 0)
				snprintf(temp, 1024, "%s", f);
			else
				snprintf(temp, 1024, "%s %s", final, f);

			strcpy(final, temp);
			free(f);
		}
		line = strtok(NULL, "\n");
	}
	free(local_message);
	//fprintf(stdout, "final='%s'\n", final);
	free(temp);

	// If we didn't find anything, just return NULL to keep things sane
	if (strlen(final) == 0) {
		free(final);
		return NULL;
	}
	return final;
}
static void create_commit(struct version_range *vr,
			  const char *sha, const char *release, int mainline,
			  const char *mainline_id, const char *reverts, const char *fixes)
{
	struct commit *c = calloc(1, sizeof(*c));

	if (!c) {
		fprintf(stderr, "Out of memory, aborting!\n");
		exit(1);
	}

	c->sha = strdup(sha);
	c->release = strdup(release);
	if (mainline_id)
		c->mainline_id = strdup(mainline_id);
	if (reverts)
		c->reverts = strdup(reverts);
	if (fixes)
		c->fixes = strdup(fixes);
	c->mainline = mainline;
	list_node_init(&c->node);

	list_add(&vr->commits, &c->node);
}

static int create_kernel_range(struct version_range *vr)
{
	const char *start = vr->from.name;
	const char *end = vr->to.name;
	char *upstream = NULL;
	char *reverts = NULL;
	char *fixes = NULL;
	char range[256];
	git_oid oid;
	git_revwalk *walker;
	int ret;
	int mainline_int;

	// Set "is this mainline or not" flag to be stored later
	if (vr->mainline)
		mainline_int = 1;
	else
		mainline_int = 0;

	dbg("%s: start=%s, end=%s, mainline=%d\n", __func__, start, end, mainline_int);

	// Let's first see if these are a few "known" ranges that we know we can
	// never find, thanks to the start of the git repo and how the first few
	// tags were set up.
	// (i.e. v2.6.11 is NOT a real commit, but rather a tree.)
	if ((strcmp(start, "2.6.11") == 0) || (strcmp(end, "2.6.11") == 0)) {
		dbg("skipping invalid 2.6.11 commit as that's just a mess\n");
		return 0;
	}
	if ((strcmp(end, "3.16.35") == 0) || (strcmp(end, "2.6.11") == 0)) {
		dbg("skipping invalid 3.16.35 commit as there was a 'break' there\n");
		return 0;
	}

	// Loop through all git ids in this range, take the id and version and store it in the
	// database
	ret = git_revwalk_new(&walker, git_repo);
	if (ret) {
		fprintf(stderr, "Error, can not init a revwalk object\n");
		return ret;
	}

	snprintf(range, sizeof(range), "v%s..v%s", start, end);

	ret = git_revwalk_push_range(walker, range);
	if (ret) {
		fprintf(stderr, "Error, can not get the range of '%s'\n", range);
		git_revwalk_free(walker);
		return ret;
	}

	terminal_fprintf(stdout, TERMINAL_SAVE_CURSOR);
	terminal_fprintf(stdout, "  Processing kernel commits from "
			 TERMINAL_FG_BLUE "v%s" TERMINAL_FG_DEFAULT " to "
			 TERMINAL_FG_BLUE "v%s" TERMINAL_FG_DEFAULT "" TERMINAL_CLEAR_RIGHT, start, end);
	fflush(stdout);

	while (!git_revwalk_next(&oid, walker)) {
		char sha[256];
		const char *message;
		git_commit *commit;

		// Turn the git oid into a full sha1
		git_oid_tostr(sha, sizeof(sha), &oid);

		// Get the git commit message so we can search it for stuff
		ret = git_commit_lookup(&commit, git_repo, &oid);
		if (ret) {
			fprintf(stderr, "git message lookup for %s failed\n", sha);
			continue;
		}

		message = git_commit_message(commit);

		// If this is not a mainline range, search the changelog message
		// to figure out if this is an upstream id, and if so, what it
		// is and then save it off.
		if (!vr->mainline) {
			upstream = find_upstream(message);
			if (upstream)
				dbg("	upstream=%s\n", upstream);
		}

		// Find if this is a revert
		reverts = find_reverts(message);
		if (reverts)
			dbg("	reverts=%s\n", reverts);

		// Find if this commit fixes anything
		fixes = find_fixes(message);
		if (fixes)
			dbg("	fixes=%s\n", fixes);

		git_commit_free(commit);

		// Save the commit off in the list of commits for this range
		create_commit(vr, sha, end, mainline_int, upstream, reverts, fixes);

		// Racy...
		num_commits++;

		if (upstream)
			free(upstream);
		if (reverts)
			free(reverts);
		if (fixes)
			free(fixes);
	}
	ret = 0;

	git_revwalk_free(walker);
	terminal_fprintf(stdout, TERMINAL_RESTORE_CURSOR);
	fflush(stdout);
	return ret;
}

static int save_commits(struct version_range *vr)
{
	const char *start = vr->from.name;
	const char *end = vr->to.name;
	struct commit *c;
	struct commit *temp;
	int ret;

	terminal_fprintf(stdout, TERMINAL_SAVE_CURSOR);
	terminal_fprintf(stdout, "  Saving kernel commits from "
			 TERMINAL_FG_BLUE "v%s" TERMINAL_FG_DEFAULT " to "
			 TERMINAL_FG_BLUE "v%s" TERMINAL_FG_DEFAULT "" TERMINAL_CLEAR_RIGHT, start, end);
	fflush(stdout);

	list_for_each_safe(&vr->commits, c, temp, node) {
		// Save it in the database
		ret = db_commit_add(c->sha, c->release, c->mainline, c->mainline_id, c->reverts, c->fixes);
		if (ret)
			goto exit;
		// Free the memory
		free(c->sha);
		free(c->release);
		if (c->mainline_id)
			free(c->mainline_id);
		if (c->reverts)
			free(c->reverts);
		if (c->fixes)
			free(c->fixes);
		list_del(&c->node);
		free(c);
	}
exit:
	terminal_fprintf(stdout, TERMINAL_RESTORE_CURSOR);
	fflush(stdout);
	return 0;
}


static const char *short_options = "Vvhfd:";

static const struct option long_options[] = {
	{
		.val =		'V',
		.name =		"version",
		.has_arg =	no_argument,
		.flag =		NULL,
	},
	{
		.val =		'v',
		.name =		"verbose",
		.has_arg =	no_argument,
		.flag =		NULL,
	},
	{
		.val =		'h',
		.name =		"help",
		.has_arg =	no_argument,
		.flag =		NULL,
	},
	{
		.val =		'f',
		.name =		"fixes",
		.has_arg =	no_argument,
		.flag =		NULL,
	},
	{
		.val =		'd',
		.name =		"database",
		.has_arg =	required_argument,
		.flag =		NULL,
	},
};

static void help(void)
{
	fprintf(stdout, "  Creates a database file of all of the Linux stable branch commits.\n");
	fprintf(stdout, "  This is used by the vulns.git CVE scripts, and other tools to track\n");
	fprintf(stdout, "  commits as they are made across multiple branches.\n\n");
	fprintf(stdout, "  Valid options:\n");
	fprintf(stdout, "	--help  -h	This message\n");
	fprintf(stdout, "	--fixes -f	Any invalid \"Fixes:\" lines will get written to stderr\n");
	fprintf(stdout, "	--verbose -V	Turn debugging messages on (warning, lots of junk here)\n");
	fprintf(stdout, "	--version -v	Print the version of the program and exit\n");
	fprintf(stdout, "	--database=	Change the default database name from '%s' to the provided one\n",
		database_name_default);
	exit(1);
}

static int get_options(int argc, char *argv[])
{
	const char *env_string;
	int option;

	env_string = getenv("CVEKERNELTREE");
	if (!env_string) {
		fprintf(stderr, "Error: Environment variable 'CVEKERNELTREE' must be set to point to\n");
		fprintf(stderr, "       the Linux kernel stable git repository directory.\n");
		return -1;
	}
	git_repo_location = strdup(env_string);

	while ((option = getopt_long(argc, argv, short_options, long_options, NULL)) != EOF) {
		switch (option) {
		case 'V':
			debug = true;
			break;

		case 'v':
			exit(0);

		case 'f':
			fixes_print = true;
			break;

		case 'd':
			database_name = strdup(optarg);
			break;

		case 'h':
		default:
			help();
		}
	}

	if (database_name == NULL)
		database_name = strdup(database_name_default);

	return 0;
}

int main(int argc, char *argv[])
{
	struct vh_timestamp *foo;
	double seconds;
	int ret;

//	int nproc = sysconf(_SC_NPROCESSORS_CONF);
//	printf("nproc = %d\n", nproc);

	terminal_fprintf(stdout, TERMINAL_FG_GREEN "%s" TERMINAL_FG_DEFAULT
			 " version " TERMINAL_FG_BLUE "%s" TERMINAL_FG_DEFAULT "\n",
			 PACKAGE_NAME, VERSION);

	ret = get_options(argc, argv);
	if (ret)
		return ret;

	terminal_fprintf(stdout, "  Reading from stable kernel tree at '"
			 TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT "'\n", git_repo_location);
	terminal_fprintf(stdout, "  Database file is '"
			 TERMINAL_FG_CYAN "%s" TERMINAL_FG_DEFAULT "'\n", database_name);

	ret = db_init();
	if (ret)
		goto exit;

	ret = git_init();
	if (ret)
		goto exit;

	versions_create();

	fixes_init();

	foo = time_start("process_commits");
	for_each_range_do(&create_kernel_range);

	terminal_fprintf(stdout, "\n");

	seconds = time_stop(foo);
	terminal_fprintf(stdout, "    Processing "
			 TERMINAL_FG_CYAN "%d" TERMINAL_FG_DEFAULT
			 " commits took "
			 TERMINAL_FG_CYAN "%.5f" TERMINAL_FG_DEFAULT
			 " seconds, "
			 TERMINAL_FG_CYAN "%.5f" TERMINAL_FG_DEFAULT
			 " seconds/commit\n", num_commits, seconds,
			 (seconds / (double)num_commits));

	foo = time_start("save_commits");
	for_each_range_do(&save_commits);
	seconds = time_stop(foo);
	terminal_fprintf(stdout, "\n");

	terminal_fprintf(stdout, "    Saved commits to database in "
			 TERMINAL_FG_CYAN "%.5f" TERMINAL_FG_DEFAULT
			 " seconds\n", seconds);

	foo = time_start("git_shutdown");
	git_shutdown();
	seconds = time_stop(foo);
	terminal_fprintf(stdout, "    Git shutdown took "
			 TERMINAL_FG_CYAN "%.5f" TERMINAL_FG_DEFAULT
			 " seconds\n", seconds);

	db_write_to_disk();

exit:
	free(database_name);
	free(git_repo_location);
	db_shutdown();
	return ret;
}
