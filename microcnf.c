#include <glib.h>
#include <gio/gio.h>
#include <solv/pool.h>
#include <solv/repo.h>
#include <solv/repo_solv.h>

G_DEFINE_QUARK(Microcnf, microcnf)
static const gchar* REPO_DIR = "/etc/zypp/repos.d/";

static Pool* microcnf_load_repos(GError** err) {
	g_assert(*err == NULL);
	g_autoptr(GFile) repos_dir = g_file_new_for_path(REPO_DIR);

	GFileEnumerator* iter = g_file_enumerate_children(repos_dir, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE, NULL, err);
	if (iter == NULL)
		return NULL;

	Pool* solv_pool = pool_create();
	if (!solv_pool) {
		*err = g_error_new(microcnf_quark(), 0x80, "pool_create returned NULL");
		return NULL;
	}

	while (TRUE) {
		GFileInfo* info;
		GFile* file;
		// Does it make sense to simply warn the user and continue execution or should
		// microcnf abort when it finds an inproper repo file?
		GError* iter_err = NULL;
		if (!g_file_enumerator_iterate(iter, &info, &file, NULL, &iter_err)) {
			g_warning("Failed querying repo file: %s", iter_err->message);
			g_error_free(iter_err);
			continue;
		}
		if (!info)
			break;

		const gchar* filename = g_file_info_get_name(info);
		if (!g_str_has_suffix(filename, ".repo"))
			continue;

		g_autoptr(GKeyFile) repo_file = g_key_file_new();
		if (!g_key_file_load_from_file(repo_file, g_file_peek_path(file) /*is peek path the proper encoding?*/, G_KEY_FILE_NONE, &iter_err)) {
			g_warning("Failed parsing repo file: %s", iter_err->message);
			g_error_free(iter_err);
			continue;
		}

		gsize num_sections;
		gchar** sections = g_key_file_get_groups(repo_file, &num_sections);

		for (gsize i = 0; i < num_sections; i++) {
			GError* section_iter_err = NULL;
			g_autofree gchar* enabled = g_key_file_get_string(repo_file, sections[i], "enabled", &section_iter_err);
			if (!enabled) {
				if (section_iter_err) {
					g_warning("Failed parsing section of repo file: %s", section_iter_err->message);
					g_error_free(section_iter_err);
				}
				continue;
			}
			if (g_strcmp0(enabled, "1") == 0) {
				g_autoptr(GString) section = g_string_new(sections[i]);
				g_string_replace(section, "/", "_", 0);
				g_autoptr(GFile) solv_data = g_file_new_build_filename("/var/cache/zypp/solv/", section->str, "solv", NULL);

				Repo* solv_repo = repo_create(solv_pool, sections[i]);
				if (!solv_pool) {
					g_warning("repo_create(%p, %s) failed", (void*)solv_pool, sections[i]);
					continue;
				}

				errno = 0;
				FILE* solv_data_fp = fopen(g_file_peek_path(solv_data), "r");
				if (errno) {
					g_critical("Failed open solv data: %s", strerror(errno));
					// what does that reuseids arg do?
					repo_free(solv_repo, 1);
					continue;
				}
				repo_add_solv(solv_repo, solv_data_fp, 0);
			}
		}

		g_strfreev(sections);
	}

	g_object_unref(iter);
	return solv_pool;
}

typedef struct {
	gchar* repo;
	gchar* package;
	gchar* path;
} MicrocnfPackageResult;
static void microcnf_package_result_free_inner(MicrocnfPackageResult* self) {
	g_free(self->repo);
	g_free(self->package);
	g_free(self->path);
}

static int microcnf_solv_search_cb(gpointer user_data, Solvable* s, struct s_Repodata* data, struct s_Repokey*, struct s_KeyValue* kv) {
	MicrocnfPackageResult res = {
		.repo = g_strdup(s->repo->name),
		.package = g_strdup(solvable_lookup_str(s, SOLVABLE_NAME)),
		.path = g_strdup(repodata_dir2str(data, kv->id, 0))
	};
	g_array_append_val((GArray*)user_data, res);

	return 0;
}

static GArray* microcnf_solv_search(const gchar* term, GError** err) {
	g_assert(*err == NULL);

	Pool* pool = microcnf_load_repos(err);
	if (!pool)
		return NULL;

	GArray* packages = g_array_new(FALSE, FALSE, sizeof(MicrocnfPackageResult));
	g_array_set_clear_func(packages, (GDestroyNotify)microcnf_package_result_free_inner);

	pool_search(pool, 0, SOLVABLE_FILELIST, term, SEARCH_STRING, microcnf_solv_search_cb, packages);
	pool_free(pool);

	return packages;
}

int main(int argc, char** argv) {
	if (argc < 2)
		return 127;

	g_autoptr(GFile) bin_path = g_file_new_build_filename("/usr/bin/", argv[1], NULL);
	if (g_file_query_exists(bin_path, NULL)) {
		g_print("Absolute path to '%s' is '%s'. Please check your $PATH variable to see whether it contains the mentioned path.\n", argv[1], g_file_peek_path(bin_path));
		return 0;
	}

	g_autoptr(GFile) sbin_path = g_file_new_build_filename("/usr/sbin/", argv[1], NULL);
	if (g_file_query_exists(sbin_path, NULL)) {
		g_print("Absolute path to '%s' is '%s', so running it may require superuser privileges (eg. root).\n", argv[1], g_file_peek_path(bin_path));
		return 0;
	}

	GError* err = NULL;
	g_autoptr(GArray) packages = microcnf_solv_search(argv[1], &err);
	if (!packages) {
		g_printerr("Error: %s\n", err->message);
		g_error_free(err);
		return 127;
	}

	if (packages->len == 0) {
		g_print(" %s: command not found\n", argv[1]);
		return 1; // or should it be 0?
	}
	// TODO: if all available packages share the same name, also use that name
	const gchar* suggested_package = packages->len == 1 ? g_array_index(packages, MicrocnfPackageResult, 0).package : "<selected_package>";
	g_print("\nThe program '%s' can be found in the following packages:\n", argv[1]);
	for (guint i = 0; i < packages->len; i++) {
		MicrocnfPackageResult* res = &g_array_index(packages, MicrocnfPackageResult, i);
		g_print("  * %s [ path: %s/%s, repository: zypp (%s) ]\n", res->package, res->path, argv[1], res->repo);
	}
	g_print("\n\
Try installing with:\n\
    sudo zypper install %s\n\n", suggested_package);

	return 0;
}
